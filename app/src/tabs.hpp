#pragma once

#include <borealis.hpp>

#include <atomic>
#include <memory>
#include <vector>
#include <string>

#include "yt_engine.hpp"
#include "ui_helpers.hpp"
#include "i18n.hpp"

class HomeTab : public brls::Box {
 public:
  HomeTab();
  ~HomeTab() override;
  brls::View* getDefaultFocus() override;
  static brls::View* create() { return new HomeTab(); }

 private:
  void load();
  void loadMore();
  void renderHome(const std::vector<switube::YtShelf>& shelves, bool append = false);
  void updateLabels();

  std::string continuationToken;
  bool isLoadingMore = false;
  std::vector<switube::YtShelf> currentShelves;
  swui::ScrollSentinel* scrollSentinel = nullptr;

  brls::Label* titleLabel = nullptr;
  brls::Label* subtitleLabel = nullptr;
  brls::Label* statusLabel = nullptr;
  brls::Box* refreshBtn = nullptr;
  brls::Label* refreshBtnLabel = nullptr;
  brls::Box* content = nullptr;
  size_t i18nObserverId = 0;
  size_t engineObserverId = 0;
  bool lastSignedInState = false;
  std::shared_ptr<std::atomic_bool> alive;
};

class SearchTab : public brls::Box {
 public:
  SearchTab();
  ~SearchTab() override;
  brls::View* getDefaultFocus() override;
  static brls::View* create() { return new SearchTab(); }

 private:
  void openKeyboard();
  void search(const std::string& query);
  void loadMore();
  void setFilter(switube::YtSearchFilter filter);
  void updateFilterChips();
  void updateLabels();
  void renderResults(const std::vector<switube::YtTrack>& tracks, bool append = false);

  std::string currentQuery;
  switube::YtSearchFilter currentFilter = switube::YtSearchFilter::All;
  std::string continuationToken;
  bool isLoadingMore = false;
  std::vector<switube::YtTrack> currentResults;
  swui::ScrollSentinel* scrollSentinel = nullptr;

  brls::Label* titleLabel = nullptr;
  brls::Label* subtitleLabel = nullptr;
  brls::Label* queryLabel = nullptr;
  brls::Label* statusLabel = nullptr;
  brls::Box* chipsRow = nullptr;
  swui::FilterChip* chipAll = nullptr;
  swui::FilterChip* chipSongs = nullptr;
  swui::FilterChip* chipVideos = nullptr;
  swui::FilterChip* chipAlbums = nullptr;
  swui::FilterChip* chipArtists = nullptr;
  swui::FilterChip* chipPlaylists = nullptr;

  brls::Box* content = nullptr;
  brls::Box* searchBtn = nullptr;
  brls::Box* loadMoreBtn = nullptr;
  size_t i18nObserverId = 0;
  std::shared_ptr<std::atomic_bool> alive;
};

class LibraryTab : public brls::Box {
 public:
  LibraryTab();
  ~LibraryTab() override;
  brls::View* getDefaultFocus() override;
  static brls::View* create() { return new LibraryTab(); }

 private:
  void refresh();
  void updateLabels();

  bool showPlaylists = false;
  brls::Label* titleLabel = nullptr;
  brls::Label* subtitleLabel = nullptr;
  brls::Box* syncBtn = nullptr;
  brls::Label* syncBtnLabel = nullptr;
  swui::FilterChip* chipFavorites = nullptr;
  swui::FilterChip* chipPlaylists = nullptr;
  brls::Box* content = nullptr;
  swui::ScrollSentinel* scrollSentinel = nullptr;
  std::vector<std::string> renderedFavoriteIds;
  std::vector<std::string> renderedPlaylistIds;
  bool lastRenderedMode = false;
  size_t observerId = 0;
  size_t i18nObserverId = 0;
  size_t downloadObsId = 0;
  std::shared_ptr<std::atomic_bool> alive;
};

class DownloadsTab : public brls::Box {
 public:
  DownloadsTab();
  ~DownloadsTab() override;
  brls::View* getDefaultFocus() override;
  static brls::View* create() { return new DownloadsTab(); }

 private:
  void refresh();
  void updateLabels();

  brls::Label* titleLabel = nullptr;
  brls::Label* subtitleLabel = nullptr;
  brls::Box* content = nullptr;
  size_t observerId = 0;
  size_t i18nObserverId = 0;
  size_t downloadObsId = 0;
  std::shared_ptr<std::atomic_bool> alive;
};

class MusicProgressBar;

class NowPlayingTab : public brls::Box {
 public:
  NowPlayingTab();
  ~NowPlayingTab() override;
  brls::View* getDefaultFocus() override;
  static brls::View* create() { return new NowPlayingTab(); }

  void draw(NVGcontext* vg, float x, float y, float width, float height,
            brls::Style style, brls::FrameContext* ctx) override;

 private:
  void refresh();
  void updateLabels();

  brls::Image* artwork = nullptr;
  brls::Label* contextLabel = nullptr;
  brls::Label* titleLabel = nullptr;
  brls::Label* artistLabel = nullptr;
  brls::Label* elapsedLabel = nullptr;
  brls::Label* remainingLabel = nullptr;
  MusicProgressBar* progressBar = nullptr;

  brls::Box* playBtn = nullptr;
  swui::PlayPauseIcon* playPauseIcon = nullptr;
  brls::Box* prevBtn = nullptr;
  brls::Box* nextBtn = nullptr;
  brls::Box* favBtn = nullptr;
  brls::Label* favBtnLabel = nullptr;
  brls::Box* downloadBtn = nullptr;
  swui::DownloadBadge* downloadBadge = nullptr;
  brls::Label* deviceLabel = nullptr;

  std::string loadedArtworkUrl;
  size_t observerId = 0;
  size_t i18nObserverId = 0;
  size_t downloadObsId = 0;
  void handleDownloadCurrentTrack();
  std::shared_ptr<std::atomic_bool> alive;
};

class SettingsTab : public brls::Box {
 public:
  SettingsTab();
  ~SettingsTab() override;
  static brls::View* create() { return new SettingsTab(); }

  brls::View* getDefaultFocus() override;

 private:
  void updateLabels();
  void refreshAccountSection();
  void startQrLinking();
  void cancelQrLinking();

  brls::Label* titleLabel = nullptr;
  brls::Label* subtitleLabel = nullptr;
  brls::Box* content = nullptr;
  brls::Header* accountHeader = nullptr;
  brls::Box* accountContainer = nullptr;
  brls::View* primaryAccountFocus = nullptr;
  brls::Header* langHeader = nullptr;
  swui::FilterChip* chipEs = nullptr;
  swui::FilterChip* chipEn = nullptr;
  brls::Label* infoTitleLabel = nullptr;
  brls::Label* infoDescLabel = nullptr;

  bool waitingForQr = false;
  bool lastAccountSignedIn = false;
  bool lastAccountSyncing = false;
  std::string lastAccountName;
  switube::YtDeviceCode activeDeviceCode;
  std::shared_ptr<std::atomic_bool> qrPollingActive;

  size_t i18nObserverId = 0;
  size_t engineObserverId = 0;
  std::shared_ptr<std::atomic_bool> alive;
};
