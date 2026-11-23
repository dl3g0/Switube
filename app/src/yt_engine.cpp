#include "yt_engine.hpp"

#include <iostream>
#include <fstream>
#include <thread>
#include <algorithm>
#include <random>
#include <sys/stat.h>
#include <nlohmann/json.hpp>

#include "NXConsoleLog.h"
#include "thread_helper.hpp"
#include "yt_audio_player.hpp"

using json = nlohmann::json;

namespace switube {

namespace {
constexpr const char* CONFIG_DIR = "/config/Switube";
constexpr const char* FAVORITES_FILE = "/config/Switube/favorites.json";
constexpr const char* PLAYLISTS_FILE = "/config/Switube/playlists_cache.json";

std::string findLocalAudioTrack(const std::string& trackId) {
  const char* exts[] = {".m4a", ".opus", ".mp3", ".webm"};
  for (const char* ext : exts) {
    std::string p = std::string(CONFIG_DIR) + "/downloads/" + trackId + ext;
    struct stat st{};
    if (stat(p.c_str(), &st) == 0 && st.st_size > 0) return p;
    std::string sp = std::string("sdmc:") + CONFIG_DIR + "/downloads/" + trackId + ext;
    if (stat(sp.c_str(), &st) == 0 && st.st_size > 0) return sp;
  }
  return "";
}

std::string findLocalTrackImage(const std::string& trackId) {
  if (trackId.empty()) return "";
  std::string p = std::string(CONFIG_DIR) + "/downloads/" + trackId + ".jpg";
  struct stat st{};
  if (stat(p.c_str(), &st) == 0 && st.st_size > 0) return p;
  std::string sp = std::string("sdmc:") + p;
  if (stat(sp.c_str(), &st) == 0 && st.st_size > 0) return sp;
  return "";
}
}

YtEngine& YtEngine::getInstance() {
  static YtEngine instance;
  return instance;
}

YtEngine::YtEngine() {
  client_.init();
}

YtEngine::~YtEngine() {
  shutdown();
}

bool YtEngine::init() {
  mkdir("/config", 0755);
  mkdir(CONFIG_DIR, 0755);

  YtAudioPlayer::getInstance().init();
  YtAudioPlayer::getInstance().setCompletionCallback([this]() {
    YtRepeatMode mode = repeatMode_.load();
    if (mode == YtRepeatMode::One) {
      size_t idx = 0;
      {
        std::lock_guard<std::recursive_mutex> lock(dataMutex_);
        if (queue_.empty()) return;
        idx = currentQueueIndex_;
      }
      playQueueIndex(idx);
      return;
    }
    if (mode == YtRepeatMode::Off) {
      bool atEnd = false;
      {
        std::lock_guard<std::recursive_mutex> lock(dataMutex_);
        atEnd = queue_.empty() || (currentQueueIndex_ + 1 >= queue_.size());
        if (atEnd) {
          state_ = PlaybackState::Stopped;
          nowPlaying_.positionMs = 0;
        }
      }
      if (atEnd) {
        notifyObservers();
        return;
      }
    }
    next();
  });

  YtAudioPlayer::getInstance().setPlaybackStartCallback([this]() {
    {
      std::lock_guard<std::recursive_mutex> lock(dataMutex_);
      state_ = PlaybackState::Playing;
      positionMeasuredAt_ = std::chrono::steady_clock::now();
    }
    notifyObservers();
  });

  loadFavorites();
  loadUserPlaylists();
  isRunning_ = true;
#ifndef SWITUBE_BACKGROUND_SERVICE
  if (client_.isSignedIn()) {
    syncAccountLibrary();
  }
#endif
  return true;
}

void YtEngine::shutdown() {
  isRunning_ = false;
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    observers_.clear();
  }
  YtAudioPlayer::getInstance().shutdown();
  saveFavorites();
  saveUserPlaylists();
}

size_t YtEngine::addObserver(EngineObserver observer) {
  std::lock_guard<std::recursive_mutex> lock(dataMutex_);
  size_t id = nextObserverId_++;
  observers_.push_back({id, std::move(observer)});
  return id;
}

void YtEngine::removeObserver(size_t id) {
  std::lock_guard<std::recursive_mutex> lock(dataMutex_);
  observers_.erase(
      std::remove_if(observers_.begin(), observers_.end(),
                     [id](const auto& pair) { return pair.first == id; }),
      observers_.end());
}

void YtEngine::notifyObservers() {
  std::vector<EngineObserver> callbacks;
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    for (const auto& pair : observers_) {
      callbacks.push_back(pair.second);
    }
  }
  for (const auto& cb : callbacks) {
    if (cb) cb();
  }
}

NowPlayingInfo YtEngine::getNowPlaying() const {
  std::lock_guard<std::recursive_mutex> lock(dataMutex_);
  NowPlayingInfo info = nowPlaying_;
  if (state_ == PlaybackState::Loading || state_ == PlaybackState::Stopped || state_ == PlaybackState::Error) {
    info.positionMs = 0;
    return info;
  }
  info.positionMs = YtAudioPlayer::getInstance().getPositionMs();
  return info;
}

std::vector<YtTrack> YtEngine::getQueue() const {
  std::lock_guard<std::recursive_mutex> lock(dataMutex_);
  return queue_;
}

void YtEngine::playTrack(const YtTrack& track) {
  if (track.isPlaylist || track.id.size() != 11) {
    nxlog::pushf("playTrack: '%s' is a playlist/album, resolving tracks...", track.id.c_str());
    {
      std::lock_guard<std::recursive_mutex> lock(dataMutex_);
      nowPlaying_.id = track.id;
      nowPlaying_.title = track.title;
      nowPlaying_.artist = "Cargando lista...";
      nowPlaying_.imageUrl = track.imageUrl;
      state_ = PlaybackState::Loading;
    }
    notifyObservers();

    launchWorkerWithStack([this, browseId = track.id]() {
      std::vector<YtTrack> playlistTracks;
      bool ok = client_.getPlaylistTracks(browseId, playlistTracks);
      if (ok && !playlistTracks.empty()) {
        playQueue(playlistTracks, 0);
      } else {
        nxlog::pushf("playTrack: failed to get tracks for playlist '%s'", browseId.c_str());
        {
          std::lock_guard<std::recursive_mutex> lock(dataMutex_);
          state_ = PlaybackState::Error;
        }
        notifyObservers();
      }
    });
    return;
  }

  playQueue({track}, 0);
}

void YtEngine::playQueue(const std::vector<YtTrack>& tracks, size_t startIndex) {
  playQueueWithMode(tracks, startIndex, false);
}

void YtEngine::playQueueShuffled(const std::vector<YtTrack>& tracks) {
  if (tracks.empty()) return;
  std::random_device rd;
  uint64_t seed = static_cast<uint64_t>(rd()) ^
                  static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
  std::mt19937 rng(static_cast<uint32_t>(seed ^ (seed >> 32)));
  std::uniform_int_distribution<size_t> dist(0, tracks.size() - 1);
  playQueueWithMode(tracks, dist(rng), true);
}

void YtEngine::playQueueWithMode(const std::vector<YtTrack>& tracks, size_t startIndex, bool shuffled) {
  if (tracks.empty() || startIndex >= tracks.size()) return;

  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    originalQueue_ = tracks;
    shuffle_.store(shuffled);
    if (shuffled) {
      queue_ = tracks;
      if (startIndex > 0) {
        std::swap(queue_[0], queue_[startIndex]);
      }
      if (queue_.size() > 2) {
        std::random_device rd;
        uint64_t seed = static_cast<uint64_t>(rd()) ^
                        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        std::mt19937 rng(static_cast<uint32_t>(seed ^ (seed >> 32)));
        std::shuffle(queue_.begin() + 1, queue_.end(), rng);
      }
      startIndex = 0;
    } else {
      queue_ = tracks;
    }
  }
  playQueueIndex(startIndex);
}

void YtEngine::playQueueIndex(size_t startIndex) {
  std::string trackId;
  uint32_t dur = 0;
  uint64_t requestGen = ++playRequestGen_;

  // Detener inmediatamente la pista anterior para que no continúe sonando ni reporte posición
  YtAudioPlayer::getInstance().stop();

  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    if (queue_.empty() || startIndex >= queue_.size()) return;
    currentQueueIndex_ = startIndex;
    const auto& current = queue_[currentQueueIndex_];

    trackId = current.id;
    std::string localImg = findLocalTrackImage(current.id);
    nowPlaying_.id = current.id;
    nowPlaying_.title = current.title;
    nowPlaying_.artist = current.artist;
    nowPlaying_.album = current.album;
    nowPlaying_.imageUrl = !localImg.empty() ? localImg : current.imageUrl;
    nowPlaying_.durationMs = current.durationMs;
    nowPlaying_.positionMs = 0;
    nowPlaying_.isFavorite = isFavorite(current.id);
    dur = nowPlaying_.durationMs;

    state_ = PlaybackState::Loading;
    positionMeasuredAt_ = std::chrono::steady_clock::now();
  }
  notifyObservers();

  // Si la pista ya está descargada en la SD, reproducirla de inmediato sin conexión
  std::string localPath = findLocalAudioTrack(trackId);
  if (!localPath.empty()) {
    nxlog::pushf("playQueue: playing local downloaded track '%s' (%s)", trackId.c_str(), localPath.c_str());
    YtAudioPlayer::getInstance().play(localPath, dur, 0);
    return;
  }

#ifdef SWITUBE_BACKGROUND_SERVICE
  nxlog::pushf("playQueue: local file not found for '%s' in background service", trackId.c_str());
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    state_ = PlaybackState::Error;
  }
  notifyObservers();
  return;
#else
  // Lanzar resolución y arranque con stack de 512 KiB para TLS/mbedtls
  launchWorkerWithStack([this, trackId, requestGen]() {
    AudioStreamUrl stream;
    bool ok = client_.resolveAudioStream(trackId, stream);

    if (requestGen != playRequestGen_.load()) {
      nxlog::pushf("playQueue: stale request discarded for '%s' (req %llu, current %llu)",
                   trackId.c_str(), (unsigned long long)requestGen, (unsigned long long)playRequestGen_.load());
      return;
    }

    if (ok) {
      consecutiveResolveFails_.store(0);
      {
        std::lock_guard<std::recursive_mutex> lock(dataMutex_);
        if (stream.approxDurationMs > 0 && nowPlaying_.durationMs == 0) {
          nowPlaying_.durationMs = stream.approxDurationMs;
        }
      }
      nxlog::pushf("Playback stream resolved for '%s' (%s, bitrate: %d)",
                   trackId.c_str(), stream.codec.c_str(), stream.bitrate);

      YtAudioPlayer::getInstance().play(stream.url, stream.approxDurationMs, stream.contentLength);
    } else {
      int fails = ++consecutiveResolveFails_;
      nxlog::pushf("Playback failed to resolve stream for '%s' (fails=%d).", trackId.c_str(), fails);
      bool canAdvance = false;
      {
        std::lock_guard<std::recursive_mutex> lock(dataMutex_);
        canAdvance = (!queue_.empty() && queue_.size() > 1 && fails < 3);
        if (!canAdvance) {
          state_ = PlaybackState::Error;
        }
      }
      if (canAdvance) {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        next();
      } else {
        notifyObservers();
      }
    }
  });
#endif
}

void YtEngine::playPause() {
  bool playing = false;
  bool changed = false;
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    if (state_ == PlaybackState::Playing) {
      state_ = PlaybackState::Paused;
      playing = false;
      changed = true;
    } else if (state_ == PlaybackState::Paused) {
      state_ = PlaybackState::Playing;
      positionMeasuredAt_ = std::chrono::steady_clock::now();
      playing = true;
      changed = true;
    }
  }
  if (!changed) return;
  if (playing) {
    YtAudioPlayer::getInstance().resume();
  } else {
    YtAudioPlayer::getInstance().pause();
  }
  notifyObservers();
}

void YtEngine::next() {
  size_t nextIdx = 0;
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    if (queue_.empty()) return;
    nextIdx = (currentQueueIndex_ + 1) % queue_.size();
  }
  playQueueIndex(nextIdx);
}

void YtEngine::previous() {
  size_t prevIdx = 0;
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    if (queue_.empty()) return;
    prevIdx = (currentQueueIndex_ == 0) ? queue_.size() - 1 : currentQueueIndex_ - 1;
  }
  playQueueIndex(prevIdx);
}

void YtEngine::setShuffle(bool enabled) {
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    if (shuffle_.load() == enabled) return;
    shuffle_.store(enabled);
    if (!queue_.empty()) {
      std::string currentId = nowPlaying_.id;
      if (enabled) {
        if (originalQueue_.empty()) originalQueue_ = queue_;
        // Mantener la pista actual en índice 0 y mezclar el resto
        if (currentQueueIndex_ < queue_.size()) {
          std::swap(queue_[0], queue_[currentQueueIndex_]);
        }
        if (queue_.size() > 2) {
          std::mt19937 rng(static_cast<uint32_t>(
              std::chrono::steady_clock::now().time_since_epoch().count()));
          std::shuffle(queue_.begin() + 1, queue_.end(), rng);
        }
        currentQueueIndex_ = 0;
      } else if (!originalQueue_.empty()) {
        queue_ = originalQueue_;
        currentQueueIndex_ = 0;
        for (size_t i = 0; i < queue_.size(); ++i) {
          if (queue_[i].id == currentId) {
            currentQueueIndex_ = i;
            break;
          }
        }
      }
    }
  }
  notifyObservers();
}

void YtEngine::toggleShuffle() {
  setShuffle(!shuffle_.load());
}

void YtEngine::setRepeatMode(YtRepeatMode mode) {
  if (repeatMode_.exchange(mode) != mode) {
    notifyObservers();
  }
}

void YtEngine::cycleRepeatMode() {
  YtRepeatMode cur = repeatMode_.load();
  YtRepeatMode nextMode = YtRepeatMode::All;
  if (cur == YtRepeatMode::Off) nextMode = YtRepeatMode::All;
  else if (cur == YtRepeatMode::All) nextMode = YtRepeatMode::One;
  else nextMode = YtRepeatMode::Off;
  setRepeatMode(nextMode);
}

void YtEngine::seekTo(uint32_t positionMs) {
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    nowPlaying_.positionMs = positionMs;
    positionMeasuredAt_ = std::chrono::steady_clock::now();
  }
  YtAudioPlayer::getInstance().seekTo(positionMs);
  notifyObservers();
}

void YtEngine::setVolume(float volume) {
  float clamped = 0.8f;
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    nowPlaying_.volume = std::clamp(volume, 0.0f, 1.0f);
    clamped = nowPlaying_.volume;
  }
  YtAudioPlayer::getInstance().setVolume(clamped);
  notifyObservers();
}

void YtEngine::onTrackDeleted(const std::string& trackId) {
  if (trackId.empty()) return;
  bool wasPlayingThis = false;
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    wasPlayingThis = (nowPlaying_.id == trackId);
  }

  if (wasPlayingThis) {
    ++playRequestGen_;
    YtAudioPlayer::getInstance().stop();
  }

  bool notify = false;
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    originalQueue_.erase(
        std::remove_if(originalQueue_.begin(), originalQueue_.end(),
                       [&trackId](const YtTrack& t) { return t.id == trackId; }),
        originalQueue_.end());
    for (size_t i = 0; i < queue_.size();) {
      if (queue_[i].id == trackId) {
        queue_.erase(queue_.begin() + i);
        if (currentQueueIndex_ > i && currentQueueIndex_ > 0) {
          currentQueueIndex_--;
        } else if (currentQueueIndex_ >= queue_.size() && !queue_.empty()) {
          currentQueueIndex_ = 0;
        }
      } else {
        ++i;
      }
    }
    if (nowPlaying_.id == trackId) {
      nowPlaying_ = NowPlayingInfo{};
      state_ = PlaybackState::Stopped;
      notify = true;
    }
  }
  if (notify) {
    notifyObservers();
  }
}

bool YtEngine::isFavorite(const std::string& trackId) {
  for (const auto& fav : favorites_) {
    if (fav.id == trackId) return true;
  }
  return false;
}

void YtEngine::toggleFavoriteCurrent() {
  std::string targetId;
  bool nowLiked = false;
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    if (nowPlaying_.id.empty()) return;
    targetId = nowPlaying_.id;

    auto it = std::find_if(favorites_.begin(), favorites_.end(),
                           [this](const YtTrack& t) { return t.id == nowPlaying_.id; });

    if (it != favorites_.end()) {
      favorites_.erase(it);
      nowPlaying_.isFavorite = false;
      nowLiked = false;
    } else {
      YtTrack t;
      t.id = nowPlaying_.id;
      t.title = nowPlaying_.title;
      t.artist = nowPlaying_.artist;
      t.album = nowPlaying_.album;
      t.imageUrl = nowPlaying_.imageUrl;
      t.durationMs = nowPlaying_.durationMs;
      favorites_.insert(favorites_.begin(), t);
      nowPlaying_.isFavorite = true;
      nowLiked = true;
    }
    saveFavorites();
  }
  notifyObservers();

#ifndef SWITUBE_BACKGROUND_SERVICE
  if (client_.isSignedIn() && !targetId.empty()) {
    launchWorkerWithStack([this, targetId, nowLiked]() {
      client_.rateTrack(targetId, nowLiked);
    });
  }
#endif
}

std::vector<YtTrack> YtEngine::getFavorites() {
  std::lock_guard<std::recursive_mutex> lock(dataMutex_);
  return favorites_;
}

std::vector<YtTrack> YtEngine::getUserPlaylists() {
  std::lock_guard<std::recursive_mutex> lock(dataMutex_);
  return userPlaylists_;
}

void YtEngine::clearAccountLibrary() {
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    userPlaylists_.clear();
    likedContinuationToken_.clear();
    playlistsContinuationToken_.clear();
    saveUserPlaylists();
  }
  notifyObservers();
}

bool YtEngine::hasMoreFavorites() const {
  std::lock_guard<std::recursive_mutex> lock(dataMutex_);
  return !likedContinuationToken_.empty();
}

bool YtEngine::hasMorePlaylists() const {
  std::lock_guard<std::recursive_mutex> lock(dataMutex_);
  return !playlistsContinuationToken_.empty();
}

void YtEngine::loadMoreFavorites() {
#ifndef SWITUBE_BACKGROUND_SERVICE
  if (!client_.isSignedIn() || syncingLibrary_.load()) return;
  std::string contTok;
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    if (likedContinuationToken_.empty()) return;
    contTok = likedContinuationToken_;
  }
  bool expected = false;
  if (!loadingMoreFavorites_.compare_exchange_strong(expected, true)) return;

  launchWorkerWithStack([this, contTok]() {
    std::vector<YtTrack> moreLiked;
    std::string nextTok;
    bool ok = client_.getLikedTracks(moreLiked, nextTok, contTok);

    bool changed = false;
    {
      std::lock_guard<std::recursive_mutex> lock(dataMutex_);
      likedContinuationToken_ = nextTok;
      if (ok && !moreLiked.empty()) {
        for (const auto& t : moreLiked) {
          bool exists = false;
          for (const auto& f : favorites_) {
            if (f.id == t.id) {
              exists = true;
              break;
            }
          }
          if (!exists) {
            favorites_.push_back(t);
            changed = true;
          }
        }
        if (changed) {
          saveFavorites();
        }
      }
    }

    loadingMoreFavorites_.store(false);
    if (changed) {
      notifyObservers();
    }
  });
#endif
}

void YtEngine::loadMorePlaylists() {
#ifndef SWITUBE_BACKGROUND_SERVICE
  if (!client_.isSignedIn() || syncingLibrary_.load()) return;
  std::string contTok;
  {
    std::lock_guard<std::recursive_mutex> lock(dataMutex_);
    if (playlistsContinuationToken_.empty()) return;
    contTok = playlistsContinuationToken_;
  }
  bool expected = false;
  if (!loadingMorePlaylists_.compare_exchange_strong(expected, true)) return;

  launchWorkerWithStack([this, contTok]() {
    std::vector<YtTrack> morePlaylists;
    std::string nextTok;
    bool ok = client_.getLibraryPlaylists(morePlaylists, nextTok, contTok);

    bool changed = false;
    {
      std::lock_guard<std::recursive_mutex> lock(dataMutex_);
      playlistsContinuationToken_ = nextTok;
      if (ok && !morePlaylists.empty()) {
        for (const auto& p : morePlaylists) {
          bool exists = false;
          for (const auto& cur : userPlaylists_) {
            if (cur.id == p.id) {
              exists = true;
              break;
            }
          }
          if (!exists) {
            userPlaylists_.push_back(p);
            changed = true;
          }
        }
        if (changed) {
          saveUserPlaylists();
        }
      }
    }

    loadingMorePlaylists_.store(false);
    if (changed) {
      notifyObservers();
    }
  });
#endif
}

void YtEngine::syncAccountLibrary() {
#ifndef SWITUBE_BACKGROUND_SERVICE
  if (!client_.isSignedIn()) return;
  bool expected = false;
  if (!syncingLibrary_.compare_exchange_strong(expected, true)) return;
  notifyObservers();

  launchWorkerWithStack([this]() {
    if (client_.getAccountInfo().name.empty() || client_.getAccountInfo().name == "YouTube Music") {
      client_.fetchAccountProfile();
    }

    std::vector<YtTrack> liked;
    std::string likedContTok;
    bool gotLikes = client_.getLikedTracks(liked, likedContTok, "");

    std::vector<YtTrack> playlists;
    std::string playlistsContTok;
    bool gotPlaylists = client_.getLibraryPlaylists(playlists, playlistsContTok, "");

    {
      std::lock_guard<std::recursive_mutex> lock(dataMutex_);
      if (gotLikes && !liked.empty()) {
        likedContinuationToken_ = likedContTok;
        favorites_ = std::move(liked);
        if (!nowPlaying_.id.empty()) {
          nowPlaying_.isFavorite = isFavorite(nowPlaying_.id);
        }
        saveFavorites();
      }
      if (gotPlaylists) {
        playlistsContinuationToken_ = playlistsContTok;
        userPlaylists_ = std::move(playlists);
        saveUserPlaylists();
      }
    }

    syncingLibrary_.store(false);
    notifyObservers();
  });
#endif
}

void YtEngine::loadFavorites() {
  std::ifstream f(FAVORITES_FILE);
  if (!f.is_open()) return;

  try {
    json j;
    f >> j;
    favorites_.clear();
    for (const auto& item : j) {
      YtTrack t;
      t.id = item.value("id", "");
      t.title = item.value("title", "");
      t.artist = item.value("artist", "");
      t.album = item.value("album", "");
      t.imageUrl = item.value("imageUrl", "");
      t.durationMs = item.value("durationMs", 0u);
      if (!t.id.empty()) {
        favorites_.push_back(t);
      }
    }
  } catch (...) {}
}

void YtEngine::saveFavorites() {
  std::ofstream f(FAVORITES_FILE);
  if (!f.is_open()) return;

  try {
    json j = json::array();
    for (const auto& t : favorites_) {
      j.push_back({
          {"id", t.id},
          {"title", t.title},
          {"artist", t.artist},
          {"album", t.album},
          {"imageUrl", t.imageUrl},
          {"durationMs", t.durationMs}
      });
    }
    f << j.dump(2);
  } catch (...) {}
}

void YtEngine::loadUserPlaylists() {
  std::ifstream f(PLAYLISTS_FILE);
  if (!f.is_open()) return;

  try {
    json j;
    f >> j;
    userPlaylists_.clear();
    for (const auto& item : j) {
      YtTrack t;
      t.id = item.value("id", "");
      t.title = item.value("title", "");
      t.artist = item.value("artist", "");
      t.imageUrl = item.value("imageUrl", "");
      t.isPlaylist = item.value("isPlaylist", true);
      t.isArtist = item.value("isArtist", false);
      if (!t.id.empty()) {
        userPlaylists_.push_back(t);
      }
    }
  } catch (...) {}
}

void YtEngine::saveUserPlaylists() {
  std::ofstream f(PLAYLISTS_FILE);
  if (!f.is_open()) return;

  try {
    json j = json::array();
    for (const auto& t : userPlaylists_) {
      j.push_back({
          {"id", t.id},
          {"title", t.title},
          {"artist", t.artist},
          {"imageUrl", t.imageUrl},
          {"isPlaylist", t.isPlaylist},
          {"isArtist", t.isArtist}
      });
    }
    f << j.dump(2);
  } catch (...) {}
}

} // namespace switube
