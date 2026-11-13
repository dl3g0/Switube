#include "background_backend.hpp"

#include "NXConsoleLog.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sys/stat.h>
#include <nlohmann/json.hpp>

namespace switube::phase2 {
namespace {

constexpr auto PollInterval = std::chrono::milliseconds(50);
constexpr auto SnapshotInterval = std::chrono::milliseconds(250);
constexpr const char* ArtworkUrlFile = "/config/Switube/current-artwork-url";
constexpr const char* ArtworkUrlTemporary = "/config/Switube/current-artwork-url.tmp";
constexpr size_t MaxBrowseItems = 32;

static std::vector<YtTrack> loadDownloadedTracks() {
  std::vector<YtTrack> list;
  std::ifstream file("/config/Switube/downloads/downloads.json");
  if (!file.is_open()) {
    file.open("sdmc:/config/Switube/downloads/downloads.json");
  }
  if (!file.is_open()) return list;
  try {
    nlohmann::json j;
    file >> j;
    if (j.is_array()) {
      for (const auto& item : j) {
        YtTrack t;
        t.id = item.value("id", "");
        t.title = item.value("title", "");
        t.artist = item.value("artist", "");
        t.album = item.value("album", "");
        t.durationMs = item.value("durationMs", 0u);
        t.imageUrl = item.value("imagePath", "");
        if (t.imageUrl.empty()) t.imageUrl = item.value("imageUrl", "");
        if (!t.id.empty()) list.push_back(t);
      }
    }
  } catch (...) {}
  return list;
}

alignas(0x1000) static uint8_t s_workerStack[0x20000];  // 128 KiB static stack in BSS

void copyBrowseText(char* destination, size_t capacity, const std::string& source) {
  if (capacity == 0) return;
  const size_t length = std::min(capacity - 1, source.size());
  if (length != 0) std::memcpy(destination, source.data(), length);
  destination[length] = '\0';
}

SwitubePlaybackState translatePlaybackState(PlaybackState state) {
  switch (state) {
    case PlaybackState::Playing:
      return SwitubePlaybackState_Playing;
    case PlaybackState::Paused:
      return SwitubePlaybackState_Paused;
    case PlaybackState::Loading:
      return SwitubePlaybackState_Buffering;
    case PlaybackState::Stopped:
    case PlaybackState::Error:
    default:
      return SwitubePlaybackState_Stopped;
  }
}

}  // namespace

BackgroundBackend::BackgroundBackend(ServiceCore& core)
    : core(core), engine(YtEngine::getInstance()) {}

BackgroundBackend::~BackgroundBackend() { stop(); }

uint64_t BackgroundBackend::monotonicMs() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

bool BackgroundBackend::start() {
  if (running.load()) return true;

  if (!engine.init()) {
    nxlog::push("BackgroundBackend: YtEngine init failed");
    return false;
  }

  running.store(true);
  const uint64_t capabilities =
      SwitubeIpcCapability_Status | SwitubeIpcCapability_PlaybackControl |
      SwitubeIpcCapability_Seek | SwitubeIpcCapability_Queue |
      SwitubeIpcCapability_Browsing | SwitubeIpcCapability_Search |
      SwitubeIpcCapability_AudioSettings;

  core.configureCommands(true, capabilities, monotonicMs());
  core.publishServiceState(SwitubeServiceState_Ready, monotonicMs());
  core.publishConnection(true, monotonicMs());
  core.publishSeekable(true, monotonicMs());

  Result rc = threadCreate(&workerThread, &BackgroundBackend::threadEntry, this,
                           s_workerStack, sizeof(s_workerStack), 49, 3);
  if (R_FAILED(rc)) {
    nxlog::pushf("BackgroundBackend: threadCreate failed: 0x%08X", rc);
    running.store(false);
    return false;
  }
  rc = threadStart(&workerThread);
  if (R_FAILED(rc)) {
    nxlog::pushf("BackgroundBackend: threadStart failed: 0x%08X", rc);
    threadClose(&workerThread);
    running.store(false);
    return false;
  }
  threadStarted = true;
  nxlog::push("BackgroundBackend: worker thread started (128 KB static stack)");
  return true;
}

void BackgroundBackend::stop() {
  if (!running.exchange(false)) return;

  core.configureCommands(false, SwitubeIpcCapability_Status, monotonicMs());
  core.publishServiceState(SwitubeServiceState_Stopping, monotonicMs());

  if (threadStarted) {
    threadWaitForExit(&workerThread);
    threadClose(&workerThread);
    threadStarted = false;
  }

  engine.shutdown();
}

void BackgroundBackend::threadEntry(void* arg) {
  static_cast<BackgroundBackend*>(arg)->run();
}

void BackgroundBackend::publishArtworkUrl(const std::string& url) {
  if (url == lastPublishedArtworkUrl) return;
  lastPublishedArtworkUrl = url;

  if (url.empty()) {
    std::remove(ArtworkUrlFile);
    return;
  }

  std::ofstream out(ArtworkUrlTemporary, std::ios::trunc);
  if (out.is_open()) {
    out << url << "\n";
    out.close();
    std::rename(ArtworkUrlTemporary, ArtworkUrlFile);
  }
}

void BackgroundBackend::publishSnapshot() {
  const auto now = monotonicMs();
  const auto playing = engine.getNowPlaying();
  const auto state = engine.getState();
  const auto switubeState = translatePlaybackState(state);

  if (playing.title.empty()) {
    core.clearTrack(now);
  } else {
    core.publishTrack(playing.title, playing.artist, playing.album, now);
  }

  core.publishPlaybackState(switubeState, playing.positionMs, playing.durationMs, now);
  core.publishVolume(static_cast<uint32_t>(playing.volume * 1000.0f), now);

  const auto queue = engine.getQueue();
  size_t currentIdx = 0;
  for (size_t i = 0; i < queue.size(); ++i) {
    if (queue[i].id == playing.id) {
      currentIdx = i;
      break;
    }
  }
  core.publishQueue(static_cast<uint32_t>(currentIdx), static_cast<uint32_t>(queue.size()), now);

  publishArtworkUrl(playing.imageUrl);
}

void BackgroundBackend::run() {
  auto nextSnapshot = std::chrono::steady_clock::now();

  while (running.load()) {
    ControlMessage message{};
    while (core.tryPop(message)) {
      process(message);
    }

    const auto now = std::chrono::steady_clock::now();
    if (now >= nextSnapshot) {
      publishSnapshot();
      nextSnapshot = now + SnapshotInterval;
    }

    std::this_thread::sleep_for(PollInterval);
  }
}

void BackgroundBackend::process(const ControlMessage& message) {
  switch (message.command) {
    case SwitubeIpcCommand_PlayPause:
      engine.playPause();
      break;
    case SwitubeIpcCommand_Play:
      if (engine.getState() != PlaybackState::Playing) engine.playPause();
      break;
    case SwitubeIpcCommand_Pause:
      if (engine.getState() == PlaybackState::Playing) engine.playPause();
      break;
    case SwitubeIpcCommand_Next:
      engine.next();
      break;
    case SwitubeIpcCommand_Previous:
      engine.previous();
      break;
    case SwitubeIpcCommand_SetVolume:
      engine.setVolume(message.value / 1000.0f);
      break;
    case SwitubeIpcCommand_Seek:
      engine.seekTo(message.value);
      break;
    default:
      processBrowse(message);
      break;
  }
  publishSnapshot();
}

uint64_t BackgroundBackend::beginRoot(std::string_view title, std::string_view subtitle) {
  std::scoped_lock lock(browseMutex);
  pageHistory.clear();
  currentBrowseGeneration = core.beginBrowse(title, subtitle, 0, monotonicMs());
  return currentBrowseGeneration;
}

uint64_t BackgroundBackend::beginChild(std::string_view title, std::string_view subtitle) {
  std::scoped_lock lock(browseMutex);
  currentBrowseGeneration = core.beginBrowse(
      title, subtitle, static_cast<uint32_t>(pageHistory.size() + 1), monotonicMs());
  return currentBrowseGeneration;
}

void BackgroundBackend::completeBrowse(uint64_t generation, BrowsePage page) {
  std::scoped_lock lock(browseMutex);
  if (generation != currentBrowseGeneration) return;

  std::vector<SwitubeIpcBrowseItem> ipcItems;
  ipcItems.reserve(std::min(page.entries.size(), MaxBrowseItems));

  for (size_t i = 0; i < page.entries.size() && i < MaxBrowseItems; ++i) {
    const auto& entry = page.entries[i];
    SwitubeIpcBrowseItem item{};
    item.struct_size = sizeof(item);
    item.abi_version = SWITUBE_IPC_BROWSE_ABI_VERSION;
    item.generation = generation;
    item.index = static_cast<uint32_t>(i);
    copyBrowseText(item.name, sizeof(item.name), entry.name);
    copyBrowseText(item.subtitle, sizeof(item.subtitle), entry.subtitle);

    if (entry.type == BrowseEntryType::Track) {
      item.kind = SwitubeBrowseItemKind_Track;
      item.duration_ms = entry.track.durationMs;
    } else {
      item.kind = SwitubeBrowseItemKind_Section;
    }
    ipcItems.push_back(item);
  }

  currentPage = std::move(page);
  core.publishBrowse(generation, currentPage.title, currentPage.subtitle,
                     ipcItems.data(), ipcItems.size(), currentPage.depth,
                     monotonicMs());
}

void BackgroundBackend::failBrowse(uint64_t generation, const std::string& message) {
  std::scoped_lock lock(browseMutex);
  if (generation != currentBrowseGeneration) return;
  core.publishBrowseError(generation, message, monotonicMs());
}

void BackgroundBackend::requestHome() {
  requestLibrary();
}

void BackgroundBackend::requestLibrary() {
  const auto gen = beginRoot("Descargas", "Música guardada en SD");
  try {
    nxlog::push("requestLibrary: loading downloaded tracks from SD...");
    auto downloads = loadDownloadedTracks();
    BrowsePage page{};
    page.title = "Descargas";
    page.subtitle = std::to_string(downloads.size()) + " canciones";
    page.depth = 0;

    for (const auto& trk : downloads) {
      BrowseEntry entry{};
      entry.type = BrowseEntryType::Track;
      entry.name = trk.title;
      entry.subtitle = trk.artist;
      entry.track = trk;
      page.entries.push_back(entry);
    }

    if (page.entries.empty()) {
      BrowseEntry emptyEntry{};
      emptyEntry.type = BrowseEntryType::Section;
      emptyEntry.name = "Sin descargas";
      emptyEntry.subtitle = "Descarga música desde la app Switube";
      page.entries.push_back(emptyEntry);
    }

    nxlog::pushf("requestLibrary: loaded %zu downloaded tracks", downloads.size());
    completeBrowse(gen, std::move(page));
  } catch (const std::exception& e) {
    failBrowse(gen, e.what());
  } catch (...) {
    failBrowse(gen, "Error al leer descargas");
  }
}

void BackgroundBackend::requestSearch(const std::string& query) {
  if (query.empty()) return;
  const auto gen = beginRoot("Búsqueda en descargas", query);
  try {
    nxlog::pushf("requestSearch: searching '%s' in downloads...", query.c_str());
    auto allTracks = loadDownloadedTracks();
    std::string qLower = query;
    std::transform(qLower.begin(), qLower.end(), qLower.begin(), ::tolower);

    BrowsePage page{};
    page.title = "Resultados";
    page.subtitle = query;
    page.depth = 0;

    for (const auto& trk : allTracks) {
      std::string tLower = trk.title;
      std::string aLower = trk.artist;
      std::transform(tLower.begin(), tLower.end(), tLower.begin(), ::tolower);
      std::transform(aLower.begin(), aLower.end(), aLower.begin(), ::tolower);

      if (tLower.find(qLower) != std::string::npos || aLower.find(qLower) != std::string::npos) {
        BrowseEntry entry{};
        entry.type = BrowseEntryType::Track;
        entry.name = trk.title;
        entry.subtitle = trk.artist;
        entry.track = trk;
        page.entries.push_back(entry);
      }
    }

    if (page.entries.empty()) {
      failBrowse(gen, "Sin resultados para: " + query);
      return;
    }

    nxlog::pushf("requestSearch: found %zu matching tracks", page.entries.size());
    completeBrowse(gen, std::move(page));
  } catch (const std::exception& e) {
    failBrowse(gen, e.what());
  } catch (...) {
    failBrowse(gen, "Error en la búsqueda");
  }
}

void BackgroundBackend::browseCurrentQueue() {
  const auto gen = beginRoot("Cola de reproducción", "En reproducción");
  auto q = engine.getQueue();
  BrowsePage page{};
  page.title = "Cola actual";
  page.subtitle = std::to_string(q.size()) + " canciones";
  page.depth = 0;

  for (const auto& trk : q) {
    BrowseEntry entry{};
    entry.type = BrowseEntryType::Track;
    entry.name = trk.title;
    entry.subtitle = trk.artist;
    entry.track = trk;
    page.entries.push_back(entry);
  }

  completeBrowse(gen, std::move(page));
}

void BackgroundBackend::openBrowseItem(uint64_t generation, uint32_t index) {
  std::scoped_lock lock(browseMutex);
  if (generation != currentBrowseGeneration || index >= currentPage.entries.size()) return;

  const auto& entry = currentPage.entries[index];
  if (entry.type == BrowseEntryType::Section && !entry.sectionTracks.empty()) {
    pageHistory.push_back(currentPage);
    const auto gen = beginChild(entry.name, currentPage.title);

    BrowsePage child{};
    child.title = entry.name;
    child.subtitle = currentPage.title;
    child.depth = static_cast<uint32_t>(pageHistory.size());

    for (const auto& trk : entry.sectionTracks) {
      BrowseEntry te{};
      te.type = BrowseEntryType::Track;
      te.name = trk.title;
      te.subtitle = trk.artist;
      te.track = trk;
      child.entries.push_back(te);
    }

    completeBrowse(gen, std::move(child));
  } else if (entry.type == BrowseEntryType::Track) {
    playBrowseItem(generation, index);
  }
}

void BackgroundBackend::backBrowse(uint64_t generation) {
  std::scoped_lock lock(browseMutex);
  if (generation != currentBrowseGeneration || pageHistory.empty()) return;

  BrowsePage previous = std::move(pageHistory.back());
  pageHistory.pop_back();

  const auto gen = (pageHistory.empty()) ? beginRoot(previous.title, previous.subtitle)
                                         : beginChild(previous.title, previous.subtitle);
  completeBrowse(gen, std::move(previous));
}

void BackgroundBackend::playBrowseItem(uint64_t generation, uint32_t index) {
  std::scoped_lock lock(browseMutex);
  if (generation != currentBrowseGeneration || index >= currentPage.entries.size()) return;

  const auto& selected = currentPage.entries[index];
  if (selected.type == BrowseEntryType::Track) {
    std::vector<YtTrack> tracks;
    size_t playIndex = 0;
    for (size_t i = 0; i < currentPage.entries.size(); ++i) {
      if (currentPage.entries[i].type == BrowseEntryType::Track) {
        if (i == index) playIndex = tracks.size();
        tracks.push_back(currentPage.entries[i].track);
      }
    }
    if (!tracks.empty()) {
      engine.playQueue(tracks, playIndex);
    }
  }
}

void BackgroundBackend::processBrowse(const ControlMessage& message) {
  switch (message.command) {
    case SwitubeIpcCommand_BrowseHome:
      requestHome();
      break;
    case SwitubeIpcCommand_BrowseLibrary:
      requestLibrary();
      break;
    case SwitubeIpcCommand_BrowseSearch:
      requestSearch(message.query.data());
      break;
    case SwitubeIpcCommand_BrowseOpen:
      openBrowseItem(message.generation, message.value);
      break;
    case SwitubeIpcCommand_BrowseBack:
      backBrowse(message.generation);
      break;
    case SwitubeIpcCommand_BrowsePlay:
      playBrowseItem(message.generation, message.value);
      break;
    case SwitubeIpcCommand_BrowseCurrentPlaylist:
    case SwitubeIpcCommand_BrowseQueue:
      browseCurrentQueue();
      break;
    default:
      break;
  }
}

}  // namespace switube::phase2
