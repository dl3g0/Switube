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
#include "update.hpp"

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

  statusLabel = subtitleLabel;

  auto* scroll = new brls::ScrollingFrame();
  scroll->setGrow(1.0f);
  scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
  content = new brls::Box();
  content->setAxis(brls::Axis::COLUMN);
  content->setPadding(4, swui::kPagePadding, 34, swui::kPagePadding);
  scroll->setContentView(content);
  addView(scroll);

  auto token = alive;
  lastSignedInState = switube::YtEngine::getInstance().getClient().isSignedIn();
  i18nObserverId = switube::I18n::getInstance().addObserver([this, token]() {
    brls::sync([this, token]() {
      if (token->load()) {
        updateLabels();
        load();
      }
    });
  });

  engineObserverId = switube::YtEngine::getInstance().addObserver([this, token]() {
    brls::sync([this, token]() {
      if (!token->load()) return;
      bool signedIn = switube::YtEngine::getInstance().getClient().isSignedIn();
      if (signedIn != lastSignedInState) {
        lastSignedInState = signedIn;
        load();
      }
    });
  });

  load();
}

HomeTab::~HomeTab() {
  alive->store(false);
  switube::I18n::getInstance().removeObserver(i18nObserverId);
  switube::YtEngine::getInstance().removeObserver(engineObserverId);
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
  continuationToken.clear();
  isLoadingMore = false;
  auto token = alive;
  launchWorkerWithStack([this, token]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    try {
      nxlog::push("Fetching Home Feed from YouTube Music...");
      std::vector<switube::YtShelf> shelves;
      std::string nextTok;
      bool ok = switube::YtEngine::getInstance().getClient().getHomeFeed(shelves, nextTok, "");
      nxlog::pushf("Home Feed result: %s (shelves: %zu, hasMore: %s)",
                   ok ? "OK" : "FAILED", shelves.size(), nextTok.empty() ? "NO" : "YES");

      brls::sync([this, token, ok, shelves = std::move(shelves), nextTok]() {
        if (!token->load()) return;
        continuationToken = nextTok;
        if (ok && !shelves.empty()) {
          statusLabel->setText(tr("home.subtitle"));
          renderHome(shelves, false);
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

void HomeTab::loadMore() {
  if (continuationToken.empty() || isLoadingMore) return;
  isLoadingMore = true;
  std::string tokToFetch = continuationToken;
  auto token = alive;

  launchWorkerWithStack([this, token, tokToFetch]() {
    try {
      std::vector<switube::YtShelf> moreShelves;
      std::string nextTok;
      bool ok = switube::YtEngine::getInstance().getClient().getHomeFeed(moreShelves, nextTok, tokToFetch);

      brls::sync([this, token, ok, moreShelves = std::move(moreShelves), nextTok]() {
        if (!token->load()) return;
        isLoadingMore = false;
        continuationToken = nextTok;
        if (ok && !moreShelves.empty()) {
          renderHome(moreShelves, true);
        }
      });
    } catch (...) {
      brls::sync([this, token]() {
        if (token->load()) isLoadingMore = false;
      });
    }
  });
}

void HomeTab::renderHome(const std::vector<switube::YtShelf>& shelves, bool append) {
  if (!append) {
    swui::safeClearViews(content, refreshBtn);
    scrollSentinel = nullptr;
    currentShelves.clear();

    if (shelves.empty()) {
      auto* emptyNotice = swui::makeStatePanel(
          tr("home.empty_title"),
          tr("home.empty_desc"));
      content->addView(emptyNotice);
      return;
    }
  } else {
    if (scrollSentinel) {
      swui::safeRemoveView(content, scrollSentinel, refreshBtn);
      scrollSentinel = nullptr;
    }
  }

  size_t startShelfIdx = currentShelves.size();
  for (const auto& shelf : shelves) {
    if (shelf.tracks.empty()) continue;
    bool dup = false;
    for (const auto& existing : currentShelves) {
      if (existing.title == shelf.title) {
        dup = true;
        break;
      }
    }
    if (!dup) {
      currentShelves.push_back(shelf);
    }
  }

  for (size_t sIdx = startShelfIdx; sIdx < currentShelves.size(); ++sIdx) {
    const auto& shelf = currentShelves[sIdx];
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
      card->getFocusEvent()->subscribe([this, sIdx](brls::View*) {
        if (!continuationToken.empty() && !isLoadingMore && sIdx + 2 >= currentShelves.size()) {
          loadMore();
        }
      });
      section->addCard(card);
    }
    content->addView(section);
  }

  if (!continuationToken.empty()) {
    scrollSentinel = new swui::ScrollSentinel([this]() {
      loadMore();
    });
    content->addView(scrollSentinel);
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
  isLoadingMore = false;
  currentResults.clear();
  swui::safeClearViews(content, searchBtn);
  loadMoreBtn = nullptr;
  scrollSentinel = nullptr;

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
  if (continuationToken.empty() || currentQuery.empty() || isLoadingMore) return;
  isLoadingMore = true;

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
        isLoadingMore = false;
        continuationToken = nextTok;
        if (ok && !newTracks.empty()) {
          renderResults(newTracks, true);
        } else {
          if (loadMoreBtn) {
            swui::safeRemoveView(content, loadMoreBtn, searchBtn);
            loadMoreBtn = nullptr;
          }
          if (scrollSentinel) {
            swui::safeRemoveView(content, scrollSentinel, searchBtn);
            scrollSentinel = nullptr;
          }
        }
      });
    } catch (const std::exception& e) {
      nxlog::pushf("SearchTab loadMore error: %s", e.what());
      brls::sync([this, token]() {
        if (token->load()) isLoadingMore = false;
      });
    }
  });
}

void SearchTab::renderResults(const std::vector<switube::YtTrack>& tracks, bool append) {
  if (!append) {
    swui::safeClearViews(content, searchBtn);
    currentResults.clear();
    loadMoreBtn = nullptr;
    scrollSentinel = nullptr;
  } else {
    if (loadMoreBtn) {
      swui::safeRemoveView(content, loadMoreBtn, searchBtn);
      loadMoreBtn = nullptr;
    }
    if (scrollSentinel) {
      swui::safeRemoveView(content, scrollSentinel, searchBtn);
      scrollSentinel = nullptr;
    }
  }

  size_t startIdx = currentResults.size();
  for (size_t i = 0; i < tracks.size(); ++i) {
    bool dup = false;
    for (const auto& existing : currentResults) {
      if (existing.id == tracks[i].id) {
        dup = true;
        break;
      }
    }
    if (!dup) {
      currentResults.push_back(tracks[i]);
    }
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
    row->getFocusEvent()->subscribe([this, i](brls::View*) {
      if (!continuationToken.empty() && !isLoadingMore && i + 5 >= currentResults.size()) {
        loadMore();
      }
    });
    content->addView(row);
  }

  statusLabel->setText(tr("search.results_count", currentQuery, std::to_string(currentResults.size())));

  if (!continuationToken.empty()) {
    scrollSentinel = new swui::ScrollSentinel([this]() {
      loadMore();
    });
    content->addView(scrollSentinel);
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
  top->setPadding(20, swui::kPagePadding, 10, swui::kPagePadding);
  top->addView(makeHeading(tr("library.title"), tr("library.subtitle"), &titleLabel, &subtitleLabel));

  syncBtn = swui::makeActionButton(
      tr("library.sync"),
      [this](brls::View*) {
        if (switube::YtEngine::getInstance().getClient().isSignedIn()) {
          switube::YtEngine::getInstance().syncAccountLibrary();
          brls::Application::notify(tr("library.syncing"));
        } else {
          renderedFavoriteIds.clear();
          renderedPlaylistIds.clear();
          refresh();
        }
        return true;
      },
      false, &syncBtnLabel);
  syncBtn->setWidth(160);
  top->addView(syncBtn);
  addView(top);

  auto* chipsRow = new brls::Box();
  chipsRow->setAxis(brls::Axis::ROW);
  chipsRow->setAlignItems(brls::AlignItems::CENTER);
  chipsRow->setPadding(0, swui::kPagePadding, 12, swui::kPagePadding);

  chipFavorites = new swui::FilterChip("♥ " + tr("library.tab_favorites"), [this]() {
    if (showPlaylists) {
      showPlaylists = false;
      chipFavorites->setSelected(true);
      chipPlaylists->setSelected(false);
      refresh();
    }
  });
  chipFavorites->setSelected(true);
  chipFavorites->setMarginRight(12);
  chipsRow->addView(chipFavorites);

  chipPlaylists = new swui::FilterChip("♫ " + tr("library.tab_playlists"), [this]() {
    if (!showPlaylists) {
      showPlaylists = true;
      chipFavorites->setSelected(false);
      chipPlaylists->setSelected(true);
      refresh();
    }
  });
  chipPlaylists->setSelected(false);
  chipsRow->addView(chipPlaylists);

  addView(chipsRow);

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
      if (lastRenderedMode == showPlaylists && !content->getChildren().empty()) {
        if (!showPlaylists) {
          auto favs = switube::YtEngine::getInstance().getFavorites();
          if (favs.size() == renderedFavoriteIds.size()) {
            bool same = true;
            for (size_t i = 0; i < favs.size(); ++i) {
              if (favs[i].id != renderedFavoriteIds[i]) {
                same = false;
                break;
              }
            }
            if (same) return;
          }
        } else {
          auto pls = switube::YtEngine::getInstance().getUserPlaylists();
          if (pls.size() == renderedPlaylistIds.size()) {
            bool same = true;
            for (size_t i = 0; i < pls.size(); ++i) {
              if (pls[i].id != renderedPlaylistIds[i]) {
                same = false;
                break;
              }
            }
            if (same) return;
          }
        }
      }
      refresh();
    });
  });

  i18nObserverId = switube::I18n::getInstance().addObserver([this, token]() {
    brls::sync([this, token]() {
      if (token->load()) {
        renderedFavoriteIds.clear();
        renderedPlaylistIds.clear();
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
  if (syncBtnLabel) syncBtnLabel->setText(tr("library.sync"));
  if (chipFavorites) chipFavorites->setText("♥ " + tr("library.tab_favorites"));
  if (chipPlaylists) chipPlaylists->setText("♫ " + tr("library.tab_playlists"));
}

brls::View* LibraryTab::getDefaultFocus() {
  if (!content->getChildren().empty()) return content->getChildren()[0];
  if (chipFavorites) return chipFavorites;
  return brls::Box::getDefaultFocus();
}

void LibraryTab::refresh() {
  auto& engine = switube::YtEngine::getInstance();

  if (showPlaylists) {
    auto playlists = engine.getUserPlaylists();
    bool hasMore = engine.hasMorePlaylists();

    // Evitar reconstruir toda la vista en notificaciones de reproducción si la lista no cambió
    if (lastRenderedMode == showPlaylists &&
        lastRenderedHasMore == hasMore &&
        playlists.size() == renderedPlaylistIds.size() &&
        !playlists.empty()) {
      bool same = true;
      for (size_t i = 0; i < playlists.size(); ++i) {
        if (playlists[i].id != renderedPlaylistIds[i]) {
          same = false;
          break;
        }
      }
      if (same) return;
    }

    if (scrollSentinel) {
      swui::safeRemoveView(content, scrollSentinel, chipPlaylists);
      scrollSentinel = nullptr;
    }

    // Verificar si es una extensión por paginación (append sin limpiar vistas)
    bool isAppend = (lastRenderedMode == showPlaylists &&
                     !renderedPlaylistIds.empty() &&
                     playlists.size() > renderedPlaylistIds.size());
    if (isAppend) {
      for (size_t i = 0; i < renderedPlaylistIds.size(); ++i) {
        if (playlists[i].id != renderedPlaylistIds[i]) {
          isAppend = false;
          break;
        }
      }
    }

    size_t startIdx = 0;
    bool hadFocus = false;
    size_t focusedChildIdx = 0;

    if (isAppend) {
      startIdx = renderedPlaylistIds.size();
    } else {
      hadFocus = content && (content->isFocused() || content->isChildFocused());
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
      if (content) content->clearViews();
    }

    lastRenderedMode = showPlaylists;
    lastRenderedHasMore = hasMore;
    renderedPlaylistIds.clear();
    renderedPlaylistIds.reserve(playlists.size());
    for (const auto& p : playlists) {
      renderedPlaylistIds.push_back(p.id);
    }

    if (playlists.empty()) {
      auto* emptyNotice = swui::makeStatePanel(
          tr("library.playlists_empty_title"),
          tr("library.playlists_empty_desc"));
      emptyNotice->setFocusable(true);
      content->addView(emptyNotice);
      if (hadFocus) {
        brls::Application::giveFocus(emptyNotice);
      }
      return;
    }

    for (size_t i = startIdx; i < playlists.size(); ++i) {
      const auto& item = playlists[i];
      auto* row = new swui::TrackRow();
      row->setTrack(item, i, [item]() {
        if (item.isArtist) {
          brls::Application::pushActivity(new ArtistActivity(item.id, item.title, item.imageUrl));
        } else {
          brls::Application::pushActivity(new TrackListActivity(item));
        }
      });
      row->getFocusEvent()->subscribe([this, i](brls::View*) {
        auto& eng = switube::YtEngine::getInstance();
        if (eng.hasMorePlaylists() && !eng.isLoadingMorePlaylists() && i + 5 >= renderedPlaylistIds.size()) {
          eng.loadMorePlaylists();
        }
      });
      content->addView(row);
    }

    if (hasMore) {
      scrollSentinel = new swui::ScrollSentinel([]() {
        switube::YtEngine::getInstance().loadMorePlaylists();
      });
      content->addView(scrollSentinel);
    }

    if (!isAppend && hadFocus) {
      if (auto* target = swui::nearestFocusableChild(content, focusedChildIdx)) {
        brls::Application::giveFocus(target);
      }
    }
  } else {
    auto favorites = engine.getFavorites();
    bool hasMore = engine.hasMoreFavorites();

    if (lastRenderedMode == showPlaylists &&
        lastRenderedHasMore == hasMore &&
        favorites.size() == renderedFavoriteIds.size() &&
        !favorites.empty()) {
      bool same = true;
      for (size_t i = 0; i < favorites.size(); ++i) {
        if (favorites[i].id != renderedFavoriteIds[i]) {
          same = false;
          break;
        }
      }
      if (same) return;
    }

    if (scrollSentinel) {
      swui::safeRemoveView(content, scrollSentinel, chipFavorites);
      scrollSentinel = nullptr;
    }

    // Verificar si es una extensión por paginación (append sin limpiar vistas)
    bool isAppend = (lastRenderedMode == showPlaylists &&
                     !renderedFavoriteIds.empty() &&
                     favorites.size() > renderedFavoriteIds.size());
    if (isAppend) {
      for (size_t i = 0; i < renderedFavoriteIds.size(); ++i) {
        if (favorites[i].id != renderedFavoriteIds[i]) {
          isAppend = false;
          break;
        }
      }
    }

    size_t startIdx = 0;
    bool hadFocus = false;
    size_t focusedChildIdx = 0;

    if (isAppend) {
      startIdx = renderedFavoriteIds.size();
    } else {
      hadFocus = content && (content->isFocused() || content->isChildFocused());
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
      if (content) content->clearViews();
    }

    lastRenderedMode = showPlaylists;
    lastRenderedHasMore = hasMore;
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

    if (!isAppend) {
      auto* actionRow = new brls::Box();
      actionRow->setAxis(brls::Axis::ROW);
      actionRow->setAlignItems(brls::AlignItems::CENTER);
      actionRow->setMarginBottom(14);

      auto* playOrderBtn = swui::makeActionButton("▶ " + tr("library.play_order"), [](brls::View*) {
        auto allFavs = switube::YtEngine::getInstance().getFavorites();
        if (!allFavs.empty()) {
          switube::YtEngine::getInstance().playQueueWithMode(allFavs, 0, false);
        }
        return true;
      }, true);
      auto* playShuffleBtn = swui::makeActionButton(tr("library.play_shuffle"), [](brls::View*) {
        auto allFavs = switube::YtEngine::getInstance().getFavorites();
        if (!allFavs.empty()) {
          switube::YtEngine::getInstance().playQueueShuffled(allFavs);
        }
        return true;
      }, false);
      playShuffleBtn->setMarginLeft(12);

      actionRow->addView(playOrderBtn);
      actionRow->addView(playShuffleBtn);
      content->addView(actionRow);
    }

    for (size_t i = startIdx; i < favorites.size(); ++i) {
      const auto& track = favorites[i];
      auto* row = new swui::TrackRow();
      row->setTrack(track, i, [trackId = track.id, i]() {
        auto allFavs = switube::YtEngine::getInstance().getFavorites();
        if (allFavs.empty()) return;
        size_t idx = (i < allFavs.size() && allFavs[i].id == trackId) ? i : allFavs.size();
        if (idx == allFavs.size()) {
          for (size_t k = 0; k < allFavs.size(); ++k) {
            if (allFavs[k].id == trackId) {
              idx = k;
              break;
            }
          }
        }
        if (idx < allFavs.size()) {
          auto& eng = switube::YtEngine::getInstance();
          eng.playQueueWithMode(allFavs, idx, eng.isShuffle());
        }
      });
      row->getFocusEvent()->subscribe([this, i](brls::View*) {
        auto& eng = switube::YtEngine::getInstance();
        if (eng.hasMoreFavorites() && !eng.isLoadingMoreFavorites() && i + 5 >= renderedFavoriteIds.size()) {
          eng.loadMoreFavorites();
        }
      });
      content->addView(row);
    }

    if (hasMore) {
      scrollSentinel = new swui::ScrollSentinel([]() {
        switube::YtEngine::getInstance().loadMoreFavorites();
      });
      content->addView(scrollSentinel);
    }

    if (!isAppend && hadFocus) {
      if (auto* target = swui::nearestFocusableChild(content, focusedChildIdx)) {
        brls::Application::giveFocus(target);
      }
    }
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
  bool hadFocus = content && (content->isFocused() || content->isChildFocused());
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

  if (content) content->clearViews();
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

  // Botones "En orden" y "Aleatorio"
  auto* actionRow = new brls::Box();
  actionRow->setAxis(brls::Axis::ROW);
  actionRow->setAlignItems(brls::AlignItems::CENTER);
  actionRow->setMarginBottom(14);

  auto* playAllBtn = swui::makeActionButton("▶ " + tr("library.play_order"), [tracks](brls::View*) {
    switube::YtEngine::getInstance().playQueueWithMode(tracks, 0, false);
    return true;
  }, true);
  auto* playShuffleBtn = swui::makeActionButton(tr("downloads.play_shuffle"), [tracks](brls::View*) {
    switube::YtEngine::getInstance().playQueueShuffled(tracks);
    return true;
  }, false);
  playShuffleBtn->setMarginLeft(12);

  actionRow->addView(playAllBtn);
  actionRow->addView(playShuffleBtn);
  content->addView(actionRow);

  for (size_t i = 0; i < tracks.size(); ++i) {
    const auto& track = tracks[i];
    auto* row = new swui::TrackRow();
    row->setTrack(track, i, [tracks, i]() {
      auto& eng = switube::YtEngine::getInstance();
      eng.playQueueWithMode(tracks, i, eng.isShuffle());
    });
    content->addView(row);
  }

  if (hadFocus) {
    if (auto* target = swui::nearestFocusableChild(content, focusedChildIdx)) {
      brls::Application::giveFocus(target);
    }
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
        true, true);

    registerAction(
        tr("now_playing.seek_fwd"), brls::BUTTON_RIGHT,
        [](brls::View*) {
          auto& eng = switube::YtEngine::getInstance();
          auto now = eng.getNowPlaying();
          if (now.positionMs + 10000 < now.durationMs)
            eng.seekTo(now.positionMs + 10000);
          return true;
        },
        true, true);

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
  setMargins(14, 24, 6, 18);
  setPadding(24, 32, 24, 32);
  setCornerRadius(16);
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
  rightCol->setWidth(450);
  rightCol->setMarginLeft(44);
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

  shuffleBtn = swui::makeShuffleButton(
      [](brls::View*) {
        switube::YtEngine::getInstance().toggleShuffle();
        return true;
      },
      &shuffleIcon);
  shuffleBtn->setWidth(52);
  shuffleBtn->setHeight(46);
  shuffleBtn->setCornerRadius(23);
  shuffleBtn->setHighlightCornerRadius(23);
  shuffleBtn->setMarginRight(12);

  prevBtn = swui::makePrevButton([](brls::View*) {
    switube::YtEngine::getInstance().previous();
    return true;
  });
  prevBtn->setWidth(72);
  prevBtn->setHeight(50);
  prevBtn->setCornerRadius(25);
  prevBtn->setMarginRight(14);

  playBtn = swui::makePlayPauseButton(
      [](brls::View*) {
        switube::YtEngine::getInstance().playPause();
        return true;
      },
      &playPauseIcon, true);
  playBtn->setWidth(92);
  playBtn->setHeight(56);
  playBtn->setCornerRadius(28);
  playBtn->setMarginRight(14);

  nextBtn = swui::makeNextButton([](brls::View*) {
    switube::YtEngine::getInstance().next();
    return true;
  });
  nextBtn->setWidth(72);
  nextBtn->setHeight(50);
  nextBtn->setCornerRadius(25);
  nextBtn->setMarginRight(12);

  repeatBtn = swui::makeRepeatButton(
      [](brls::View*) {
        switube::YtEngine::getInstance().cycleRepeatMode();
        return true;
      },
      &repeatIcon);
  repeatBtn->setWidth(52);
  repeatBtn->setHeight(46);
  repeatBtn->setCornerRadius(23);
  repeatBtn->setHighlightCornerRadius(23);

  controls->addView(shuffleBtn);
  controls->addView(prevBtn);
  controls->addView(playBtn);
  controls->addView(nextBtn);
  controls->addView(repeatBtn);
  rightCol->addView(controls);

  auto* actionRow = new brls::Box();
  actionRow->setAxis(brls::Axis::ROW);
  actionRow->setAlignItems(brls::AlignItems::CENTER);
  actionRow->setJustifyContent(brls::JustifyContent::CENTER);
  actionRow->setMarginTop(14);

  favBtn = new brls::Box();
  favBtn->setFocusable(true);
  favBtn->setWidth(64);
  favBtn->setHeight(46);
  favBtn->setCornerRadius(23);
  favBtn->setHighlightCornerRadius(23);
  favBtn->setBackgroundColor(swui::surfaceRaised());
  favBtn->setAlignItems(brls::AlignItems::CENTER);
  favBtn->setJustifyContent(brls::JustifyContent::CENTER);
  favBtn->addGestureRecognizer(new brls::TapGestureRecognizer(favBtn));

  favBtnLabel = swui::makeLabel("♡", 22, swui::textPrimary(), true);
  favBtn->addView(favBtnLabel);

  favBtn->registerClickAction([](brls::View*) {
    switube::YtEngine::getInstance().toggleFavoriteCurrent();
    return true;
  });
  favBtn->registerAction("Me gusta", brls::BUTTON_A, [](brls::View*) {
    switube::YtEngine::getInstance().toggleFavoriteCurrent();
    return true;
  });

  downloadBtn = new brls::Box();
  downloadBtn->setFocusable(true);
  downloadBtn->setWidth(64);
  downloadBtn->setHeight(46);
  downloadBtn->setCornerRadius(23);
  downloadBtn->setHighlightCornerRadius(23);
  downloadBtn->setBackgroundColor(swui::surfaceRaised());
  downloadBtn->setAlignItems(brls::AlignItems::CENTER);
  downloadBtn->setJustifyContent(brls::JustifyContent::CENTER);
  downloadBtn->setMarginLeft(14);
  downloadBtn->addGestureRecognizer(new brls::TapGestureRecognizer(downloadBtn));

  downloadBadge = new swui::DownloadBadge(28.0f);
  downloadBtn->addView(downloadBadge);

  downloadBtn->registerClickAction([this](brls::View*) {
    handleDownloadCurrentTrack();
    return true;
  });
  downloadBtn->registerAction("Descargar", brls::BUTTON_A, [this](brls::View*) {
    handleDownloadCurrentTrack();
    return true;
  });

  actionRow->addView(favBtn);
  actionRow->addView(downloadBtn);
  rightCol->addView(actionRow);

  body->addView(rightCol);
  addView(body);

  // Acciones rápidas de gatillos para buscar (ocultas del menú inferior)
  registerAction(
      "Seek -10s", brls::BUTTON_LT,
      [](brls::View*) {
        auto& eng = switube::YtEngine::getInstance();
        auto now = eng.getNowPlaying();
        if (now.positionMs > 10000) eng.seekTo(now.positionMs - 10000);
        else eng.seekTo(0);
        return true;
      },
      true, true);

  registerAction(
      "Seek +10s", brls::BUTTON_RT,
      [](brls::View*) {
        auto& eng = switube::YtEngine::getInstance();
        auto now = eng.getNowPlaying();
        if (now.positionMs + 10000 < now.durationMs) eng.seekTo(now.positionMs + 10000);
        return true;
      },
      true, true);

  registerAction("Descargar", brls::BUTTON_X, [this](brls::View*) {
    handleDownloadCurrentTrack();
    return true;
  });

  registerAction("Me gusta", brls::BUTTON_Y, [](brls::View*) {
    switube::YtEngine::getInstance().toggleFavoriteCurrent();
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

  const float r = 16.0f;
  nvgSave(vg);

  // Sombra suave flotante del card
  NVGpaint shadow = nvgBoxGradient(vg, x, y + 3.0f, width, height, r, 14.0f,
                                   nvgRGBA(0, 0, 0, 125), nvgRGBA(0, 0, 0, 0));
  nvgBeginPath(vg);
  nvgRect(vg, x - 12.0f, y - 8.0f, width + 24.0f, height + 24.0f);
  nvgRoundedRect(vg, x, y, width, height, r);
  nvgPathWinding(vg, NVG_HOLE);
  nvgFillPaint(vg, shadow);
  nvgFill(vg);

  // Fondo base del card
  nvgBeginPath(vg);
  nvgRoundedRect(vg, x, y, width, height, r);
  NVGpaint baseGrad = nvgRadialGradient(vg, x + width * 0.65f, y + height * 0.30f,
                                        height * 0.15f, width * 0.85f,
                                        nvgRGBA(52, 12, 20, 225), nvgRGBA(14, 13, 19, 238));
  nvgFillPaint(vg, baseGrad);
  nvgFill(vg);

  int tex = artwork ? artwork->getTexture() : 0;
  if (tex > 0) {
    nvgIntersectScissor(vg, x, y, width, height);
    float side = std::max(width, height);
    float imgX = x + (width - side) * 0.5f;
    float imgY = y + (height - side) * 0.5f;

    // Carátula ambiental recortada exactamente a las esquinas redondeadas del card
    NVGpaint imgPaint = nvgImagePattern(vg, imgX, imgY, side, side, 0.0f, tex, 0.24f);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, width, height, r);
    nvgFillPaint(vg, imgPaint);
    nvgFill(vg);

    // Degradado interno carmesí -> obsidiana
    NVGpaint darkOverlay = nvgLinearGradient(vg, x, y, x, y + height,
                                             nvgRGBA(24, 8, 12, 145),
                                             nvgRGBA(10, 10, 14, 235));
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, width, height, r);
    nvgFillPaint(vg, darkOverlay);
    nvgFill(vg);

    // Viñeta radial suave enfocada en la carátula
    NVGpaint radialVignette = nvgRadialGradient(vg, x + width * 0.30f, y + height * 0.48f,
                                                height * 0.20f, width * 0.78f,
                                                nvgRGBA(0, 0, 0, 15),
                                                nvgRGBA(8, 8, 12, 185));
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, width, height, r);
    nvgFillPaint(vg, radialVignette);
    nvgFill(vg);
  }

  // Borde cristalino 1px igual al sidebar y al MiniPlayer
  nvgBeginPath(vg);
  nvgRoundedRect(vg, x, y, width, height, r);
  nvgStrokeWidth(vg, 1.0f);
  nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 24));
  nvgStroke(vg);

  nvgRestore(vg);

  brls::Box::draw(vg, x, y, width, height, style, ctx);
}

void NowPlayingTab::refresh() {
  auto& engine = switube::YtEngine::getInstance();
  const auto now = engine.getNowPlaying();
  auto state = engine.getState();

  if (shuffleIcon) {
    shuffleIcon->setActive(engine.isShuffle());
  }
  if (repeatIcon) {
    switch (engine.getRepeatMode()) {
      case switube::YtRepeatMode::One:
        repeatIcon->setMode(swui::RepeatMode::One);
        break;
      case switube::YtRepeatMode::All:
        repeatIcon->setMode(swui::RepeatMode::All);
        break;
      default:
        repeatIcon->setMode(swui::RepeatMode::Off);
        break;
    }
  }

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

    if (favBtn && favBtnLabel) {
      favBtn->setVisibility(brls::Visibility::VISIBLE);
      favBtnLabel->setText(now.isFavorite ? "♥" : "♡");
      favBtnLabel->setTextColor(now.isFavorite ? swui::accent() : swui::textPrimary());
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
      if (hint != lastDownloadHint) {
        lastDownloadHint = hint;
        updateActionHint(brls::BUTTON_X, hint);
        if (this->isFocused() || this->isChildFocused()) {
          brls::Application::getGlobalHintsUpdateEvent()->fire();
        }
      }
    }
  } else {
    if (favBtn) favBtn->setVisibility(brls::Visibility::GONE);
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

  // 1. Cuenta de YouTube Music (Google TV QR Login)
  accountHeader = new brls::Header();
  accountHeader->setTitle(tr("settings.account_header"));
  accountHeader->setSubtitle(tr("settings.account_desc"));
  accountHeader->setHeight(48);
  accountHeader->setMarginBottom(10);
  content->addView(accountHeader);

  accountContainer = new brls::Box();
  accountContainer->setAxis(brls::Axis::COLUMN);
  accountContainer->setMarginBottom(24);
  content->addView(accountContainer);

  // 2. Selector de Idioma (Español / English)
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

  // 3. Actualizaciones (GitHub Releases)
  updateHeader = new brls::Header();
  updateHeader->setTitle(tr("settings.update_header"));
  updateHeader->setSubtitle(tr("settings.update_desc"));
  updateHeader->setHeight(48);
  updateHeader->setMarginBottom(10);
  content->addView(updateHeader);

  auto* updateCard = new brls::Box();
  updateCard->setCornerRadius(16);
  updateCard->setBorderThickness(1.0f);
  updateCard->setBorderColor(nvgRGBA(255, 255, 255, 20));
  updateCard->setBackgroundColor(swui::surfaceRaised());
  updateCard->setAxis(brls::Axis::COLUMN);
  updateCard->setPadding(20, 24, 20, 24);
  updateCard->setMarginBottom(24);

  std::string verKey = switube::update::hasPending() ? "settings.version_pending" : "settings.version_label";
  versionLabel = swui::makeLabel(tr(verKey, APP_VERSION), 17, swui::textPrimary(), true);
  versionLabel->setMarginBottom(14);
  updateCard->addView(versionLabel);

  auto* updateBtnsRow = new brls::Box();
  updateBtnsRow->setAxis(brls::Axis::ROW);
  updateBtnsRow->setAlignItems(brls::AlignItems::CENTER);

  auto tabAlive = alive;
  checkUpdatesBtn = swui::makeActionButton(
      tr("settings.check_updates_btn"),
      [this, tabAlive](brls::View*) {
        if (checkingUpdates) return true;
        checkingUpdates = true;
        if (!checkUpdatesBtn->getChildren().empty()) {
          if (auto* lbl = dynamic_cast<brls::Label*>(checkUpdatesBtn->getChildren()[0])) {
            lbl->setText(tr("settings.checking_updates"));
          }
        }
        switube::update::checkAsync([this, tabAlive](switube::update::Release r) {
          if (!tabAlive->load()) return;
          checkingUpdates = false;
          if (checkUpdatesBtn && !checkUpdatesBtn->getChildren().empty()) {
            if (auto* lbl = dynamic_cast<brls::Label*>(checkUpdatesBtn->getChildren()[0])) {
              lbl->setText(tr("settings.check_updates_btn"));
            }
          }
          if (!r.ok) {
            auto* d = new brls::Dialog(tr("update.check_failed") + r.error);
            d->addButton(tr("update.ok"), []() {});
            d->open();
          } else if (!r.newer) {
            auto* d = new brls::Dialog(tr("update.up_to_date", APP_VERSION));
            d->addButton(tr("update.ok"), []() {});
            d->open();
          } else if (r.url.empty()) {
            auto* d = new brls::Dialog(tr("update.no_nro_asset", r.version));
            d->addButton(tr("update.ok"), []() {});
            d->open();
          } else {
            switube::update::promptInstall(r);
          }
        });
        return true;
      },
      true);
  checkUpdatesBtn->setWidth(250);
  checkUpdatesBtn->setMarginRight(14);
  updateBtnsRow->addView(checkUpdatesBtn);

  viewChangelogBtn = swui::makeActionButton(
      tr("settings.view_changelog_btn"),
      [](brls::View*) {
        switube::update::showChangelog(APP_VERSION);
        return true;
      },
      false);
  viewChangelogBtn->setWidth(250);
  updateBtnsRow->addView(viewChangelogBtn);

  updateCard->addView(updateBtnsRow);
  content->addView(updateCard);

  // 4. Información y Créditos
  auto* infoBox = swui::makeStatePanel(
      tr("settings.about_title"),
      tr("settings.about_desc"),
      swui::surfaceRaised(),
      &infoTitleLabel,
      &infoDescLabel);
  infoBox->setMarginBottom(16);
  content->addView(infoBox);

  refreshAccountSection();

  auto token = alive;
  i18nObserverId = switube::I18n::getInstance().addObserver([this, token]() {
    brls::sync([this, token]() {
      if (token->load()) {
        updateLabels();
        refreshAccountSection();
      }
    });
  });

  engineObserverId = switube::YtEngine::getInstance().addObserver([this, token]() {
    brls::sync([this, token]() {
      if (!token->load() || waitingForQr) return;
      auto& eng = switube::YtEngine::getInstance();
      bool signedIn = eng.getClient().isSignedIn();
      bool syncing = eng.isSyncingLibrary();
      std::string accName = eng.getClient().getAccountInfo().name;
      if (signedIn != lastAccountSignedIn || syncing != lastAccountSyncing || accName != lastAccountName) {
        refreshAccountSection();
      }
    });
  });
}

SettingsTab::~SettingsTab() {
  alive->store(false);
  if (qrPollingActive) {
    qrPollingActive->store(false);
  }
  switube::I18n::getInstance().removeObserver(i18nObserverId);
  switube::YtEngine::getInstance().removeObserver(engineObserverId);
}

void SettingsTab::startQrLinking() {
  if (waitingForQr) return;
  waitingForQr = true;
  if (qrPollingActive) {
    qrPollingActive->store(false);
  }
  auto pollToken = std::make_shared<std::atomic_bool>(true);
  qrPollingActive = pollToken;
  activeDeviceCode = switube::YtDeviceCode{};
  refreshAccountSection();

  auto tabAlive = alive;
  launchWorkerWithStack([this, tabAlive, pollToken]() {
    switube::YtDeviceCode code;
    bool ok = switube::YtEngine::getInstance().getClient().requestDeviceCode(code);
    if (!tabAlive->load() || !pollToken->load()) return;

    if (!ok) {
      brls::sync([this, tabAlive, pollToken]() {
        if (!tabAlive->load() || !pollToken->load()) return;
        waitingForQr = false;
        brls::Application::notify("Error de conexión al generar el código QR");
        refreshAccountSection();
      });
      return;
    }

    brls::sync([this, tabAlive, pollToken, code]() {
      if (!tabAlive->load() || !pollToken->load()) return;
      activeDeviceCode = code;
      refreshAccountSection();
    });

    int intervalSec = std::max(4, code.intervalSec);
    int elapsedSec = 0;
    while (tabAlive->load() && pollToken->load() && elapsedSec < code.expiresInSec) {
      for (int s = 0; s < intervalSec * 10 && tabAlive->load() && pollToken->load(); ++s) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }
      if (!tabAlive->load() || !pollToken->load()) return;
      elapsedSec += intervalSec;

      int status = switube::YtEngine::getInstance().getClient().pollDeviceToken(code.deviceCode);
      if (status == 1) {
        brls::sync([this, tabAlive, pollToken]() {
          if (!tabAlive->load() || !pollToken->load()) return;
          pollToken->store(false);
          waitingForQr = false;
          switube::YtEngine::getInstance().syncAccountLibrary();
          brls::Application::notify(tr("settings.account_notify_linked"));
          refreshAccountSection();
        });
        return;
      } else if (status < 0) {
        brls::sync([this, tabAlive, pollToken]() {
          if (!tabAlive->load() || !pollToken->load()) return;
          pollToken->store(false);
          waitingForQr = false;
          refreshAccountSection();
        });
        return;
      }
    }
  });
}

void SettingsTab::cancelQrLinking() {
  if (qrPollingActive) {
    qrPollingActive->store(false);
  }
  waitingForQr = false;
  activeDeviceCode = switube::YtDeviceCode{};
  refreshAccountSection();
}

void SettingsTab::refreshAccountSection() {
  if (!accountContainer) return;

  bool hadAccountFocus = accountContainer->isFocused() || accountContainer->isChildFocused();
  if (hadAccountFocus) {
    brls::Application::giveFocus(nullptr);
  }

  accountContainer->clearViews();
  primaryAccountFocus = nullptr;

  auto& engine = switube::YtEngine::getInstance();
  auto& client = engine.getClient();
  lastAccountSignedIn = client.isSignedIn();
  lastAccountSyncing = engine.isSyncingLibrary();
  lastAccountName = client.getAccountInfo().name;

  auto* card = new brls::Box();
  card->setCornerRadius(16);
  card->setBorderThickness(1.0f);
  card->setBorderColor(nvgRGBA(255, 255, 255, 20));
  card->setBackgroundColor(swui::surfaceRaised());
  card->setAxis(brls::Axis::COLUMN);
  card->setPadding(22, 24, 22, 24);

  if (waitingForQr) {
    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);

    // Marco blanco para que el código QR tenga contraste perfecto al escanear con la cámara
    auto* qrBox = new brls::Box();
    qrBox->setWidth(188);
    qrBox->setHeight(188);
    qrBox->setCornerRadius(16);
    qrBox->setBackgroundColor(nvgRGB(255, 255, 255));
    qrBox->setAlignItems(brls::AlignItems::CENTER);
    qrBox->setJustifyContent(brls::JustifyContent::CENTER);
    qrBox->setMarginRight(26);

    auto* qrImg = new brls::Image();
    qrImg->setWidth(172);
    qrImg->setHeight(172);
    qrImg->setScalingType(brls::ImageScalingType::FIT);
    qrBox->addView(qrImg);

    if (!activeDeviceCode.userCode.empty()) {
      std::string targetUrl = "https://www.google.com/device?user_code=" + activeDeviceCode.userCode;
      std::string qrApiUrl =
          "https://api.qrserver.com/v1/create-qr-code/?size=280x280&margin=8&data=https%3A%2F%2Fwww.google.com%2Fdevice%3Fuser_code%3D" +
          activeDeviceCode.userCode;
      swui::loadRemoteImage(qrImg, qrApiUrl, 280);
    }

    row->addView(qrBox);

    auto* infoCol = new brls::Box();
    infoCol->setAxis(brls::Axis::COLUMN);
    infoCol->setGrow(1.0f);

    auto* step1 = swui::makeLabel(tr("settings.account_qr_step1"), 15, swui::textSecondary());
    infoCol->addView(step1);

    auto* urlLabel = swui::makeLabel("google.com/device  (o youtube.com/activate)", 18, swui::accent(), true);
    urlLabel->setMarginTop(4);
    urlLabel->setMarginBottom(12);
    infoCol->addView(urlLabel);

    auto* step2 = swui::makeLabel(tr("settings.account_qr_step2"), 15, swui::textSecondary());
    infoCol->addView(step2);

    std::string codeDisplay = activeDeviceCode.userCode.empty() ? "..." : activeDeviceCode.userCode;
    auto* codeLabel = swui::makeLabel(codeDisplay, 30, swui::textPrimary(), true);
    codeLabel->setMarginTop(6);
    codeLabel->setMarginBottom(10);
    infoCol->addView(codeLabel);

    auto* waitLabel = swui::makeLabel(tr("settings.account_qr_waiting"), 14, swui::textSecondary(), true);
    waitLabel->setMarginBottom(14);
    infoCol->addView(waitLabel);

    auto* cancelBtn = swui::makeActionButton(
        tr("settings.account_cancel_btn"),
        [this](brls::View*) {
          cancelQrLinking();
          return true;
        },
        false);
    cancelBtn->setWidth(180);
    infoCol->addView(cancelBtn);
    primaryAccountFocus = cancelBtn;

    row->addView(infoCol);
    card->addView(row);
  } else if (client.isSignedIn()) {
    auto info = client.getAccountInfo();

    auto* profileRow = new brls::Box();
    profileRow->setAxis(brls::Axis::ROW);
    profileRow->setAlignItems(brls::AlignItems::CENTER);
    profileRow->setMarginBottom(16);

    auto* avatar = new brls::Image();
    avatar->setWidth(64);
    avatar->setHeight(64);
    avatar->setCornerRadius(32);
    avatar->setScalingType(brls::ImageScalingType::FILL);
    avatar->setMarginRight(18);
    if (!info.photoUrl.empty()) {
      swui::loadRemoteImage(avatar, info.photoUrl, 160);
    }
    profileRow->addView(avatar);

    auto* textCol = new brls::Box();
    textCol->setAxis(brls::Axis::COLUMN);
    textCol->setGrow(1.0f);

    std::string dispName = info.name.empty() ? "YouTube Music" : info.name;
    auto* nameLbl = swui::makeLabel(dispName, 21, swui::textPrimary(), true);
    textCol->addView(nameLbl);

    if (!info.handle.empty()) {
      auto* handleLbl = swui::makeLabel(info.handle, 14, swui::textSecondary(), true);
      handleLbl->setMarginTop(3);
      textCol->addView(handleLbl);
    }

    std::string badgeTxt = engine.isSyncingLibrary()
                               ? tr("library.syncing")
                               : ("✓ " + tr("settings.account_linked_badge"));
    auto* badgeLbl = swui::makeLabel(badgeTxt, 14, nvgRGB(46, 213, 115), true);
    badgeLbl->setMarginTop(5);
    textCol->addView(badgeLbl);

    profileRow->addView(textCol);
    card->addView(profileRow);

    auto* actionsRow = new brls::Box();
    actionsRow->setAxis(brls::Axis::ROW);
    actionsRow->setAlignItems(brls::AlignItems::CENTER);

    auto* syncNowBtn = swui::makeActionButton(
        tr("settings.account_sync_btn"),
        [](brls::View*) {
          switube::YtEngine::getInstance().syncAccountLibrary();
          brls::Application::notify(tr("library.syncing"));
          return true;
        },
        true);
    syncNowBtn->setWidth(220);
    syncNowBtn->setMarginRight(14);
    actionsRow->addView(syncNowBtn);

    auto* logoutBtn = swui::makeActionButton(
        tr("settings.account_logout_btn"),
        [this](brls::View*) {
          auto& eng = switube::YtEngine::getInstance();
          eng.getClient().logout();
          eng.clearAccountLibrary();
          brls::Application::notify(tr("settings.account_notify_logout"));
          refreshAccountSection();
          return true;
        },
        false);
    logoutBtn->setWidth(180);
    actionsRow->addView(logoutBtn);

    primaryAccountFocus = syncNowBtn;
    card->addView(actionsRow);
  } else {
    auto* guestTitle = swui::makeLabel(tr("settings.account_guest_title"), 20, swui::textPrimary(), true);
    card->addView(guestTitle);

    auto* guestDesc = swui::makeLabel(tr("settings.account_guest_desc"), 15, swui::textSecondary());
    guestDesc->setMarginTop(6);
    guestDesc->setMarginBottom(16);
    card->addView(guestDesc);

    auto* linkBtn = swui::makeActionButton(
        tr("settings.account_link_btn"),
        [this](brls::View*) {
          startQrLinking();
          return true;
        },
        true);
    linkBtn->setWidth(260);
    card->addView(linkBtn);
    primaryAccountFocus = linkBtn;
  }

  accountContainer->addView(card);

  if (hadAccountFocus && primaryAccountFocus) {
    brls::Application::giveFocus(primaryAccountFocus);
  }
}

brls::View* SettingsTab::getDefaultFocus() {
  if (primaryAccountFocus) return primaryAccountFocus;
  auto currentLang = switube::I18n::getInstance().getLanguage();
  if (currentLang == switube::Language::Spanish && chipEs) return chipEs;
  if (currentLang == switube::Language::English && chipEn) return chipEn;
  if (chipEs) return chipEs;
  return brls::Box::getDefaultFocus();
}

void SettingsTab::updateLabels() {
  if (titleLabel) titleLabel->setText(tr("settings.title"));
  if (subtitleLabel) subtitleLabel->setText(tr("settings.subtitle"));

  if (accountHeader) {
    accountHeader->setTitle(tr("settings.account_header"));
    accountHeader->setSubtitle(tr("settings.account_desc"));
  }

  if (langHeader) {
    langHeader->setTitle(tr("settings.language_header"));
    langHeader->setSubtitle(tr("settings.language_desc"));
  }

  if (updateHeader) {
    updateHeader->setTitle(tr("settings.update_header"));
    updateHeader->setSubtitle(tr("settings.update_desc"));
  }
  if (versionLabel) {
    std::string verKey = switube::update::hasPending() ? "settings.version_pending" : "settings.version_label";
    versionLabel->setText(tr(verKey, APP_VERSION));
  }
  if (checkUpdatesBtn && !checkUpdatesBtn->getChildren().empty()) {
    if (auto* lbl = dynamic_cast<brls::Label*>(checkUpdatesBtn->getChildren()[0])) {
      lbl->setText(checkingUpdates ? tr("settings.checking_updates") : tr("settings.check_updates_btn"));
    }
  }
  if (viewChangelogBtn && !viewChangelogBtn->getChildren().empty()) {
    if (auto* lbl = dynamic_cast<brls::Label*>(viewChangelogBtn->getChildren()[0])) {
      lbl->setText(tr("settings.view_changelog_btn"));
    }
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

  // Conservar el foco activamente en el chip de idioma actual solo si Ajustes tiene el foco
  if (this->isFocused() || this->isChildFocused()) {
    auto* activeChip = (currentLang == switube::Language::Spanish) ? chipEs : chipEn;
    if (activeChip) {
      brls::Application::giveFocus(activeChip);
    }
  }
}
