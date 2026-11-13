#pragma once

#include <borealis.hpp>

#include <atomic>
#include <memory>
#include <string>

#include "yt_engine.hpp"

namespace swui {
class PlayPauseIcon;
class DownloadBadge;
}

class MiniPlayer : public brls::Box {
 public:
  MiniPlayer();
  ~MiniPlayer() override;

  static brls::View* create() { return new MiniPlayer(); }

 private:
  void refresh();
  void handleDownloadCurrentTrack();

  brls::Image* artwork = nullptr;
  brls::Label* title = nullptr;
  brls::Label* subtitle = nullptr;
  brls::Label* state = nullptr;
  swui::PlayPauseIcon* playPauseIcon = nullptr;
  brls::Box* downloadBtn = nullptr;
  swui::DownloadBadge* downloadBadge = nullptr;
  std::string loadedArtworkUrl;
  size_t observerId = 0;
  size_t i18nObserverId = 0;
  size_t downloadObsId = 0;
  std::shared_ptr<std::atomic_bool> alive;
};
