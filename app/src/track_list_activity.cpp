#include "track_list_activity.hpp"

#include "playback_shortcuts.hpp"
#include "ui_helpers.hpp"
#include "thread_helper.hpp"
#include "NXConsoleLog.h"
#include "i18n.hpp"

using switube::tr;

TrackListActivity::TrackListActivity(switube::YtTrack collection)
    : collection(std::move(collection)),
      alive(std::make_shared<std::atomic_bool>(true)) {}

TrackListActivity::~TrackListActivity() {
  alive->store(false);
}

void TrackListActivity::onContentAvailable() {
  registerPlaybackShortcuts(this);

  title->setText(collection.title.empty() ? tr("tracklist.collection") : collection.title);
  subtitle->setText(collection.artist.empty() ? tr("tracklist.loading") : collection.artist);
  description->setVisibility(brls::Visibility::GONE);

  if (!collection.imageUrl.empty()) {
    swui::loadRemoteImage(artwork, collection.imageUrl, 640);
  }

  playAll->setFocusable(false);
  playAll->setBackgroundColor(swui::surfaceRaised());
  playAllLabel->setText(tr("tracklist.loading"));
  playAllLabel->setTextColor(swui::textSecondary());

  playAll->registerClickAction([this](brls::View*) {
    if (!currentTracks.empty()) {
      switube::YtEngine::getInstance().playQueue(currentTracks, 0);
    }
    return true;
  });
  playAll->addGestureRecognizer(new brls::TapGestureRecognizer(playAll));
  playAll->getFocusEvent()->subscribe([this](brls::View*) {
    playAllLabel->setTextColor(swui::textPrimary());
  });
  playAll->getFocusLostEvent()->subscribe([this](brls::View*) {
    playAllLabel->setTextColor(playAll->isFocusable() ? swui::textPrimary() : swui::textSecondary());
  });

  list->addView(swui::makeStatePanel(
      tr("tracklist.loading_panel_title"),
      tr("tracklist.loading_panel_desc")));

  auto token = alive;
  switube::launchWorkerWithStack([this, token]() {
    std::vector<switube::YtTrack> tracks;
    bool ok = switube::YtEngine::getInstance().getClient().getPlaylistTracks(collection.id, tracks);
    nxlog::pushf("TrackListActivity: fetched %zu tracks for collection '%s'", tracks.size(), collection.title.c_str());

    brls::sync([this, token, ok, tracks = std::move(tracks)]() mutable {
      if (!token->load()) return;
      showTracks(tracks);
    });
  });
}

void TrackListActivity::showTracks(const std::vector<switube::YtTrack>& tracks) {
  list->clearViews();

  if (tracks.empty()) {
    subtitle->setText(tr("tracklist.empty_title"));
    playAllLabel->setText(tr("tracklist.empty_title"));
    list->addView(swui::makeStatePanel(
        tr("tracklist.empty_title"),
        tr("tracklist.empty_desc")));
    return;
  }

  currentTracks = tracks;
  std::string info = tr("tracklist.songs_count", std::to_string(currentTracks.size()));
  if (!collection.artist.empty()) {
    info += "  |  " + collection.artist;
  }
  subtitle->setText(info);

  playAllLabel->setText(tr("tracklist.play_all"));
  playAll->setFocusable(true);
  playAll->setBackgroundColor(swui::accent());
  playAllLabel->setTextColor(swui::textPrimary());

  for (size_t i = 0; i < currentTracks.size(); ++i) {
    auto* row = new swui::TrackRow();
    row->setTrack(currentTracks[i], i, [this, i]() {
      switube::YtEngine::getInstance().playQueue(currentTracks, i);
    });
    list->addView(row);
  }
}
