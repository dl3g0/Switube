#include "download_manager.hpp"

#include <curl/curl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <ctime>
#include <chrono>
#include <fstream>
#include <nlohmann/json.hpp>
#include <borealis.hpp>

#include "NXConsoleLog.h"
#include "thread_helper.hpp"
#include "yt_engine.hpp"

using json = nlohmann::json;

namespace switube {

namespace {

bool fileExists(const std::string& path) {
  if (path.empty()) return false;
  struct stat st{};
  return stat(path.c_str(), &st) == 0 && st.st_size > 0;
}

uint64_t getFileLength(const std::string& path) {
  if (path.empty()) return 0;
  struct stat st{};
  if (stat(path.c_str(), &st) == 0) {
    return static_cast<uint64_t>(st.st_size);
  }
  return 0;
}

struct DownloadWriteCtx {
  std::vector<uint8_t>* vec = nullptr;
  DownloadManager* mgr = nullptr;
  std::string videoId;
  uint64_t expectedTotal = 0;
  std::chrono::steady_clock::time_point lastUpdate{};
};

static size_t CurlWriteDownload(void* contents, size_t size, size_t nmemb, void* userp) {
  auto* ctx = static_cast<DownloadWriteCtx*>(userp);
  if (!ctx || !ctx->vec) return 0;
  size_t total = size * nmemb;
  const auto* bytes = static_cast<const uint8_t*>(contents);
  ctx->vec->insert(ctx->vec->end(), bytes, bytes + total);

  if (ctx->mgr && ctx->expectedTotal > 0) {
    float frac = static_cast<float>(ctx->vec->size()) / static_cast<float>(ctx->expectedTotal);
    if (frac > 0.99f) frac = 0.99f;
    auto now = std::chrono::steady_clock::now();
    if (now - ctx->lastUpdate > std::chrono::milliseconds(200)) {
      ctx->lastUpdate = now;
      ctx->mgr->updateProgress(ctx->videoId, frac);
    }
  }
  return total;
}

static size_t CurlWriteVector(void* contents, size_t size, size_t nmemb, void* userp) {
  auto* vec = static_cast<std::vector<uint8_t>*>(userp);
  if (!vec) return 0;
  size_t total = size * nmemb;
  const auto* bytes = static_cast<const uint8_t*>(contents);
  vec->insert(vec->end(), bytes, bytes + total);
  return total;
}

} // namespace

DownloadManager& DownloadManager::getInstance() {
  static DownloadManager instance;
  return instance;
}

bool DownloadManager::init() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  mkdir("/config", 0755);
  mkdir("/config/Switube", 0755);
  mkdir("/config/Switube/downloads", 0755);
  mkdir("sdmc:/config", 0755);
  mkdir("sdmc:/config/Switube", 0755);
  mkdir("sdmc:/config/Switube/downloads", 0755);

  loadIndex();
  nxlog::pushf("DownloadManager initialized: %zu downloaded songs", downloads_.size());
  return true;
}

void DownloadManager::loadIndex() {
  std::ifstream file(indexPath_);
  if (!file.is_open()) {
    file.open("sdmc:" + indexPath_);
  }
  if (!file.is_open()) return;

  try {
    json j;
    file >> j;
    file.close();

    bool needsResave = false;
    downloads_.clear();

    if (j.is_array()) {
      for (const auto& item : j) {
        DownloadedSong song;
        song.id = item.value("id", "");
        song.title = item.value("title", "");
        song.artist = item.value("artist", "");
        song.album = item.value("album", "");
        song.durationMs = item.value("durationMs", 0u);
        song.audioPath = item.value("audioPath", "");
        song.imagePath = item.value("imagePath", "");
        song.imageUrl = item.value("imageUrl", "");
        song.fileSize = item.value("fileSize", uint64_t{0});
        song.downloadedAt = item.value("downloadedAt", uint64_t{0});

        if (song.id.empty() || song.audioPath.empty()) continue;

        // Verify that the file actually exists
        if (fileExists(song.audioPath) || fileExists("sdmc:" + song.audioPath)) {
          if (song.fileSize == 0) {
            song.fileSize = getFileLength(song.audioPath);
            if (song.fileSize == 0) song.fileSize = getFileLength("sdmc:" + song.audioPath);
          }
          downloads_[song.id] = song;
        } else {
          nxlog::pushf("DownloadManager: local file missing for '%s', dropping from index", song.id.c_str());
          needsResave = true;
        }
      }
    }

    if (needsResave) {
      saveIndex();
    }
  } catch (const std::exception& e) {
    nxlog::pushf("DownloadManager: error reading index: %s", e.what());
  }
}

void DownloadManager::saveIndex() {
  try {
    json arr = json::array();
    for (const auto& [_, song] : downloads_) {
      arr.push_back({
          {"id", song.id},
          {"title", song.title},
          {"artist", song.artist},
          {"album", song.album},
          {"durationMs", song.durationMs},
          {"audioPath", song.audioPath},
          {"imagePath", song.imagePath},
          {"imageUrl", song.imageUrl},
          {"fileSize", song.fileSize},
          {"downloadedAt", song.downloadedAt}
      });
    }

    std::ofstream file(indexPath_, std::ios::trunc);
    if (!file.is_open()) {
      file.open("sdmc:" + indexPath_, std::ios::trunc);
    }
    if (file.is_open()) {
      file << arr.dump(2);
      file.close();
    }
  } catch (const std::exception& e) {
    nxlog::pushf("DownloadManager: error saving index: %s", e.what());
  }
}

bool DownloadManager::isDownloaded(const std::string& videoId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto it = downloads_.find(videoId);
  if (it == downloads_.end()) return false;
  return fileExists(it->second.audioPath) || fileExists("sdmc:" + it->second.audioPath);
}

bool DownloadManager::isDownloading(const std::string& videoId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return activeProgress_.find(videoId) != activeProgress_.end();
}

float DownloadManager::getProgress(const std::string& videoId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto it = activeProgress_.find(videoId);
  if (it != activeProgress_.end()) {
    return it->second;
  }
  return isDownloaded(videoId) ? 1.0f : 0.0f;
}

void DownloadManager::updateProgress(const std::string& videoId, float fraction) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  activeProgress_[videoId] = fraction;
}

std::string DownloadManager::getDownloadedAudioPath(const std::string& videoId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto it = downloads_.find(videoId);
  if (it != downloads_.end()) {
    if (fileExists(it->second.audioPath)) return it->second.audioPath;
    std::string sdmcPath = "sdmc:" + it->second.audioPath;
    if (fileExists(sdmcPath)) return sdmcPath;
  }
  return "";
}

std::string DownloadManager::getDownloadedImagePath(const std::string& videoId) {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  auto it = downloads_.find(videoId);
  if (it != downloads_.end()) {
    if (fileExists(it->second.imagePath)) return it->second.imagePath;
    std::string sdmcPath = "sdmc:" + it->second.imagePath;
    if (fileExists(sdmcPath)) return sdmcPath;
  }
  return "";
}

std::vector<DownloadedSong> DownloadManager::getDownloadedSongs() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::vector<DownloadedSong> list;
  list.reserve(downloads_.size());
  for (const auto& [_, song] : downloads_) {
    list.push_back(song);
  }
  return list;
}

std::vector<YtTrack> DownloadManager::getDownloadedAsTracks() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  std::vector<YtTrack> list;
  list.reserve(downloads_.size());
  for (const auto& [_, song] : downloads_) {
    YtTrack t;
    t.id = song.id;
    t.title = song.title;
    t.artist = song.artist;
    t.album = song.album;
    t.durationMs = song.durationMs;
    t.imageUrl = fileExists(song.imagePath) ? song.imagePath : song.imageUrl;
    list.push_back(t);
  }
  return list;
}

size_t DownloadManager::getDownloadCount() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  return downloads_.size();
}

uint64_t DownloadManager::getTotalDownloadedBytes() {
  std::lock_guard<std::recursive_mutex> lock(mutex_);
  uint64_t total = 0;
  for (const auto& [_, s] : downloads_) {
    total += s.fileSize;
  }
  return total;
}

bool DownloadManager::deleteDownload(const std::string& videoId) {
  if (videoId.empty()) return false;
  std::string idCopy = videoId;

  // Si la canción que se va a eliminar se está reproduciendo, detenerla antes de borrar el archivo
  YtEngine::getInstance().onTrackDeleted(idCopy);

  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = downloads_.find(idCopy);
    if (it == downloads_.end()) return false;

    const auto song = it->second;
    unlink(song.audioPath.c_str());
    unlink(("sdmc:" + song.audioPath).c_str());
    if (!song.imagePath.empty()) {
      unlink(song.imagePath.c_str());
      unlink(("sdmc:" + song.imagePath).c_str());
    }

    downloads_.erase(it);
    activeProgress_.erase(idCopy);
    saveIndex();
  }
  notifyObservers();
  nxlog::pushf("DownloadManager: deleted download '%s'", idCopy.c_str());
  return true;
}

bool DownloadManager::startDownload(const YtTrack& track, std::function<void(bool)> onComplete) {
  if (track.id.empty()) return false;

  auto safeComplete = [onComplete](bool success) {
    if (onComplete) {
      brls::sync([onComplete, success]() {
        onComplete(success);
      });
    }
  };

  {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (isDownloaded(track.id)) {
      nxlog::pushf("DownloadManager: '%s' is already downloaded", track.id.c_str());
      safeComplete(true);
      return false;
    }
    if (isDownloading(track.id)) {
      nxlog::pushf("DownloadManager: '%s' is already downloading", track.id.c_str());
      return false;
    }

    activeProgress_[track.id] = 0.01f;
  }
  notifyObservers();

  launchWorkerWithStack([this, track, safeComplete]() {
    try {
      nxlog::pushf("DownloadManager: starting download for '%s' ('%s')...",
                   track.id.c_str(), track.title.c_str());

      // 1. Resolve Audio Stream
      YtMusicClient client;
      AudioStreamUrl stream;
      bool ok = client.resolveAudioStream(track.id, stream);
      if (!ok || stream.url.empty()) {
        nxlog::pushf("DownloadManager: failed to resolve audio stream for '%s'", track.id.c_str());
        {
          std::lock_guard<std::recursive_mutex> lock(mutex_);
          activeProgress_.erase(track.id);
        }
        notifyObservers();
        safeComplete(false);
        return;
      }

      std::string ext = (stream.codec == "opus") ? ".opus" : ".m4a";
      std::string finalAudioPath = downloadsDir_ + "/" + track.id + ext;
      std::string finalImagePath = downloadsDir_ + "/" + track.id + ".jpg";

      // 2. Fetch Audio into RAM buffer in 512 KiB chunks (proven pattern on Switch)
      std::vector<uint8_t> audioData;
      if (stream.contentLength > 0) {
        audioData.reserve(stream.contentLength + 32 * 1024);
      } else {
        audioData.reserve(3 * 1024 * 1024);
      }

      CURL* curl = curl_easy_init();
      if (!curl) {
        nxlog::push("DownloadManager: curl_easy_init failed");
        {
          std::lock_guard<std::recursive_mutex> lock(mutex_);
          activeProgress_.erase(track.id);
        }
        notifyObservers();
        safeComplete(false);
        return;
      }

      DownloadWriteCtx writeCtx;
      writeCtx.vec = &audioData;
      writeCtx.mgr = this;
      writeCtx.videoId = track.id;
      writeCtx.expectedTotal = stream.contentLength > 0 ? stream.contentLength : (3 * 1024 * 1024);
      writeCtx.lastUpdate = std::chrono::steady_clock::now();

      curl_easy_setopt(curl, CURLOPT_URL, stream.url.c_str());
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteDownload);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writeCtx);
      curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
      curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
      curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
      curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
      curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, 102400L);
      curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
      curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
      curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);

      const uint64_t totalContentLen = stream.contentLength;
      const uint64_t CHUNK_SIZE = 512 * 1024;
      bool downloadFailed = false;

      if (totalContentLen > 0) {
        uint64_t downloaded = 0;
        int retries = 0;
        while (downloaded < totalContentLen) {
          uint64_t endByte = downloaded + CHUNK_SIZE - 1;
          if (endByte >= totalContentLen) {
            endByte = totalContentLen - 1;
          }

          char rangeHeader[128];
          std::snprintf(rangeHeader, sizeof(rangeHeader), "Range: bytes=%llu-%llu",
                        static_cast<unsigned long long>(downloaded),
                        static_cast<unsigned long long>(endByte));

          struct curl_slist* chunkHeaders = nullptr;
          chunkHeaders = curl_slist_append(chunkHeaders, "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 Version/26.0 Safari/605.1.15");
          chunkHeaders = curl_slist_append(chunkHeaders, "Connection: keep-alive");
          chunkHeaders = curl_slist_append(chunkHeaders, rangeHeader);

          curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunkHeaders);
          curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 25000L);

          size_t beforeBytes = audioData.size();
          CURLcode res = curl_easy_perform(curl);
          long httpCode = 0;
          curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
          curl_slist_free_all(chunkHeaders);

          size_t afterBytes = audioData.size();
          size_t received = (afterBytes > beforeBytes) ? (afterBytes - beforeBytes) : 0;

          if (res == CURLE_OK && (httpCode == 200 || httpCode == 206) && received > 0) {
            downloaded = afterBytes;
            retries = 0;
            updateProgress(track.id, static_cast<float>(downloaded) / static_cast<float>(totalContentLen));
            if (httpCode == 200 && downloaded >= totalContentLen) {
              break;
            }
          } else {
            retries++;
            nxlog::pushf("DownloadManager: chunk failed at %llu/%llu (curl=%d, HTTP=%ld, retry=%d)",
                         static_cast<unsigned long long>(downloaded),
                         static_cast<unsigned long long>(totalContentLen),
                         res, httpCode, retries);
            if (retries >= 4) {
              downloadFailed = true;
              break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(250 * retries));
          }
        }
      } else {
        struct curl_slist* headers = nullptr;
        headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 Version/26.0 Safari/605.1.15");
        curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 15L);

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
        curl_slist_free_all(headers);

        if (res != CURLE_OK || (httpCode != 200 && httpCode != 206) || audioData.empty()) {
          nxlog::pushf("DownloadManager: curl failed (code %d, HTTP %ld, size %zu)", res, httpCode, audioData.size());
          downloadFailed = true;
        }
      }

      curl_easy_cleanup(curl);

      if (downloadFailed || audioData.empty()) {
        nxlog::pushf("DownloadManager: audio download failed for '%s' (bytes: %zu)",
                     track.id.c_str(), audioData.size());
        {
          std::lock_guard<std::recursive_mutex> lock(mutex_);
          activeProgress_.erase(track.id);
        }
        notifyObservers();
        safeComplete(false);
        return;
      }

      // Write audio to SD card atomically in a single flush
      unlink(finalAudioPath.c_str());
      unlink(("sdmc:" + finalAudioPath).c_str());

      FILE* fp = fopen(finalAudioPath.c_str(), "wb");
      if (!fp) {
        fp = fopen(("sdmc:" + finalAudioPath).c_str(), "wb");
      }
      if (!fp) {
        nxlog::pushf("DownloadManager: cannot create destination file '%s'", finalAudioPath.c_str());
        {
          std::lock_guard<std::recursive_mutex> lock(mutex_);
          activeProgress_.erase(track.id);
        }
        notifyObservers();
        safeComplete(false);
        return;
      }

      size_t written = fwrite(audioData.data(), 1, audioData.size(), fp);
      fclose(fp);

      if (written != audioData.size()) {
        nxlog::pushf("DownloadManager: incomplete file write (%zu of %zu)", written, audioData.size());
        unlink(finalAudioPath.c_str());
        unlink(("sdmc:" + finalAudioPath).c_str());
        {
          std::lock_guard<std::recursive_mutex> lock(mutex_);
          activeProgress_.erase(track.id);
        }
        notifyObservers();
        safeComplete(false);
        return;
      }

      // 3. Download Thumbnail into memory buffer then write to disk
      if (!track.imageUrl.empty()) {
        std::vector<uint8_t> imgData;
        imgData.reserve(128 * 1024);
        CURL* imgCurl = curl_easy_init();
        if (imgCurl) {
          curl_easy_setopt(imgCurl, CURLOPT_URL, track.imageUrl.c_str());
          curl_easy_setopt(imgCurl, CURLOPT_WRITEFUNCTION, CurlWriteVector);
          curl_easy_setopt(imgCurl, CURLOPT_WRITEDATA, &imgData);
          curl_easy_setopt(imgCurl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
          curl_easy_setopt(imgCurl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
          curl_easy_setopt(imgCurl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
          curl_easy_setopt(imgCurl, CURLOPT_NOSIGNAL, 1L);
          curl_easy_setopt(imgCurl, CURLOPT_FOLLOWLOCATION, 1L);
          curl_easy_setopt(imgCurl, CURLOPT_SSL_VERIFYPEER, 0L);
          curl_easy_setopt(imgCurl, CURLOPT_SSL_VERIFYHOST, 0L);
          curl_easy_setopt(imgCurl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
          curl_easy_setopt(imgCurl, CURLOPT_TIMEOUT_MS, 15000L);
          CURLcode imgRes = curl_easy_perform(imgCurl);
          curl_easy_cleanup(imgCurl);

          if (imgRes == CURLE_OK && !imgData.empty()) {
            unlink(finalImagePath.c_str());
            unlink(("sdmc:" + finalImagePath).c_str());
            FILE* imgFp = fopen(finalImagePath.c_str(), "wb");
            if (!imgFp) imgFp = fopen(("sdmc:" + finalImagePath).c_str(), "wb");
            if (imgFp) {
              fwrite(imgData.data(), 1, imgData.size(), imgFp);
              fclose(imgFp);
            }
          }
        }
      }

      // 4. Save metadata
      uint64_t fileSize = getFileLength(finalAudioPath);
      if (fileSize == 0) fileSize = getFileLength("sdmc:" + finalAudioPath);

      DownloadedSong ds;
      ds.id = track.id;
      ds.title = track.title;
      ds.artist = track.artist;
      ds.album = track.album;
      ds.durationMs = track.durationMs > 0 ? track.durationMs : stream.approxDurationMs;
      ds.audioPath = finalAudioPath;
      ds.imagePath = fileExists(finalImagePath) ? finalImagePath : (fileExists("sdmc:" + finalImagePath) ? "sdmc:" + finalImagePath : "");
      ds.imageUrl = track.imageUrl;
      ds.fileSize = fileSize;
      ds.downloadedAt = static_cast<uint64_t>(time(nullptr));

      {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        downloads_[track.id] = ds;
        activeProgress_.erase(track.id);
        saveIndex();
      }

      nxlog::pushf("DownloadManager: SUCCESS downloading '%s' (%llu bytes)",
                   track.title.c_str(), static_cast<unsigned long long>(fileSize));
      notifyObservers();
      safeComplete(true);

    } catch (const std::exception& e) {
      nxlog::pushf("DownloadManager exception: %s", e.what());
      {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        activeProgress_.erase(track.id);
      }
      notifyObservers();
      safeComplete(false);
    } catch (...) {
      nxlog::push("DownloadManager unknown fatal exception");
      {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        activeProgress_.erase(track.id);
      }
      notifyObservers();
      safeComplete(false);
    }
  }, 1024 * 1024);

  return true;
}

size_t DownloadManager::addObserver(std::function<void()> observer) {
  std::lock_guard<std::mutex> lock(obsMutex_);
  size_t id = nextObserverId_++;
  observers_[id] = std::move(observer);
  return id;
}

void DownloadManager::removeObserver(size_t id) {
  std::lock_guard<std::mutex> lock(obsMutex_);
  observers_.erase(id);
}

void DownloadManager::notifyObservers() {
  std::vector<std::function<void()>> callbacks;
  {
    std::lock_guard<std::mutex> lock(obsMutex_);
    callbacks.reserve(observers_.size());
    for (const auto& [_, cb] : observers_) {
      if (cb) callbacks.push_back(cb);
    }
  }

  // Despachar siempre en el hilo de UI de Borealis para evitar race conditions y crasheos
  brls::sync([cbs = std::move(callbacks)]() {
    for (const auto& cb : cbs) {
      try {
        if (cb) cb();
      } catch (...) {}
    }
  });
}

} // namespace switube
