#include <borealis/core/touch/tap_gesture.hpp>
#include "tabs.hpp"
#include "download_manager.hpp"

#include <thread>
#include <chrono>
#include "ui_helpers.hpp"
#include "NXConsoleLog.h"
#include "thread_helper.hpp"
#include "track_list_activity.hpp"
#include "artist_activity.hpp"
#include "i18n.hpp"

using switube::launchWorkerWithStack;
using switube::tr;

namespace {

brls::Box* makeHeading(const std::string& title, const std::string& subtitle,
                       brls::Label** titleOut = nullptr,
                       brls::Label** subtitleOut = nullptr) {
  auto* heading = new brls::Box();
  heading->setAxis(brls::Axis::COLUMN);
  heading->setGrow(1.0f);
  auto* tLabel = swui::makeLabel(title, 28, swui::textPrimary(), true);
  auto* sLabel = swui::makeLabel(subtitle, 15, swui::textSecondary(), true);
  sLabel->setMarginTop(4);
  heading->addView(tLabel);
  heading->addView(sLabel);
  if (titleOut) *titleOut = tLabel;
  if (subtitleOut) *subtitleOut = sLabel;
  return heading;
}

} // namespace

// -------------------------------------------------------------
// HomeTab
// -------------------------------------------------------------
HomeTab::HomeTab() : alive(std::make_shared<std::atomic_bool>(true)) {
  setAxis(brls::Axis::COLUMN);
  setGrow(1.0f);
  setBackgroundColor(swui::background());

  auto* top = new brls::Box();
  top->setAxis(brls::Axis::ROW);
  top->setAlignItems(brls::AlignItems::CENTER);
  top->setPadding(20, swui::kPagePadding, 12, swui::kPagePadding);
  top->addView(makeHeading(tr("home.title"), tr("home.subtitle"), &titleLabel, &subtitleLabel));

  refreshBtn = swui::makeActionButton(
      tr("home.refresh"),
      [this](brls::View*) {
        load();
        return true;
      },
      false, &refreshBtnLabel);
  refreshBtn->setWidth(140);
  top->addView(refreshBtn);
  addView(top);

  statusLabel = swui::makeLabel(tr("home.loading"), 15, swui::textSecondary());
  statusLabel->setMarginLeft(swui::kPagePadding);
  statusLabel->setMarginRight(swui::kPagePadding);
  statusLabel->setMarginBottom(10);
  addView(statusLabel);

  auto* scroll = new brls::ScrollingFrame();
  scroll->setGrow(1.0f);
  scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
  content = new brls::Box();
  content->setAxis(brls::Axis::COLUMN);
  content->setPadding(4, swui::kPagePadding, 34, swui::kPagePadding);
  scroll->setContentView(content);
  addView(scroll);

  auto token = alive;
  i18nObserverId = switube::I18n::getInstance().addObserver([this, token]() {
    brls::sync([this, token]() {
      if (token->load()) {
        updateLabels();
        load();
      }
    });
  });

  load();
}

HomeTab::~HomeTab() {
  alive->store(false);
  switube::I18n::getInstance().removeObserver(i18nObserverId);
}

void HomeTab::updateLabels() {
  if (titleLabel) titleLabel->setText(tr("home.title"));
  if (subtitleLabel) subtitleLabel->setText(tr("home.subtitle"));
  if (refreshBtnLabel) refreshBtnLabel->setText(tr("home.refresh"));
}

brls::View* HomeTab::getDefaultFocus() {
  if (refreshBtn) return refreshBtn;
  return brls::Box::getDefaultFocus();
}

void HomeTab::load() {
  statusLabel->setText(tr("home.loading"));
  auto token = alive;
  launchWorkerWithStack([this, token]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    try {
      nxlog::push("Fetching Home Feed from YouTube Music...");
      std::vector<switube::YtShelf> shelves;
      bool ok = switube::YtEngine::getInstance().getClient().getHomeFeed(shelves);
      nxlog::pushf("Home Feed result: %s (shelves: %zu)", ok ? "OK" : "FAILED", shelves.size());

      brls::sync([this, token, ok, shelves]() {
        if (!token->load()) return;
        if (ok && !shelves.empty()) {
          statusLabel->setText(tr("home.subtitle"));
          renderHome(shelves);
        } else {
          statusLabel->setText(tr("home.error"));
        }
      });
    } catch (const std::exception& e) {
      nxlog::pushf("HomeTab thread error: %s", e.what());
    } catch (...) {
      nxlog::push("HomeTab thread error: unknown exception");
    }
  });
}

void HomeTab::renderHome(const std::vector<switube::YtShelf>& shelves) {
  content->clearViews();

  if (shelves.empty()) {
    auto* emptyNotice = swui::makeStatePanel(
        tr("home.empty_title"),
        tr("home.empty_desc"));
    content->addView(emptyNotice);
    return;
  }

  for (const auto& shelf : shelves) {
    if (shelf.tracks.empty()) continue;

    auto* section = new swui::HorizontalSection(shelf.title, "");
    for (size_t i = 0; i < shelf.tracks.size(); ++i) {
      const auto& track = shelf.tracks[i];
      auto* card = new swui::MediaCard();
      card->setContent(track.title, track.artist, track.imageUrl);
      card->setAction([track] {
        if (track.isArtist) {
          brls::Application::pushActivity(new ArtistActivity(track.id, track.title, track.imageUrl));
        } else if (track.isPlaylist) {
          brls::Application::pushActivity(new TrackListActivity(track));
        } else {
          switube::YtEngine::getInstance().playTrack(track);
        }
      });
      section->addCard(card);
    }
    content->addView(section);
  }
}

// -------------------------------------------------------------
// SearchTab
// -------------------------------------------------------------
SearchTab::SearchTab() : alive(std::make_shared<std::atomic_bool>(true)) {
  setAxis(brls::Axis::COLUMN);
  setGrow(1.0f);
  setBackgroundColor(swui::background());

  auto* top = new brls::Box();
  top->setAxis(brls::Axis::ROW);
  top->setAlignItems(brls::AlignItems::CENTER);
  top->setPadding(20, swui::kPagePadding, 10, swui::kPagePadding);
  top->addView(makeHeading(tr("search.title"), tr("search.subtitle"), &titleLabel, &subtitleLabel));

  searchBtn = swui::makeActionButton(
      tr("search.open_keyboard"),
      [this](brls::View*) {
        openKeyboard();
        return true;
      },
      true, &queryLabel);
  searchBtn->setWidth(220);
  top->addView(searchBtn);
  addView(top);

  // Barra de chips de filtros (Estilo BitChord)
  auto* chipsScroller = new brls::HScrollingFrame();
  chipsScroller->setHeight(54);
  chipsScroller->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
  chipsScroller->setScrollingIndicatorVisible(false);
  chipsScroller->setMarginLeft(swui::kPagePadding);
  chipsScroller->setMarginRight(swui::kPagePadding);
  chipsScroller->setMarginBottom(6);

  chipsRow = new brls::Box();
  chipsRow->setAxis(brls::Axis::ROW);
  chipsRow->setAlignItems(brls::AlignItems::CENTER);

  chipAll = new swui::FilterChip(tr("filter.all"), [this]() { setFilter(switube::YtSearchFilter::All); });
  chipAll->setMarginRight(10);
  chipsRow->addView(chipAll);

  chipSongs = new swui::FilterChip(tr("filter.songs"), [this]() { setFilter(switube::YtSearchFilter::Songs); });
  chipSongs->setMarginRight(10);
  chipsRow->addView(chipSongs);

  chipVideos = new swui::FilterChip(tr("filter.videos"), [this]() { setFilter(switube::YtSearchFilter::Videos); });
  chipVideos->setMarginRight(10);
  chipsRow->addView(chipVideos);

  chipAlbums = new swui::FilterChip(tr("filter.albums"), [this]() { setFilter(switube::YtSearchFilter::Albums); });
  chipAlbums->setMarginRight(10);
  chipsRow->addView(chipAlbums);

  chipArtists = new swui::FilterChip(tr("filter.artists"), [this]() { setFilter(switube::YtSearchFilter::Artists); });
  chipArtists->setMarginRight(10);
  chipsRow->addView(chipArtists);

  chipPlaylists = new swui::FilterChip(tr("filter.playlists"), [this]() { setFilter(switube::YtSearchFilter::Playlists); });
  chipPlaylists->setMarginRight(10);
  chipsRow->addView(chipPlaylists);

  chipsScroller->setContentView(chipsRow);
  addView(chipsScroller);
  updateFilterChips();

  statusLabel = swui::makeLabel(tr("search.hint"), 15, swui::textSecondary());
  statusLabel->setMarginLeft(swui::kPagePadding);
  statusLabel->setMarginRight(swui::kPagePadding);
  statusLabel->setMarginBottom(10);
  addView(statusLabel);

  auto* scroll = new brls::ScrollingFrame();
  scroll->setGrow(1.0f);
  scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
  content = new brls::Box();
  content->setAxis(brls::Axis::COLUMN);
  content->setPadding(4, swui::kPagePadding, 34, swui::kPagePadding);
  scroll->setContentView(content);
  addView(scroll);

  auto token = alive;
  i18nObserverId = switube::I18n::getInstance().addObserver([this, token]() {
    brls::sync([this, token]() {
      if (token->load()) {
        updateLabels();
      }
    });
  });
}

SearchTab::~SearchTab() {
  alive->store(false);
  switube::I18n::getInstance().removeObserver(i18nObserverId);
}

void SearchTab::updateLabels() {
  if (titleLabel) titleLabel->setText(tr("search.title"));
  if (subtitleLabel) subtitleLabel->setText(tr("search.subtitle"));
  if (queryLabel && currentQuery.empty()) queryLabel->setText(tr("search.open_keyboard"));
  if (statusLabel && currentQuery.empty()) statusLabel->setText(tr("search.hint"));

  if (chipAll) chipAll->setText(tr("filter.all"));
  if (chipSongs) chipSongs->setText(tr("filter.songs"));
  if (chipVideos) chipVideos->setText(tr("filter.videos"));
  if (chipAlbums) chipAlbums->setText(tr("filter.albums"));
  if (chipArtists) chipArtists->setText(tr("filter.artists"));
  if (chipPlaylists) chipPlaylists->setText(tr("filter.playlists"));
}

brls::View* SearchTab::getDefaultFocus() {
  if (searchBtn) return searchBtn;
  return brls::Box::getDefaultFocus();
}

void SearchTab::openKeyboard() {
  brls::Application::getImeManager()->openForText(
      [this](std::string text) {
        if (!text.empty()) {
          search(text);
        }
      },
      tr("search.keyboard_title"), currentQuery, 64, "");
}

void SearchTab::setFilter(switube::YtSearchFilter filter) {
  if (currentFilter == filter) return;
  currentFilter = filter;
  updateFilterChips();
  if (!currentQuery.empty()) {
    search(currentQuery);
  }
}

void SearchTab::updateFilterChips() {
  chipAll->setSelected(currentFilter == switube::YtSearchFilter::All);
  chipSongs->setSelected(currentFilter == switube::YtSearchFilter::Songs);
  chipVideos->setSelected(currentFilter == switube::YtSearchFilter::Videos);
  chipAlbums->setSelected(currentFilter == switube::YtSearchFilter::Albums);
  chipArtists->setSelected(currentFilter == switube::YtSearchFilter::Artists);
  chipPlaylists->setSelected(currentFilter == switube::YtSearchFilter::Playlists);
}

void SearchTab::search(const std::string& query) {
  currentQuery = query;
  continuationToken.clear();
  currentResults.clear();
  content->clearViews();
  loadMoreBtn = nullptr;

  statusLabel->setText(tr("search.searching", query));

  auto token = alive;
  auto filter = currentFilter;
  launchWorkerWithStack([this, token, query, filter]() {
    try {
      nxlog::pushf("Searching YouTube Music for '%s' (filter: %d)...", query.c_str(), static_cast<int>(filter));
      std::vector<switube::YtTrack> tracks;
      std::string nextTok;
      bool ok = switube::YtEngine::getInstance().getClient().searchTracks(
          query, filter, tracks, nextTok, "", 25);
      nxlog::pushf("Search result: %s (%zu tracks, hasMore: %s)",
                   ok ? "OK" : "FAILED", tracks.size(), nextTok.empty() ? "NO" : "YES");

      brls::sync([this, token, query, ok, tracks = std::move(tracks), nextTok = std::move(nextTok)]() mutable {
        if (!token->load()) return;
        continuationToken = nextTok;
        if (ok && !tracks.empty()) {
          renderResults(tracks, false);
        } else {
          statusLabel->setText(tr("search.no_results", query));
        }
      });
    } catch (const std::exception& e) {
      nxlog::pushf("SearchTab thread error: %s", e.what());
    } catch (...) {
      nxlog::push("SearchTab thread error: unknown exception");
    }
  });
}

void SearchTab::loadMore() {
  if (continuationToken.empty() || currentQuery.empty()) return;

  if (loadMoreBtn) {
    loadMoreBtn->setFocusable(false);
  }

  auto token = alive;
  auto query = currentQuery;
  auto filter = currentFilter;
  auto cont = continuationToken;

  launchWorkerWithStack([this, token, query, filter, cont]() {
    try {
      std::vector<switube::YtTrack> newTracks;
      std::string nextTok;
      bool ok = switube::YtEngine::getInstance().getClient().searchTracks(
          query, filter, newTracks, nextTok, cont, 25);

      brls::sync([this, token, ok, newTracks = std::move(newTracks), nextTok = std::move(nextTok)]() mutable {
        if (!token->load()) return;
        continuationToken = nextTok;
        if (ok && !newTracks.empty()) {
          renderResults(newTracks, true);
        } else if (loadMoreBtn) {
          content->removeView(loadMoreBtn);
          loadMoreBtn = nullptr;
        }
      });
    } catch (const std::exception& e) {
      nxlog::pushf("SearchTab loadMore error: %s", e.what());
    }
  });
}

void SearchTab::renderResults(const std::vector<switube::YtTrack>& tracks, bool append) {
  if (!append) {
    content->clearViews();
    currentResults.clear();
    loadMoreBtn = nullptr;
  } else if (loadMoreBtn) {
    content->removeView(loadMoreBtn);
    loadMoreBtn = nullptr;
  }

  size_t startIdx = currentResults.size();
  for (size_t i = 0; i < tracks.size(); ++i) {
    currentResults.push_back(tracks[i]);
  }

  for (size_t i = startIdx; i < currentResults.size(); ++i) {
    const auto& track = currentResults[i];
    auto* row = new swui::TrackRow();
    row->setTrack(track, i, [this, track, i]() {
      if (track.isArtist) {
        brls::Application::pushActivity(new ArtistActivity(track.id, track.title, track.imageUrl));
      } else if (track.isPlaylist) {
        brls::Application::pushActivity(new TrackListActivity(track));
      } else {
        std::vector<switube::YtTrack> playable;
        size_t playIdx = 0;
        for (size_t k = 0; k < currentResults.size(); ++k) {
          const auto& candidate = currentResults[k];
          if (!candidate.isPlaylist && !candidate.isArtist && candidate.id.size() == 11) {
            if (k == i) playIdx = playable.size();
            playable.push_back(candidate);
          }
        }
        if (!playable.empty()) {
          switube::YtEngine::getInstance().playQueue(playable, playIdx);
        }
      }
    });
    content->addView(row);
  }

  statusLabel->setText(tr("search.results_count", currentQuery, std::to_string(currentResults.size())));

  if (!continuationToken.empty()) {
    loadMoreBtn = swui::makeActionButton(
        tr("search.load_more"),
        [this](brls::View*) {
          loadMore();
          return true;
        },
        false);
    loadMoreBtn->setWidth(300);
    loadMoreBtn->setMarginTop(18);
    loadMoreBtn->setMarginBottom(18);
    content->addView(loadMoreBtn);
  }
}

// -------------------------------------------------------------
// LibraryTab
// -------------------------------------------------------------
LibraryTab::LibraryTab() : alive(std::make_shared<std::atomic_bool>(true)) {
  setAxis(brls::Axis::COLUMN);
  setGrow(1.0f);
  setBackgroundColor(swui::background());

  auto* top = new brls::Box();
  top->setAxis(brls::Axis::ROW);
  top->setAlignItems(brls::AlignItems::CENTER);
  top->setPadding(20, swui::kPagePadding, 12, swui::kPagePadding);
  top->addView(makeHeading(tr("library.title"), tr("library.subtitle"), &titleLabel, &subtitleLabel));
  addView(top);

  auto* scroll = new brls::ScrollingFrame();
  scroll->setGrow(1.0f);
  scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
  content = new brls::Box();
  content->setAxis(brls::Axis::COLUMN);
  content->setPadding(4, swui::kPagePadding, 34, swui::kPagePadding);
  scroll->setContentView(content);
  addView(scroll);

  auto token = alive;
  observerId = switube::YtEngine::getInstance().addObserver([this, token] {
    brls::sync([this, token] {
      if (!token->load()) return;
      auto favs = switube::YtEngine::getInstance().getFavorites();
      if (favs.size() == renderedFavoriteIds.size()) {
        bool same = true;
        for (size_t i = 0; i < favs.size(); ++i) {
          if (favs[i].id != renderedFavoriteIds[i]) {
            same = false;
            break;
          }
        }
        if (same && !content->getChildren().empty()) return;
      }
      refresh();
    });
  });

  i18nObserverId = switube::I18n::getInstance().addObserver([this, token]() {
    brls::sync([this, token]() {
      if (token->load()) {
        renderedFavoriteIds.clear();
        updateLabels();
        refresh();
      }
    });
  });

  downloadObsId = switube::DownloadManager::getInstance().addObserver([this, token] {
    if (token->load()) {
      renderedFavoriteIds.clear();
      refresh();
    }
  });

  refresh();
}

LibraryTab::~LibraryTab() {
  alive->store(false);
  switube::YtEngine::getInstance().removeObserver(observerId);
  switube::I18n::getInstance().removeObserver(i18nObserverId);
  if (downloadObsId != 0) {
    switube::DownloadManager::getInstance().removeObserver(downloadObsId);
    downloadObsId = 0;
  }
}

void LibraryTab::updateLabels() {
  if (titleLabel) titleLabel->setText(tr("library.title"));
  if (subtitleLabel) subtitleLabel->setText(tr("library.subtitle"));
}

brls::View* LibraryTab::getDefaultFocus() {
  if (!content->getChildren().empty()) return content->getChildren()[0];
  return brls::Box::getDefaultFocus();
}

void LibraryTab::refresh() {
  bool hadFocus = this->isFocused() || this->isChildFocused();
  size_t focusedChildIdx = 0;
  if (hadFocus && content) {
    const auto& children = content->getChildren();
    for (size_t i = 0; i < children.size(); ++i) {
      if (children[i] == brls::Application::getCurrentFocus()) {
        focusedChildIdx = i;
        break;
      }
    }
    brls::Application::giveFocus(nullptr);
  }

  content->clearViews();
  auto favorites = switube::YtEngine::getInstance().getFavorites();
  renderedFavoriteIds.clear();
  renderedFavoriteIds.reserve(favorites.size());
  for (const auto& f : favorites) {
    renderedFavoriteIds.push_back(f.id);
  }

  if (favorites.empty()) {
    auto* emptyNotice = swui::makeStatePanel(
        tr("library.empty_title"),
        tr("library.empty_desc"));
    emptyNotice->setFocusable(true);
    content->addView(emptyNotice);
    if (hadFocus) {
      brls::Application::giveFocus(emptyNotice);
    }
    return;
  }

  for (size_t i = 0; i < favorites.size(); ++i) {
    const auto& track = favorites[i];
    auto* row = new swui::TrackRow();
    row->setTrack(track, i, [track, favorites, i]() {
      switube::YtEngine::getInstance().playQueue(favorites, i);
    });
    content->addView(row);
  }

  if (hadFocus && !content->getChildren().empty()) {
    size_t targetIdx = std::min(focusedChildIdx, content->getChildren().size() - 1);
    brls::Application::giveFocus(content->getChildren()[targetIdx]);
  }
}

// -------------------------------------------------------------
// DownloadsTab
// -------------------------------------------------------------
DownloadsTab::DownloadsTab() : alive(std::make_shared<std::atomic_bool>(true)) {
  setAxis(brls::Axis::COLUMN);
  setPadding(32, swui::kPagePadding, 32, swui::kPagePadding);

  auto* header = new brls::Box();
  header->setAxis(brls::Axis::COLUMN);
  header->setMarginBottom(18);

  titleLabel = swui::makeLabel(tr("downloads.title"), 28, swui::textPrimary(), true);
  subtitleLabel = swui::makeLabel(tr("downloads.subtitle"), 15, swui::textSecondary(), true);
  subtitleLabel->setMarginTop(4);

  header->addView(titleLabel);
  header->addView(subtitleLabel);
  addView(header);

  auto* scroller = new brls::ScrollingFrame();
  scroller->setGrow(1.0f);
  scroller->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

  content = new brls::Box();
  content->setAxis(brls::Axis::COLUMN);
  scroller->setContentView(content);
  addView(scroller);

  auto token = alive;
  observerId = switube::DownloadManager::getInstance().addObserver([this, token]() {
    if (token->load()) refresh();
  });

  i18nObserverId = switube::I18n::getInstance().addObserver([this, token]() {
    brls::sync([this, token]() {
      if (token->load()) {
        updateLabels();
        refresh();
      }
    });
  });

  refresh();
}

DownloadsTab::~DownloadsTab() {
  alive->store(false);
  if (observerId != 0) {
    switube::DownloadManager::getInstance().removeObserver(observerId);
    observerId = 0;
  }
  switube::I18n::getInstance().removeObserver(i18nObserverId);
}

void DownloadsTab::updateLabels() {
  if (titleLabel) titleLabel->setText(tr("downloads.title"));
  if (subtitleLabel) subtitleLabel->setText(tr("downloads.subtitle"));
}

brls::View* DownloadsTab::getDefaultFocus() {
  if (!content->getChildren().empty()) return content->getChildren()[0];
  return brls::Box::getDefaultFocus();
}

void DownloadsTab::refresh() {
  bool hadFocus = this->isFocused() || this->isChildFocused();
  size_t focusedChildIdx = 0;
  if (hadFocus && content) {
    const auto& children = content->getChildren();
    for (size_t i = 0; i < children.size(); ++i) {
      if (children[i] == brls::Application::getCurrentFocus()) {
        focusedChildIdx = i;
        break;
      }
    }
    brls::Application::giveFocus(nullptr);
  }

  content->clearViews();
  auto tracks = switube::DownloadManager::getInstance().getDownloadedAsTracks();

  if (tracks.empty()) {
    auto* emptyNotice = swui::makeStatePanel(
        tr("downloads.empty_title"),
        tr("downloads.empty_desc"));
    emptyNotice->setFocusable(true);
    content->addView(emptyNotice);
    if (subtitleLabel) subtitleLabel->setText(tr("downloads.subtitle_empty"));
    if (hadFocus) {
      brls::Application::giveFocus(emptyNotice);
    }
    return;
  }

  uint64_t bytes = switube::DownloadManager::getInstance().getTotalDownloadedBytes();
  double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
  char buf[96];
  snprintf(buf, sizeof(buf), "%zu %s  •  %.1f MB", tracks.size(),
           (tracks.size() == 1 ? "canción" : "canciones"), mb);
  if (subtitleLabel) subtitleLabel->setText(buf);

  // Botón "Reproducir todo"
  auto* playAllBtn = swui::makeActionButton("▶ " + tr("downloads.play_all"), [tracks](brls::View*) {
    switube::YtEngine::getInstance().playQueue(tracks, 0);
    return true;
  }, true);
  playAllBtn->setMarginBottom(14);
  content->addView(playAllBtn);

  for (size_t i = 0; i < tracks.size(); ++i) {
    const auto& track = tracks[i];
    auto* row = new swui::TrackRow();
    row->setTrack(track, i, [tracks, i]() {
      switube::YtEngine::getInstance().playQueue(tracks, i);
    });
    content->addView(row);
  }

  if (hadFocus && !content->getChildren().empty()) {
    size_t targetIdx = std::min(focusedChildIdx, content->getChildren().size() - 1);
    brls::Application::giveFocus(content->getChildren()[targetIdx]);
  }
}

// -------------------------------------------------------------
// MusicProgressBar
// -------------------------------------------------------------
class MusicProgressBar : public brls::View {
 public:
  MusicProgressBar() {
    setHeight(32);
    setFocusable(true);
    setFocusSound(brls::SOUND_FOCUS_SIDEBAR);

    registerAction(
        tr("now_playing.seek_back"), brls::BUTTON_LEFT,
        [](brls::View*) {
          auto& eng = switube::YtEngine::getInstance();
          auto now = eng.getNowPlaying();
          if (now.positionMs > 10000)
            eng.seekTo(now.positionMs - 10000);
          else
            eng.seekTo(0);
          return true;
        },
        false, true);

    registerAction(
        tr("now_playing.seek_fwd"), brls::BUTTON_RIGHT,
        [](brls::View*) {
          auto& eng = switube::YtEngine::getInstance();
          auto now = eng.getNowPlaying();
          if (now.positionMs + 10000 < now.durationMs)
            eng.seekTo(now.positionMs + 10000);
          return true;
        },
        false, true);

    registerAction(
        "Play/Pause", brls::BUTTON_A,
        [](brls::View*) {
          switube::YtEngine::getInstance().playPause();
          return true;
        },
        false, true);

    addGestureRecognizer(new brls::TapGestureRecognizer([this](brls::TapGestureStatus status, brls::Sound*) {
      if (status.state == brls::GestureState::END) {
        float vx = getX();
        float vy = getY();
        float vw = getWidth();
        float vh = getHeight();
        // Verificar que el toque se haya realizado dentro de los limites de la barra
        if (vw > 0.0f &&
            status.position.x >= vx && status.position.x <= vx + vw &&
            status.position.y >= vy - 12.0f && status.position.y <= vy + vh + 12.0f) {
          float ratio = std::clamp((status.position.x - vx) / vw, 0.0f, 1.0f);
          auto& eng = switube::YtEngine::getInstance();
          auto now = eng.getNowPlaying();
          if (now.durationMs > 0) {
            eng.seekTo(static_cast<uint32_t>(ratio * static_cast<float>(now.durationMs)));
          }
        }
      }
    }));
  }

  void setProgress(float p) {
    progress = std::clamp(p, 0.0f, 1.0f);
  }

  void draw(NVGcontext* vg, float x, float y, float width, float height,
            brls::Style style, brls::FrameContext* ctx) override {
    float barY = y + height * 0.5f - 3.0f;
    float barH = 6.0f;
    float corner = 3.0f;

    // Track de fondo
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, barY, width, barH, corner);
    nvgFillColor(vg, isFocused() ? nvgRGBA(255, 255, 255, 75) : nvgRGBA(255, 255, 255, 30));
    nvgFill(vg);

    // Barra de progreso llena
    float fillW = width * progress;
    if (fillW > 0.0f) {
      nvgBeginPath(vg);
      nvgRoundedRect(vg, x, barY, fillW, barH, corner);
      nvgFillColor(vg, isFocused() ? nvgRGB(255, 255, 255) : nvgRGB(255, 45, 60));
      nvgFill(vg);
    }

    // Cabezal / Scrubber knob
    float thumbX = x + fillW;
    float thumbY = y + height * 0.5f;
    float thumbR = isFocused() ? 8.0f : 5.5f;
    nvgBeginPath(vg);
    nvgCircle(vg, thumbX, thumbY, thumbR);
    nvgFillColor(vg, nvgRGB(255, 255, 255));
    nvgFill(vg);
  }

 private:
  float progress = 0.0f;
};

// -------------------------------------------------------------
// NowPlayingTab (Estilo BitChord / DEMO 3 SWITCH)
// -------------------------------------------------------------
NowPlayingTab::NowPlayingTab() : alive(std::make_shared<std::atomic_bool>(true)) {
  setAxis(brls::Axis::COLUMN);
  setGrow(1.0f);
  setPadding(0, 0, 0, 0);
  setJustifyContent(brls::JustifyContent::CENTER);
  setAlignItems(brls::AlignItems::CENTER);
  setBackgroundColor(nvgRGBA(0, 0, 0, 0));

  auto* body = new brls::Box();
  body->setPadding(0, 0, 0, 0);
  body->setAxis(brls::Axis::ROW);
  body->setAlignItems(brls::AlignItems::CENTER);
  body->setJustifyContent(brls::JustifyContent::CENTER);

  // Columna izquierda: Carátula de Álbum
  auto* leftCol = new brls::Box();
  leftCol->setAxis(brls::Axis::COLUMN);
  leftCol->setAlignItems(brls::AlignItems::CENTER);
  leftCol->setWidth(340);

  artwork = new brls::Image();
  artwork->setDimensions(340, 340);
  artwork->setCornerRadius(18);
  artwork->setBackgroundColor(swui::surfaceRaised());
  artwork->setScalingType(brls::ImageScalingType::FIT);
  leftCol->addView(artwork);

  auto* statusRow = new brls::Box();
  statusRow->setAxis(brls::Axis::ROW);
  statusRow->setAlignItems(brls::AlignItems::CENTER);
  statusRow->setJustifyContent(brls::JustifyContent::CENTER);
  statusRow->setWidth(340);
  statusRow->setMarginTop(14);

  deviceLabel = swui::makeLabel("Switch Audio (48 kHz)", 13, nvgRGB(145, 150, 160), true);
  statusRow->addView(deviceLabel);
  leftCol->addView(statusRow);

  body->addView(leftCol);

  // Columna derecha: Título, Artista, Scrubber y Controles (centrado y ancho balanceado)
  auto* rightCol = new brls::Box();
  rightCol->setAxis(brls::Axis::COLUMN);
  rightCol->setWidth(480);
  rightCol->setMarginLeft(54);
  rightCol->setJustifyContent(brls::JustifyContent::CENTER);

  contextLabel = swui::makeLabel(tr("now_playing.context_from"), 14, nvgRGB(160, 165, 175), true);
  contextLabel->setMarginBottom(10);
  rightCol->addView(contextLabel);

  titleLabel = swui::makeLabel(tr("now_playing.no_playback"), 30, swui::textPrimary(), true);
  titleLabel->setMarginBottom(8);
  rightCol->addView(titleLabel);

  artistLabel = swui::makeLabel(tr("now_playing.choose_track"), 19, swui::textSecondary(), true);
  artistLabel->setMarginBottom(26);
  rightCol->addView(artistLabel);

  // Barra de progreso y scrubber interactivo
  progressBar = new MusicProgressBar();
  rightCol->addView(progressBar);

  auto* timeRow = new brls::Box();
  timeRow->setAxis(brls::Axis::ROW);
  timeRow->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
  timeRow->setMarginTop(4);
  timeRow->setMarginBottom(24);

  elapsedLabel = swui::makeLabel("0:00", 14, swui::textSecondary(), true);
  remainingLabel = swui::makeLabel("0:00", 14, swui::textSecondary(), true);
  timeRow->addView(elapsedLabel);
  timeRow->addView(remainingLabel);
  rightCol->addView(timeRow);

  // Botones de reproducción principales
  auto* controls = new brls::Box();
  controls->setAxis(brls::Axis::ROW);
  controls->setAlignItems(brls::AlignItems::CENTER);
  controls->setJustifyContent(brls::JustifyContent::CENTER);

  prevBtn = swui::makePrevButton([](brls::View*) {
    switube::YtEngine::getInstance().previous();
    return true;
  });
  prevBtn->setWidth(76);
  prevBtn->setHeight(52);
  prevBtn->setCornerRadius(26);
  prevBtn->setMarginRight(18);

  playBtn = swui::makePlayPauseButton(
      [](brls::View*) {
        switube::YtEngine::getInstance().playPause();
        return true;
      },
      &playPauseIcon, true);
  playBtn->setWidth(96);
  playBtn->setHeight(56);
  playBtn->setCornerRadius(28);
  playBtn->setMarginRight(18);

  nextBtn = swui::makeNextButton([](brls::View*) {
    switube::YtEngine::getInstance().next();
    return true;
  });
  nextBtn->setWidth(76);
  nextBtn->setHeight(52);
  nextBtn->setCornerRadius(26);

  downloadBtn = new brls::Box();
  downloadBtn->setFocusable(true);
  downloadBtn->setWidth(64);
  downloadBtn->setHeight(52);
  downloadBtn->setCornerRadius(26);
  downloadBtn->setHighlightCornerRadius(26);
  downloadBtn->setBackgroundColor(swui::surfaceRaised());
  downloadBtn->setAlignItems(brls::AlignItems::CENTER);
  downloadBtn->setJustifyContent(brls::JustifyContent::CENTER);
  downloadBtn->setMarginLeft(18);
  downloadBtn->addGestureRecognizer(new brls::TapGestureRecognizer(downloadBtn));

  downloadBadge = new swui::DownloadBadge(30.0f);
  downloadBtn->addView(downloadBadge);

  downloadBtn->registerClickAction([this](brls::View*) {
    handleDownloadCurrentTrack();
    return true;
  });
  downloadBtn->registerAction("Descargar", brls::BUTTON_A, [this](brls::View*) {
    handleDownloadCurrentTrack();
    return true;
  });

  controls->addView(prevBtn);
  controls->addView(playBtn);
  controls->addView(nextBtn);
  controls->addView(downloadBtn);
  rightCol->addView(controls);

  body->addView(rightCol);
  addView(body);

  // Acciones rápidas de gatillos para saltar canciones / buscar
  registerAction(
      "Seek -10s", brls::BUTTON_LT,
      [](brls::View*) {
        auto& eng = switube::YtEngine::getInstance();
        auto now = eng.getNowPlaying();
        if (now.positionMs > 10000) eng.seekTo(now.positionMs - 10000);
        else eng.seekTo(0);
        return true;
      },
      false, true);

  registerAction(
      "Seek +10s", brls::BUTTON_RT,
      [](brls::View*) {
        auto& eng = switube::YtEngine::getInstance();
        auto now = eng.getNowPlaying();
        if (now.positionMs + 10000 < now.durationMs) eng.seekTo(now.positionMs + 10000);
        return true;
      },
      false, true);

  registerAction("Descargar", brls::BUTTON_X, [this](brls::View*) {
    handleDownloadCurrentTrack();
    return true;
  });

  auto token = alive;
  observerId = switube::YtEngine::getInstance().addObserver([this, token] {
    brls::sync([this, token] {
      if (token->load()) refresh();
    });
  });

  i18nObserverId = switube::I18n::getInstance().addObserver([this, token]() {
    brls::sync([this, token]() {
      if (token->load()) {
        updateLabels();
        refresh();
      }
    });
  });

  downloadObsId = switube::DownloadManager::getInstance().addObserver([this, token] {
    if (token->load()) refresh();
  });

  refresh();
}

NowPlayingTab::~NowPlayingTab() {
  alive->store(false);
  switube::YtEngine::getInstance().removeObserver(observerId);
  switube::I18n::getInstance().removeObserver(i18nObserverId);
  if (downloadObsId != 0) {
    switube::DownloadManager::getInstance().removeObserver(downloadObsId);
    downloadObsId = 0;
  }
}

void NowPlayingTab::handleDownloadCurrentTrack() {
  auto& engine = switube::YtEngine::getInstance();
  const auto now = engine.getNowPlaying();
  if (now.id.empty()) return;
  auto& dm = switube::DownloadManager::getInstance();
  if (dm.isDownloaded(now.id)) {
    dm.deleteDownload(now.id);
  } else if (!dm.isDownloading(now.id)) {
    switube::YtTrack trk;
    trk.id = now.id;
    trk.title = now.title;
    trk.artist = now.artist;
    trk.album = now.album;
    trk.imageUrl = now.imageUrl;
    trk.durationMs = now.durationMs;
    dm.startDownload(trk);
  }
  refresh();
}

brls::View* NowPlayingTab::getDefaultFocus() {
  if (playBtn) return playBtn;
  return brls::Box::getDefaultFocus();
}

void NowPlayingTab::updateLabels() {
  if (deviceLabel) deviceLabel->setText("Switch Audio (48 kHz)");
}

void NowPlayingTab::draw(NVGcontext* vg, float x, float y, float width, float height,
                         brls::Style style, brls::FrameContext* ctx) {
  // Actualización fluida y dinámica de la barra de progreso y tiempos cada frame
  auto& engine = switube::YtEngine::getInstance();
  const auto now = engine.getNowPlaying();

  if (playPauseIcon) {
    playPauseIcon->setPlaying(engine.getState() == switube::PlaybackState::Playing);
  }
  if (now.durationMs > 0) {
    float p = static_cast<float>(now.positionMs) / static_cast<float>(now.durationMs);
    if (progressBar) progressBar->setProgress(p);

    std::string posStr = swui::formatDuration(now.positionMs);
    if (posStr.empty()) posStr = "0:00";
    if (elapsedLabel && elapsedLabel->getFullText() != posStr) {
      elapsedLabel->setText(posStr);
    }

    std::string remStr;
    if (now.durationMs > now.positionMs) {
      uint32_t remMs = now.durationMs - now.positionMs;
      remStr = "-" + swui::formatDuration(remMs);
    } else {
      remStr = swui::formatDuration(now.durationMs);
    }
    if (remainingLabel && remainingLabel->getFullText() != remStr) {
      remainingLabel->setText(remStr);
    }
  }

  int tex = artwork ? artwork->getTexture() : 0;
  if (tex > 0) {
    nvgSave(vg);
    nvgBeginPath(vg);
    nvgRect(vg, x, y, width, height);

    // 1. Imagen de portada ambiental estirada al fondo con opacidad suave
    NVGpaint imgPaint = nvgImagePattern(vg, x, y, width, height, 0.0f, tex, 0.26f);
    nvgFillPaint(vg, imgPaint);
    nvgFill(vg);

    // 2. Viñeta degradada estilo BitChord: de rojo oscuro YouTube a negro obsidiana
    NVGpaint darkOverlay = nvgLinearGradient(vg, x, y, x, y + height,
                                             nvgRGBA(25, 7, 11, 160),
                                             nvgRGBA(7, 7, 10, 246));
    nvgBeginPath(vg);
    nvgRect(vg, x, y, width, height);
    nvgFillPaint(vg, darkOverlay);
    nvgFill(vg);

    // Viñeta radial para suavizar e integrar los bordes
    NVGpaint radialVignette = nvgRadialGradient(vg, x + width * 0.28f, y + height * 0.45f,
                                                height * 0.25f, width * 0.85f,
                                                nvgRGBA(0, 0, 0, 20),
                                                nvgRGBA(5, 5, 8, 200));
    nvgBeginPath(vg);
    nvgRect(vg, x, y, width, height);
    nvgFillPaint(vg, radialVignette);
    nvgFill(vg);

    nvgRestore(vg);
  } else {
    // Si no hay reproducción activa, degradado radial rojo YouTube / negro de DEMO 3 SWITCH
    nvgSave(vg);
    nvgBeginPath(vg);
    nvgRect(vg, x, y, width, height);
    NVGpaint bg = nvgRadialGradient(vg, x + width * 0.65f, y + height * 0.30f,
                                    height * 0.18f, width * 0.90f,
                                    nvgRGB(0x40, 0x0c, 0x14), nvgRGB(0x07, 0x07, 0x0a));
    nvgFillPaint(vg, bg);
    nvgFill(vg);
    nvgRestore(vg);
  }

  brls::Box::draw(vg, x, y, width, height, style, ctx);
}

void NowPlayingTab::refresh() {
  auto& engine = switube::YtEngine::getInstance();
  const auto now = engine.getNowPlaying();
  auto state = engine.getState();

  if (!now.id.empty()) {
    contextLabel->setText(tr("now_playing.context_from"));
    titleLabel->setText(now.title);
    artistLabel->setText(now.artist.empty() ? tr("now_playing.artist_fallback") : now.artist);

    std::string posStr = swui::formatDuration(now.positionMs);
    std::string durStr = swui::formatDuration(now.durationMs);
    if (posStr.empty()) posStr = "0:00";
    if (durStr.empty()) durStr = "0:00";
    elapsedLabel->setText(posStr);

    if (now.durationMs > now.positionMs) {
      uint32_t remMs = now.durationMs - now.positionMs;
      remainingLabel->setText("-" + swui::formatDuration(remMs));
    } else {
      remainingLabel->setText(durStr);
    }

    if (now.durationMs > 0) {
      progressBar->setProgress((float)now.positionMs / (float)now.durationMs);
    } else {
      progressBar->setProgress(0.0f);
    }

    if (playPauseIcon) {
      playPauseIcon->setPlaying(state == switube::PlaybackState::Playing);
    }

    if (!now.imageUrl.empty() && now.imageUrl != loadedArtworkUrl) {
      loadedArtworkUrl = now.imageUrl;
      swui::loadRemoteImage(artwork, now.imageUrl, 544);
    }

    if (downloadBtn && downloadBadge) {
      downloadBtn->setVisibility(brls::Visibility::VISIBLE);
      auto& dm = switube::DownloadManager::getInstance();
      bool downloaded = dm.isDownloaded(now.id);
      bool downloading = dm.isDownloading(now.id);

      if (downloaded) {
        downloadBadge->setState(swui::DownloadVisualState::Downloaded);
      } else if (downloading) {
        downloadBadge->setState(swui::DownloadVisualState::Downloading);
      } else {
        downloadBadge->setState(swui::DownloadVisualState::NotDownloaded);
      }

      std::string hint = downloaded ? "Eliminar descarga" : (downloading ? "Descargando..." : "Descargar");
      updateActionHint(brls::BUTTON_X, hint);
      if (this->isFocused() || this->isChildFocused()) {
        brls::Application::getGlobalHintsUpdateEvent()->fire();
      }
    }
  } else {
    if (downloadBtn) downloadBtn->setVisibility(brls::Visibility::GONE);
    contextLabel->setText(tr("now_playing.title"));
    titleLabel->setText(tr("now_playing.no_playback"));
    artistLabel->setText(tr("now_playing.choose_track"));
    elapsedLabel->setText("0:00");
    remainingLabel->setText("0:00");
    progressBar->setProgress(0.0f);
    if (playPauseIcon) playPauseIcon->setPlaying(false);
  }
}

// -------------------------------------------------------------
// SettingsTab
// -------------------------------------------------------------
SettingsTab::SettingsTab() : alive(std::make_shared<std::atomic_bool>(true)) {
  setAxis(brls::Axis::COLUMN);
  setGrow(1.0f);
  setBackgroundColor(swui::background());

  auto* top = new brls::Box();
  top->setAxis(brls::Axis::ROW);
  top->setAlignItems(brls::AlignItems::CENTER);
  top->setPadding(20, swui::kPagePadding, 12, swui::kPagePadding);
  top->addView(makeHeading(tr("settings.title"), tr("settings.subtitle"), &titleLabel, &subtitleLabel));
  addView(top);

  auto* scroll = new brls::ScrollingFrame();
  scroll->setGrow(1.0f);
  scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

  content = new brls::Box();
  content->setAxis(brls::Axis::COLUMN);
  content->setPadding(4, swui::kPagePadding, 34, swui::kPagePadding);

  scroll->setContentView(content);
  addView(scroll);

  // 1. Selector de Idioma (Español / English)
  langHeader = new brls::Header();
  langHeader->setTitle(tr("settings.language_header"));
  langHeader->setSubtitle(tr("settings.language_desc"));
  langHeader->setHeight(48);
  langHeader->setMarginBottom(10);
  content->addView(langHeader);

  auto* langRow = new brls::Box();
  langRow->setAxis(brls::Axis::ROW);
  langRow->setAlignItems(brls::AlignItems::CENTER);
  langRow->setMarginBottom(24);

  auto currentLang = switube::I18n::getInstance().getLanguage();

  chipEs = new swui::FilterChip(tr("settings.lang_es"), [this]() {
    switube::I18n::getInstance().setLanguage(switube::Language::Spanish);
    brls::Application::notify(tr("settings.notify_lang"));
  });
  chipEs->setSelected(currentLang == switube::Language::Spanish);
  chipEs->setWidth(160);
  chipEs->setMarginRight(16);
  langRow->addView(chipEs);

  chipEn = new swui::FilterChip(tr("settings.lang_en"), [this]() {
    switube::I18n::getInstance().setLanguage(switube::Language::English);
    brls::Application::notify(tr("settings.notify_lang"));
  });
  chipEn->setSelected(currentLang == switube::Language::English);
  chipEn->setWidth(160);
  langRow->addView(chipEn);

  content->addView(langRow);

  // 2. Información y Créditos
  auto* infoBox = swui::makeStatePanel(
      tr("settings.about_title"),
      tr("settings.about_desc"),
      swui::surfaceRaised(),
      &infoTitleLabel,
      &infoDescLabel);
  infoBox->setMarginBottom(16);
  content->addView(infoBox);

  auto token = alive;
  i18nObserverId = switube::I18n::getInstance().addObserver([this, token]() {
    brls::sync([this, token]() {
      if (token->load()) {
        updateLabels();
      }
    });
  });
}

SettingsTab::~SettingsTab() {
  alive->store(false);
  switube::I18n::getInstance().removeObserver(i18nObserverId);
}

brls::View* SettingsTab::getDefaultFocus() {
  auto currentLang = switube::I18n::getInstance().getLanguage();
  if (currentLang == switube::Language::Spanish && chipEs) return chipEs;
  if (currentLang == switube::Language::English && chipEn) return chipEn;
  if (chipEs) return chipEs;
  return brls::Box::getDefaultFocus();
}

void SettingsTab::updateLabels() {
  if (titleLabel) titleLabel->setText(tr("settings.title"));
  if (subtitleLabel) subtitleLabel->setText(tr("settings.subtitle"));

  if (langHeader) {
    langHeader->setTitle(tr("settings.language_header"));
    langHeader->setSubtitle(tr("settings.language_desc"));
  }

  auto currentLang = switube::I18n::getInstance().getLanguage();
  if (chipEs) {
    chipEs->setText(tr("settings.lang_es"));
    chipEs->setSelected(currentLang == switube::Language::Spanish);
  }
  if (chipEn) {
    chipEn->setText(tr("settings.lang_en"));
    chipEn->setSelected(currentLang == switube::Language::English);
  }

  if (infoTitleLabel) infoTitleLabel->setText(tr("settings.about_title"));
  if (infoDescLabel) infoDescLabel->setText(tr("settings.about_desc"));

  // Conservar el foco activamente en el chip de idioma actual para que los controles no se desconecten
  auto* activeChip = (currentLang == switube::Language::Spanish) ? chipEs : chipEn;
  if (activeChip) {
    brls::Application::giveFocus(activeChip);
  }
}
