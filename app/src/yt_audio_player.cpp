#include "yt_audio_player.hpp"

#include <iostream>
#include <thread>
#include <chrono>
#include <cstring>
#include <algorithm>
#include <curl/curl.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavformat/avio.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

#include "NXAudioSink.h"
#include "NXConsoleLog.h"
#include "thread_helper.hpp"

namespace switube {

namespace {

#ifdef SWITUBE_BACKGROUND_SERVICE
constexpr size_t AVIO_BUFFER_SIZE = 8 * 1024;
#else
constexpr size_t AVIO_BUFFER_SIZE = 32 * 1024;
#endif
constexpr size_t CHUNK_SIZE = 512 * 1024; // 512 KiB per HTTP range

struct BufferReader {
  std::vector<uint8_t> buffer;
  size_t readPos = 0;
  FILE* localFp = nullptr;
  long localFileSize = 0;
  std::atomic<bool> isDownloading{true};
  std::atomic<bool> downloadFailed{false};
  std::shared_ptr<std::atomic<bool>> stopToken;
  std::mutex mutex;

  ~BufferReader() {
    if (localFp) {
      fclose(localFp);
      localFp = nullptr;
    }
  }
};

size_t CurlChunkWrite(void* contents, size_t size, size_t nmemb, void* userp) {
  auto* reader = static_cast<BufferReader*>(userp);
  if (reader->stopToken && reader->stopToken->load()) {
    return 0; // Aborta curl inmediatamente
  }
  size_t total = size * nmemb;
  {
    std::lock_guard<std::mutex> lock(reader->mutex);
    const auto* bytes = static_cast<const uint8_t*>(contents);
    reader->buffer.insert(reader->buffer.end(), bytes, bytes + total);
  }
  return total;
}

int AvioRead(void* opaque, uint8_t* buf, int buf_size) {
  auto* reader = static_cast<BufferReader*>(opaque);
  while (!reader->stopToken->load()) {
    {
      std::lock_guard<std::mutex> lock(reader->mutex);
      if (reader->localFp) {
        size_t n = fread(buf, 1, static_cast<size_t>(buf_size), reader->localFp);
        if (n > 0) {
          reader->readPos += n;
          return static_cast<int>(n);
        }
        return AVERROR_EOF;
      }
      if (reader->readPos < reader->buffer.size()) {
        int avail = static_cast<int>(reader->buffer.size() - reader->readPos);
        int toRead = std::min(buf_size, avail);
        std::memcpy(buf, reader->buffer.data() + reader->readPos, toRead);
        reader->readPos += toRead;
        return toRead;
      }
    }
    if (!reader->isDownloading.load()) {
      return AVERROR_EOF;
    }
    if (reader->downloadFailed.load()) {
      return AVERROR(EIO);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return AVERROR_EXIT;
}

int64_t AvioSeek(void* opaque, int64_t offset, int whence) {
  auto* reader = static_cast<BufferReader*>(opaque);
  std::lock_guard<std::mutex> lock(reader->mutex);

  // IMPORTANTE: Para streams WebM/Opus de YouTube, NUNCA devolver el tamaño estático en AVSEEK_SIZE.
  // Si FFmpeg recibe el tamaño total, asume que es un archivo estático indexado e intenta
  // hacer seek al final del buffer buscando Cues/Index, alcanzando EOF prematuramente y
  // deteniendo la reproducción a los pocos segundos. Devolver -1 garantiza reproducción secuencial fluida.
  if (whence & AVSEEK_SIZE) {
    return -1;
  }

  int baseWhence = whence & ~AVSEEK_FORCE;

  if (reader->localFp) {
    if (baseWhence != SEEK_SET && baseWhence != SEEK_CUR && baseWhence != SEEK_END) {
      return -1;
    }
    if (fseek(reader->localFp, static_cast<long>(offset), baseWhence) != 0) {
      return -1;
    }
    long pos = ftell(reader->localFp);
    if (pos < 0) return -1;
    reader->readPos = static_cast<size_t>(pos);
    return pos;
  }

  int64_t newPos = 0;
  if (baseWhence == SEEK_SET) {
    newPos = offset;
  } else if (baseWhence == SEEK_CUR) {
    newPos = static_cast<int64_t>(reader->readPos) + offset;
  } else if (baseWhence == SEEK_END) {
    newPos = static_cast<int64_t>(reader->buffer.size()) + offset;
  } else {
    return -1;
  }

  if (newPos < 0) return -1;
  if (!reader->isDownloading.load() && static_cast<size_t>(newPos) > reader->buffer.size()) {
    newPos = static_cast<int64_t>(reader->buffer.size());
  }
  reader->readPos = static_cast<size_t>(newPos);
  return reader->readPos;
}

} // namespace

YtAudioPlayer& YtAudioPlayer::getInstance() {
  static YtAudioPlayer instance;
  return instance;
}

YtAudioPlayer::YtAudioPlayer() {
  sink_ = std::make_unique<NXAudioSink>();
  sink_->setParams(48000, 2, 16);
  sink_->setVolume(volume_.load());
}

YtAudioPlayer::~YtAudioPlayer() {
  stop();
}

bool YtAudioPlayer::init() {
  return sink_ && sink_->ok();
}

void YtAudioPlayer::shutdown() {
  stop();
}

void YtAudioPlayer::play(const std::string& streamUrl, uint32_t durationMs, uint64_t contentLength) {
  uint64_t gen = 0;
  std::shared_ptr<std::atomic<bool>> newToken;

  {
    std::lock_guard<std::mutex> lock(controlMutex_);
    if (currentToken_) {
      currentToken_->store(true);
    }

    newToken = std::make_shared<std::atomic<bool>>(false);
    currentToken_ = newToken;

    isPaused_ = false;
    isPlaying_ = true;
    currentPositionMs_ = 0;
    durationMs_ = durationMs;
    gen = ++currentGeneration_;
  }

  {
    std::lock_guard<std::mutex> lock(sinkMutex_);
    if (sink_) {
      sink_->flush();
    }
  }

  nxlog::pushf("YtAudioPlayer: starting playback (gen %llu, bytes: %llu)...",
               static_cast<unsigned long long>(gen), static_cast<unsigned long long>(contentLength));

#ifdef SWITUBE_BACKGROUND_SERVICE
  constexpr size_t kDecoderStackSize = 128 * 1024;
#else
  constexpr size_t kDecoderStackSize = 1024 * 1024;
#endif
  launchWorkerWithStack([this, streamUrl, durationMs, contentLength, gen, newToken]() {
    audioWorker(streamUrl, durationMs, contentLength, gen, newToken);
  }, kDecoderStackSize);
}

void YtAudioPlayer::pause() {
  isPaused_ = true;
  isPlaying_ = false;
}

void YtAudioPlayer::resume() {
  isPaused_ = false;
  isPlaying_ = true;
}

void YtAudioPlayer::stop() {
  {
    std::lock_guard<std::mutex> lock(controlMutex_);
    if (currentToken_) {
      currentToken_->store(true);
    }
    isPlaying_ = false;
    isPaused_ = false;
    currentPositionMs_ = 0;
  }
  {
    std::lock_guard<std::mutex> lock(sinkMutex_);
    if (sink_) {
      sink_->flush();
    }
  }
}

void YtAudioPlayer::seekTo(uint32_t positionMs) {
  currentPositionMs_ = positionMs;
  seekTargetMs_.store(static_cast<int64_t>(positionMs));
}

void YtAudioPlayer::setVolume(float volume) {
  volume_ = volume;
  std::lock_guard<std::mutex> lock(sinkMutex_);
  if (sink_) {
    sink_->setVolume(volume);
  }
}

void YtAudioPlayer::audioWorker(std::string url, uint32_t durMs, uint64_t contentLength, uint64_t generation,
                                std::shared_ptr<std::atomic<bool>> stopToken) {
  std::lock_guard<std::mutex> workerLock(workerMutex_);
  if (stopToken->load() || generation != currentGeneration_.load()) {
    return;
  }

  bool isLocal = (url.rfind("/", 0) == 0 || url.rfind("sdmc:/", 0) == 0);
  AVFormatContext* fmtCtx = nullptr;
  AVIOContext* avioCtx = nullptr;
  auto reader = std::make_shared<BufferReader>();
  reader->stopToken = stopToken;

  if (isLocal) {
    nxlog::pushf("YtAudioPlayer: playing local file '%s'", url.c_str());
    FILE* fp = fopen(url.c_str(), "rb");
    if (!fp && url.rfind("/", 0) == 0) {
      std::string sdmcUrl = "sdmc:" + url;
      fp = fopen(sdmcUrl.c_str(), "rb");
    }
    if (!fp) {
      nxlog::pushf("YtAudioPlayer error: cannot open local file: %s", url.c_str());
      return;
    }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (fsize <= 0) {
      fclose(fp);
      nxlog::pushf("YtAudioPlayer error: empty local file: %s", url.c_str());
      return;
    }
#ifdef SWITUBE_BACKGROUND_SERVICE
    reader->localFp = fp;
    reader->localFileSize = fsize;
    reader->isDownloading = false;
#else
    reader->buffer.resize(static_cast<size_t>(fsize));
    size_t readBytes = fread(reader->buffer.data(), 1, static_cast<size_t>(fsize), fp);
    fclose(fp);
    if (readBytes == 0) {
      nxlog::pushf("YtAudioPlayer error: fread failed on local file: %s", url.c_str());
      return;
    }
    reader->buffer.resize(readBytes);
    reader->isDownloading = false;
#endif
  } else {
  // Pre-reservar memoria para evitar realocaciones continuas durante la reproducción
  {
    std::lock_guard<std::mutex> lock(reader->mutex);
    size_t reserveBytes = (contentLength > 0) ? (contentLength + 64 * 1024) : (8 * 1024 * 1024);
    reader->buffer.reserve(reserveBytes);
  }

  // Descarga concurrente por bloques finitos (burst a velocidad máxima sin estrangulamiento de YouTube)
  launchWorkerWithStack([reader, url, stopToken, contentLength]() {
    constexpr size_t CHUNK_SIZE = 512 * 1024; // 512 KiB por fragmento
    size_t offset = 0;
    int consecutiveFails = 0;

    while (!stopToken->load()) {
      if (contentLength > 0 && offset >= contentLength) {
        break;
      }

      CURL* curl = curl_easy_init();
      if (!curl) {
        reader->downloadFailed = true;
        break;
      }

      size_t endByte = offset + CHUNK_SIZE - 1;
      if (contentLength > 0 && endByte >= contentLength) {
        endByte = contentLength - 1;
      }

      std::string rangeHdr = "Range: bytes=" + std::to_string(offset) + "-" + std::to_string(endByte);
      struct curl_slist* headers = nullptr;
      headers = curl_slist_append(headers, rangeHdr.c_str());
      headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 Version/26.0 Safari/605.1.15");

      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
      curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
      curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
      curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1000L);
      curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 5L);
      curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlChunkWrite);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, reader.get());
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

      size_t prevSize = 0;
      {
        std::lock_guard<std::mutex> lock(reader->mutex);
        prevSize = reader->buffer.size();
      }

      CURLcode res = curl_easy_perform(curl);
      long httpCode = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);

      if (stopToken->load()) {
        break;
      }

      size_t newSize = 0;
      {
        std::lock_guard<std::mutex> lock(reader->mutex);
        newSize = reader->buffer.size();
      }
      size_t received = (newSize >= prevSize) ? (newSize - prevSize) : 0;
      offset = newSize;

      if (httpCode == 416) {
        nxlog::pushf("YtAudioPlayer: HTTP 416 (end of stream at %zu bytes)", offset);
        break;
      }

      if (res == CURLE_OK) {
        consecutiveFails = 0;
        if (contentLength > 0 && offset >= contentLength) {
          break;
        }
        if (received == 0) {
          break;
        }
        if (httpCode == 200) {
          break;
        }
      } else {
        if (res == CURLE_WRITE_ERROR && stopToken->load()) {
          break;
        }
        consecutiveFails++;
        nxlog::pushf("YtAudioPlayer: chunk interrupted (%s, HTTP %ld, total %zu, retry %d/5)",
                     curl_easy_strerror(res), httpCode, offset, consecutiveFails);
        if (consecutiveFails >= 5) {
          nxlog::push("YtAudioPlayer: max retries reached, aborting stream download");
          reader->downloadFailed = true;
          break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
      }
    }
    reader->isDownloading = false;
    nxlog::pushf("YtAudioPlayer: stream download finished (%zu total bytes)", reader->buffer.size());
  }, 256 * 1024);

  // Pre-buffer ultrarrápido de 32 KiB: inicia en menos de 0.1s sin espera
  size_t targetPrebuffer = 32 * 1024;
  if (contentLength > 0 && contentLength < targetPrebuffer) {
    targetPrebuffer = contentLength;
  }

  while (!stopToken->load() && generation == currentGeneration_.load()) {
    {
      std::lock_guard<std::mutex> lock(reader->mutex);
      if (reader->buffer.size() >= targetPrebuffer || !reader->isDownloading.load()) {
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }

  if (stopToken->load()) return;
  }

  if (stopToken->load()) return;

  uint8_t* avioBuffer = static_cast<uint8_t*>(av_malloc(AVIO_BUFFER_SIZE));
  if (!avioBuffer) {
    nxlog::push("YtAudioPlayer error: av_malloc(AVIO_BUFFER_SIZE) failed");
    return;
  }
  avioCtx = avio_alloc_context(avioBuffer, AVIO_BUFFER_SIZE, 0, reader.get(), AvioRead, nullptr, AvioSeek);
  if (!avioCtx) {
    av_free(avioBuffer);
    nxlog::push("YtAudioPlayer error: avio_alloc_context failed");
    return;
  }

  fmtCtx = avformat_alloc_context();
  if (!fmtCtx) {
    if (avioCtx->buffer) av_freep(&avioCtx->buffer);
    avio_context_free(&avioCtx);
    nxlog::push("YtAudioPlayer error: avformat_alloc_context failed");
    return;
  }
  fmtCtx->pb = avioCtx;
  fmtCtx->flags |= AVFMT_FLAG_CUSTOM_IO;

  if (avformat_open_input(&fmtCtx, nullptr, nullptr, nullptr) < 0) {
    nxlog::push("YtAudioPlayer error: avformat_open_input failed");
    if (fmtCtx) {
      fmtCtx->pb = nullptr;
      avformat_close_input(&fmtCtx);
    }
    if (avioCtx) {
      if (avioCtx->buffer) av_freep(&avioCtx->buffer);
      avio_context_free(&avioCtx);
    }
    return;
  }

  if (stopToken->load() || generation != currentGeneration_.load()) {
    if (fmtCtx) {
      fmtCtx->pb = nullptr;
      avformat_close_input(&fmtCtx);
    }
    if (avioCtx) {
      if (avioCtx->buffer) av_freep(&avioCtx->buffer);
      avio_context_free(&avioCtx);
    }
    return;
  }

  if (avformat_find_stream_info(fmtCtx, nullptr) < 0) {
    nxlog::push("YtAudioPlayer error: avformat_find_stream_info failed");
    if (fmtCtx) {
      fmtCtx->pb = nullptr;
      avformat_close_input(&fmtCtx);
    }
    if (avioCtx) {
      if (avioCtx->buffer) av_freep(&avioCtx->buffer);
      avio_context_free(&avioCtx);
    }
    return;
  }

  const AVCodec* codec = nullptr;
  int streamIdx = av_find_best_stream(fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
  if (streamIdx < 0 || !codec) {
    nxlog::push("YtAudioPlayer error: no audio stream or codec found");
    if (fmtCtx) {
      fmtCtx->pb = nullptr;
      avformat_close_input(&fmtCtx);
    }
    if (avioCtx) {
      if (avioCtx->buffer) av_freep(&avioCtx->buffer);
      avio_context_free(&avioCtx);
    }
    return;
  }

  AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
  if (!codecCtx) {
    nxlog::push("YtAudioPlayer error: avcodec_alloc_context3 failed (OOM)");
    if (fmtCtx) {
      fmtCtx->pb = nullptr;
      avformat_close_input(&fmtCtx);
    }
    if (avioCtx) {
      if (avioCtx->buffer) av_freep(&avioCtx->buffer);
      avio_context_free(&avioCtx);
    }
    return;
  }
  if (avcodec_parameters_to_context(codecCtx, fmtCtx->streams[streamIdx]->codecpar) < 0 ||
      avcodec_open2(codecCtx, codec, nullptr) < 0) {
    nxlog::push("YtAudioPlayer error: avcodec_open2 failed");
    avcodec_free_context(&codecCtx);
    if (fmtCtx) {
      fmtCtx->pb = nullptr;
      avformat_close_input(&fmtCtx);
    }
    if (avioCtx) {
      if (avioCtx->buffer) av_freep(&avioCtx->buffer);
      avio_context_free(&avioCtx);
    }
    return;
  }

  // Configurar SwrContext para remuestrear a 48000 Hz, S16 estéreo
  SwrContext* swrCtx = nullptr;
  AVChannelLayout outChLayout = AV_CHANNEL_LAYOUT_STEREO;
  AVChannelLayout inChLayout = {};
  if (codecCtx->ch_layout.order != AV_CHANNEL_ORDER_UNSPEC && codecCtx->ch_layout.nb_channels > 0) {
    av_channel_layout_copy(&inChLayout, &codecCtx->ch_layout);
  } else {
    int channels = codecCtx->ch_layout.nb_channels > 0 ? codecCtx->ch_layout.nb_channels : 2;
    av_channel_layout_default(&inChLayout, channels);
  }
  int swrAllocRet = swr_alloc_set_opts2(&swrCtx,
                                        &outChLayout,
                                        AV_SAMPLE_FMT_S16,
                                        48000,
                                        &inChLayout,
                                        codecCtx->sample_fmt,
                                        codecCtx->sample_rate,
                                        0, nullptr);
  av_channel_layout_uninit(&inChLayout);
  if (swrAllocRet < 0 || !swrCtx || swr_init(swrCtx) < 0) {
    nxlog::push("YtAudioPlayer error: swr_init failed");
    if (swrCtx) swr_free(&swrCtx);
    avcodec_free_context(&codecCtx);
    if (fmtCtx) {
      fmtCtx->pb = nullptr;
      avformat_close_input(&fmtCtx);
    }
    if (avioCtx) {
      if (avioCtx->buffer) av_freep(&avioCtx->buffer);
      avio_context_free(&avioCtx);
    }
    return;
  }

  AVPacket* pkt = av_packet_alloc();
  AVFrame* frame = av_frame_alloc();
  if (!pkt || !frame) {
    nxlog::push("YtAudioPlayer error: av_packet_alloc/av_frame_alloc failed");
    if (frame) av_frame_free(&frame);
    if (pkt) av_packet_free(&pkt);
    swr_free(&swrCtx);
    avcodec_free_context(&codecCtx);
    if (fmtCtx) {
      fmtCtx->pb = nullptr;
      avformat_close_input(&fmtCtx);
    }
    if (avioCtx) {
      if (avioCtx->buffer) av_freep(&avioCtx->buffer);
      avio_context_free(&avioCtx);
    }
    return;
  }
  AVRational tb = fmtCtx->streams[streamIdx]->time_base;

  constexpr int MAX_OUT_SAMPLES = 8192;
  std::vector<uint8_t> pcmOut(MAX_OUT_SAMPLES * 2 * sizeof(int16_t));

  nxlog::pushf("YtAudioPlayer: audio decoding started (%d Hz -> 48000 Hz, codec: %s)",
               codecCtx->sample_rate, codec->name);

  bool completed = false;
  bool firstFrameNotified = false;

  while (!stopToken->load()) {
    int64_t seekMs = seekTargetMs_.exchange(-1);
    if (seekMs >= 0) {
      int64_t targetPts = static_cast<int64_t>((static_cast<double>(seekMs) / 1000.0) / av_q2d(tb));
      int retSeek = av_seek_frame(fmtCtx, streamIdx, targetPts, AVSEEK_FLAG_BACKWARD);
      if (retSeek >= 0) {
        avcodec_flush_buffers(codecCtx);
        {
          std::lock_guard<std::mutex> lock(sinkMutex_);
          if (sink_) sink_->flush();
        }
        currentPositionMs_.store(static_cast<uint32_t>(seekMs));
        nxlog::pushf("YtAudioPlayer: seeked to %lld ms", seekMs);
      } else {
        nxlog::pushf("YtAudioPlayer: av_seek_frame failed (%d) for %lld ms", retSeek, seekMs);
      }
    }

    if (isPaused_.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
      continue;
    }

    int ret = av_read_frame(fmtCtx, pkt);
    if (ret < 0) {
      if (ret == AVERROR_EOF) {
        completed = true;
      }
      break;
    }

    if (pkt->stream_index == streamIdx) {
      if (avcodec_send_packet(codecCtx, pkt) == 0) {
        while (avcodec_receive_frame(codecCtx, frame) == 0) {
          if (stopToken->load()) break;

          uint8_t* outPtr = pcmOut.data();
          const uint8_t** inData = frame->extended_data
                                       ? const_cast<const uint8_t**>(frame->extended_data)
                                       : const_cast<const uint8_t**>(frame->data);
          int outSamples = swr_convert(swrCtx,
                                       &outPtr,
                                       MAX_OUT_SAMPLES,
                                       inData,
                                       frame->nb_samples);
          if (outSamples > 0 && !stopToken->load()) {
            if (!firstFrameNotified) {
              firstFrameNotified = true;
              std::function<void()> startCb;
              {
                std::lock_guard<std::mutex> lock(controlMutex_);
                startCb = onStart_;
              }
              if (startCb && !stopToken->load()) {
                startCb();
              }
            }
            std::lock_guard<std::mutex> lock(sinkMutex_);
            if (!stopToken->load() && sink_) {
              size_t bytes = static_cast<size_t>(outSamples) * 2 * sizeof(int16_t);
              sink_->feedPCMFrames(pcmOut.data(), bytes);
            }
          }

          if (frame->pts != AV_NOPTS_VALUE) {
            double secs = static_cast<double>(frame->pts) * av_q2d(tb);
            currentPositionMs_ = static_cast<uint32_t>(secs * 1000.0);
          }
        }
      }
    }
    av_packet_unref(pkt);
  }

  if (frame) av_frame_free(&frame);
  if (pkt) av_packet_free(&pkt);
  if (swrCtx) swr_free(&swrCtx);
  if (codecCtx) avcodec_free_context(&codecCtx);
  if (fmtCtx) {
    fmtCtx->pb = nullptr;
    avformat_close_input(&fmtCtx);
  }
  if (avioCtx) {
    if (avioCtx->buffer) av_freep(&avioCtx->buffer);
    avio_context_free(&avioCtx);
  }

  nxlog::pushf("YtAudioPlayer: playback loop ended (completed: %s, stopped: %s)",
               completed ? "YES" : "NO", stopToken->load() ? "YES" : "NO");

  bool isActualEnd = (durMs == 0 || currentPositionMs_.load() >= static_cast<uint32_t>(durMs * 0.80f));
  if (completed && isActualEnd && !stopToken->load()) {
    std::function<void()> cb;
    {
      std::lock_guard<std::mutex> lock(controlMutex_);
      cb = onComplete_;
    }
    if (cb) cb();
  } else if (!isActualEnd && completed) {
    nxlog::pushf("YtAudioPlayer: playback ended prematurely at %u ms (expected %u ms), not auto-advancing",
                 currentPositionMs_.load(), durMs);
  }
}

} // namespace switube
