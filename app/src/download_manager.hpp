#pragma once

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include <map>
#include "yt_music_client.hpp"

namespace switube {

struct DownloadedSong {
  std::string id;
  std::string title;
  std::string artist;
  std::string album;
  uint32_t durationMs = 0;
  std::string audioPath;
  std::string imagePath;
  std::string imageUrl;
  uint64_t fileSize = 0;
  uint64_t downloadedAt = 0;
};

class DownloadManager {
 public:
  static DownloadManager& getInstance();

  bool init();

  bool isDownloaded(const std::string& videoId);
  bool isDownloading(const std::string& videoId);
  float getProgress(const std::string& videoId);
  void updateProgress(const std::string& videoId, float fraction);
  std::string getDownloadedAudioPath(const std::string& videoId);
  std::string getDownloadedImagePath(const std::string& videoId);

  bool startDownload(const YtTrack& track, std::function<void(bool)> onComplete = nullptr);
  bool deleteDownload(const std::string& videoId);

  std::vector<DownloadedSong> getDownloadedSongs();
  std::vector<YtTrack> getDownloadedAsTracks();
  size_t getDownloadCount();
  uint64_t getTotalDownloadedBytes();

  size_t addObserver(std::function<void()> observer);
  void removeObserver(size_t id);

 private:
  DownloadManager() = default;
  ~DownloadManager() = default;
  DownloadManager(const DownloadManager&) = delete;
  DownloadManager& operator=(const DownloadManager&) = delete;

  void loadIndex();
  void saveIndex();
  void notifyObservers();

  std::string downloadsDir_ = "/config/Switube/downloads";
  std::string indexPath_ = "/config/Switube/downloads/downloads.json";

  std::recursive_mutex mutex_;
  std::map<std::string, DownloadedSong> downloads_;
  std::map<std::string, float> activeProgress_;

  std::mutex obsMutex_;
  size_t nextObserverId_ = 1;
  std::map<size_t, std::function<void()>> observers_;
};

} // namespace switube
