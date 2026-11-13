#pragma once

#include <borealis.hpp>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "yt_music_client.hpp"
#include "yt_engine.hpp"

class ArtistActivity : public brls::Activity {
 public:
  explicit ArtistActivity(std::string artistBrowseId,
                          std::string fallbackName = "",
                          std::string fallbackImageUrl = "");
  ~ArtistActivity() override;

  brls::View* createContentView() override;
  void onContentAvailable() override;

 private:
  void renderArtist(const switube::YtArtistPage& page);

  std::string artistBrowseId;
  std::string fallbackName;
  std::string fallbackImageUrl;

  brls::AppletFrame* frame = nullptr;
  brls::Image* artwork = nullptr;
  brls::Label* nameLabel = nullptr;
  brls::Label* statsLabel = nullptr;
  brls::Box* playBtn = nullptr;
  brls::Box* content = nullptr;
  brls::Box* statusPanel = nullptr;

  switube::YtArtistPage artistPage;
  std::shared_ptr<std::atomic_bool> alive;
};
