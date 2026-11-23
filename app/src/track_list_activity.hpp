#pragma once

#include <borealis.hpp>
#include <atomic>
#include <memory>
#include <vector>

#include "yt_music_client.hpp"
#include "yt_engine.hpp"
#include "ui_helpers.hpp"

class TrackListActivity : public brls::Activity {
 public:
  explicit TrackListActivity(switube::YtTrack collection);
  ~TrackListActivity() override;

  CONTENT_FROM_XML_RES("activity/tracklist.xml");
  void onContentAvailable() override;

 private:
  void showTracks(const std::vector<switube::YtTrack>& tracks, bool append = false);
  void loadMore();

  switube::YtTrack collection;
  std::vector<switube::YtTrack> currentTracks;
  std::string continuationToken;
  bool isLoadingMore = false;
  swui::ScrollSentinel* scrollSentinel = nullptr;
  std::shared_ptr<std::atomic_bool> alive;

  BRLS_BIND(brls::Image, artwork, "artwork");
  BRLS_BIND(brls::Label, title, "title");
  BRLS_BIND(brls::Label, subtitle, "subtitle");
  BRLS_BIND(brls::Label, description, "description");
  BRLS_BIND(brls::Box, playAll, "playAll");
  BRLS_BIND(brls::Label, playAllLabel, "playAllLabel");
  BRLS_BIND(brls::Box, playShuffle, "playShuffle");
  BRLS_BIND(brls::Label, playShuffleLabel, "playShuffleLabel");
  BRLS_BIND(brls::Box, list, "list");
};
