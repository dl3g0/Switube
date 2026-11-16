#pragma once

#include <borealis.hpp>

#include <functional>
#include <string>

#include "yt_music_client.hpp"

namespace swui {

constexpr float kPagePadding = 42.0f;

enum class RepeatMode { Off, All, One };

NVGcolor background();
NVGcolor surface();
NVGcolor surfaceRaised();
NVGcolor textPrimary();
NVGcolor textSecondary();
NVGcolor accent();
NVGcolor danger();

std::string formatDuration(uint32_t milliseconds);
std::string upgradeThumbnailUrl(const std::string& rawUrl, int targetSize = 544);
void loadRemoteImage(brls::Image* image, const std::string& url,
                     int preferredSize = 0);
void shutdownRemoteImages();

brls::Label* makeLabel(const std::string& text, float size, NVGcolor color,
                       bool singleLine = false);
brls::Box* makeActionButton(const std::string& text,
                            std::function<bool(brls::View*)> action,
                            bool primary = false,
                            brls::Label** labelOut = nullptr);

class GradientBox : public brls::Box {
 public:
  GradientBox();
  void draw(NVGcontext* vg, float x, float y, float width, float height,
            brls::Style style, brls::FrameContext* ctx) override;
  static brls::View* create() { return new GradientBox(); }
};

class FilterChip : public brls::Box {
 public:
  FilterChip(const std::string& text, std::function<void()> action);
  void setSelected(bool selected);
  bool isSelected() const { return selected; }
  void setText(const std::string& text);

 private:
  void applyStyle(bool focused);

  brls::Label* label = nullptr;
  bool selected = false;
};

class RepeatIcon : public brls::View {
 public:
  RepeatIcon();
  void setMode(RepeatMode mode);
  void draw(NVGcontext* vg, float x, float y, float width, float height,
            brls::Style style, brls::FrameContext* ctx) override;

 private:
  RepeatMode mode = RepeatMode::Off;
};

brls::Box* makeRepeatButton(std::function<bool(brls::View*)> action,
                            RepeatIcon** iconOut = nullptr);

class ShuffleIcon : public brls::View {
 public:
  ShuffleIcon();
  void setActive(bool active);
  void draw(NVGcontext* vg, float x, float y, float width, float height,
            brls::Style style, brls::FrameContext* ctx) override;

 private:
  bool active = false;
};

brls::Box* makeShuffleButton(std::function<bool(brls::View*)> action,
                             ShuffleIcon** iconOut = nullptr);

class PlayPauseIcon : public brls::View {
 public:
  PlayPauseIcon(float size = 24.0f);
  void setPlaying(bool playing);
  bool getPlaying() const { return isPlaying; }
  void draw(NVGcontext* vg, float x, float y, float width, float height,
            brls::Style style, brls::FrameContext* ctx) override;

 private:
  float iconSize = 24.0f;
  bool isPlaying = false;
};

class PrevTrackIcon : public brls::View {
 public:
  PrevTrackIcon(float size = 20.0f);
  void draw(NVGcontext* vg, float x, float y, float width, float height,
            brls::Style style, brls::FrameContext* ctx) override;

 private:
  float iconSize = 20.0f;
};

class NextTrackIcon : public brls::View {
 public:
  NextTrackIcon(float size = 20.0f);
  void draw(NVGcontext* vg, float x, float y, float width, float height,
            brls::Style style, brls::FrameContext* ctx) override;

 private:
  float iconSize = 20.0f;
};

brls::Box* makePlayPauseButton(std::function<bool(brls::View*)> action,
                               PlayPauseIcon** iconOut = nullptr,
                               bool primary = true);
brls::Box* makePrevButton(std::function<bool(brls::View*)> action,
                          PrevTrackIcon** iconOut = nullptr);
brls::Box* makeNextButton(std::function<bool(brls::View*)> action,
                          NextTrackIcon** iconOut = nullptr);

class MediaCard : public brls::Box {
 public:
  MediaCard();

  void setContent(const std::string& title, const std::string& subtitle,
                  const std::string& imageUrl);
  void setAction(std::function<void()> action);

 private:
  brls::Image* artwork = nullptr;
  brls::Label* titleLabel = nullptr;
  brls::Label* subtitleLabel = nullptr;
  std::function<void()> onSelected;
};

enum class DownloadVisualState {
  NotDownloaded,
  Downloading,
  Downloaded
};

class DownloadBadge : public brls::View {
 public:
  explicit DownloadBadge(float size = 32.0f);
  void setState(DownloadVisualState state);
  void draw(NVGcontext* vg, float x, float y, float width, float height,
            brls::Style style, brls::FrameContext* ctx) override;

 private:
  DownloadVisualState state_ = DownloadVisualState::NotDownloaded;
  float size_ = 32.0f;
};

class TrackRow : public brls::Box {
 public:
  TrackRow();
  ~TrackRow() override;

  void setTrack(const switube::YtTrack& track, size_t index,
                std::function<void()> action);
  void setCompact(bool compact);
  void setCurrent(bool current);
  void updateDownloadStatus();
  void handleDownloadToggle();
  bool isDownloadable() const {
    return !track_.isPlaylist && !track_.isArtist && track_.id.size() == 11;
  }

 private:
  brls::Image* artwork = nullptr;
  brls::Label* indexLabel = nullptr;
  brls::Label* titleLabel = nullptr;
  brls::Label* subtitleLabel = nullptr;
  brls::Label* durationLabel = nullptr;
  DownloadBadge* downloadBadge = nullptr;
  brls::ActionIdentifier downloadActionId_ = ACTION_NONE;
  bool artworkEnabled = true;
  switube::YtTrack track_;
  size_t downloadObsId_ = 0;
  std::shared_ptr<std::atomic_bool> alive_;
  std::function<void()> onSelected;
};

class HorizontalSection : public brls::Box {
 public:
  explicit HorizontalSection(const std::string& title,
                             const std::string& subtitle = "");

  void addCard(MediaCard* card);
  void showEmpty(const std::string& text);

 private:
  brls::HScrollingFrame* scroller = nullptr;
  brls::Box* row = nullptr;
};

class ScrollSentinel : public brls::View {
 public:
  explicit ScrollSentinel(std::function<void()> onVisible)
      : onVisible_(std::move(onVisible)) {
    setHeight(16.0f);
    setFocusable(false);
  }
  void draw(NVGcontext*, float, float, float, float, brls::Style,
            brls::FrameContext*) override {
    if (!triggered_ && onVisible_) {
      triggered_ = true;
      onVisible_();
    }
  }
  void reset() { triggered_ = false; }

 private:
  std::function<void()> onVisible_;
  bool triggered_ = false;
};

brls::Box* makeStatePanel(const std::string& title,
                          const std::string& detail,
                          NVGcolor tint = surfaceRaised(),
                          brls::Label** titleOut = nullptr,
                          brls::Label** detailOut = nullptr);

// Limpia vistas de forma segura liberando el foco antes de destruirlas, evitando punteros colgantes en Borealis
void safeClearViews(brls::Box* container, brls::View* fallbackFocus = nullptr);
void safeRemoveView(brls::Box* container, brls::View* child, brls::View* fallbackFocus = nullptr);
brls::View* nearestFocusableChild(brls::Box* container, size_t preferredIdx = 0);

} // namespace swui
