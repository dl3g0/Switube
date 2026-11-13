#pragma once

#include <algorithm>
#include <cstdint>

#include <borealis.hpp>

#include "yt_engine.hpp"

// Atajos globales de control de reproducción con Joy-Con / Pro Controller
template <typename Host>
void registerPlaybackShortcuts(Host* host) {
  host->registerAction(
      "Play / pause", brls::BUTTON_Y,
      [](brls::View*) {
        switube::YtEngine::getInstance().playPause();
        return true;
      },
      true);
  host->registerAction(
      "Previous", brls::BUTTON_LB,
      [](brls::View*) {
        switube::YtEngine::getInstance().previous();
        return true;
      },
      true);
  host->registerAction(
      "Next", brls::BUTTON_RB,
      [](brls::View*) {
        switube::YtEngine::getInstance().next();
        return true;
      },
      true);
  host->registerAction(
      "Rewind", brls::BUTTON_LT,
      [](brls::View*) {
        const auto playback = switube::YtEngine::getInstance().getNowPlaying();
        switube::YtEngine::getInstance().seekTo(
            playback.positionMs > 15000 ? playback.positionMs - 15000 : 0);
        return true;
      },
      true);
  host->registerAction(
      "Fast forward", brls::BUTTON_RT,
      [](brls::View*) {
        const auto playback = switube::YtEngine::getInstance().getNowPlaying();
        const uint64_t target = static_cast<uint64_t>(playback.positionMs) + 15000;
        switube::YtEngine::getInstance().seekTo(static_cast<uint32_t>(
            playback.durationMs > 0
                ? std::min<uint64_t>(target, static_cast<uint64_t>(playback.durationMs))
                : target));
        return true;
      },
      true);
}
