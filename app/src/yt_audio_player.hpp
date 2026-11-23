#pragma once

#include <string>
#include <vector>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <functional>
#include <cstdint>
#include <memory>

#ifdef __SWITCH__
#include <switch.h>
#endif
#include <pthread.h>

class NXAudioSink;

namespace switube {

class YtAudioPlayer {
 public:
  static YtAudioPlayer& getInstance();

  bool init();
  void shutdown();

  // Inicia la reproducción de una URL directa de audio o ruta local (Opus/WebM/AAC)
  void play(const std::string& streamUrl, uint32_t durationMs = 0, uint64_t contentLength = 0);
  void pause();
  void resume();
  void stop();
  void seekTo(uint32_t positionMs);
  void setVolume(float volume);

  bool isPlaying() const { return isPlaying_.load(); }
  bool isPaused() const { return isPaused_.load(); }
  uint32_t getPositionMs() const { return currentPositionMs_.load(); }
  uint32_t getDurationMs() const { return durationMs_.load(); }

  void setCompletionCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    onComplete_ = std::move(cb);
  }

  void setPlaybackStartCallback(std::function<void()> cb) {
    std::lock_guard<std::mutex> lock(controlMutex_);
    onStart_ = std::move(cb);
  }

 private:
  struct PlayJob {
    std::string url;
    uint32_t durationMs = 0;
    uint64_t contentLength = 0;
    uint64_t generation = 0;
    std::shared_ptr<std::atomic<bool>> stopToken;
  };

  YtAudioPlayer();
  ~YtAudioPlayer();

  void ensureWorkerStarted();
  void stopWorker();
  void workerLoop();
  bool runDecodeSession(const PlayJob& job, std::vector<uint8_t>& pcmOut);

  std::unique_ptr<NXAudioSink> sink_;
  std::atomic<bool> isPlaying_{false};
  std::atomic<bool> isPaused_{false};
  std::atomic<uint32_t> currentPositionMs_{0};
  std::atomic<uint32_t> durationMs_{0};
  std::atomic<uint64_t> currentGeneration_{0};
  std::atomic<float> volume_{0.8f};
  std::atomic<int64_t> seekTargetMs_{-1};

  std::shared_ptr<std::atomic<bool>> currentToken_;
  std::mutex controlMutex_;
  std::mutex sinkMutex_;

  std::mutex jobMutex_;
  std::condition_variable jobCv_;
  PlayJob pendingJob_;
  bool hasPendingJob_ = false;
  bool workerStop_ = false;
  bool workerStarted_ = false;

#ifdef SWITUBE_BACKGROUND_SERVICE
  Thread nxWorkerThread_{};
#else
  pthread_t pthreadWorker_{};
#endif

  std::function<void()> onComplete_;
  std::function<void()> onStart_;
};

} // namespace switube
