#include "mini_player.hpp"

#include "ui_helpers.hpp"
#include "download_manager.hpp"
#include "i18n.hpp"

MiniPlayer::MiniPlayer()
    : alive(std::make_shared<std::atomic_bool>(true)) {
  setAxis(brls::Axis::ROW);
  setAlignItems(brls::AlignItems::CENTER);
  setHeight(76);
  setMargins(4, 24, 10, 24);
  setPadding(8, 22, 10, 18);
  setCornerRadius(16);
  setBackgroundColor(nvgRGBA(0, 0, 0, 0));

  artwork = new brls::Image();
  artwork->setDimensions(62, 62);
  artwork->setCornerRadius(6);
  artwork->setBackgroundColor(swui::surfaceRaised());
  artwork->setScalingType(brls::ImageScalingType::FIT);
  addView(artwork);

  auto* text = new brls::Box();
  text->setAxis(brls::Axis::COLUMN);
  text->setGrow(1.0f);
  text->setMarginLeft(14);
  title = swui::makeLabel("Switube", 18, swui::textPrimary(), true);
  subtitle = swui::makeLabel(switube::tr("mini_player.default_subtitle"), 14,
                             swui::textSecondary(), true);
  subtitle->setMarginTop(3);
  text->addView(title);
  text->addView(subtitle);
  addView(text);

  state = swui::makeLabel(switube::tr("state.ready"), 13, swui::accent(), true);
  state->setMarginRight(12);
  addView(state);

  downloadBtn = new brls::Box();
  downloadBtn->setFocusable(true);
  downloadBtn->setAxis(brls::Axis::ROW);
  downloadBtn->setAlignItems(brls::AlignItems::CENTER);
  downloadBtn->setJustifyContent(brls::JustifyContent::CENTER);
  downloadBtn->setWidth(46);
  downloadBtn->setHeight(46);
  downloadBtn->setCornerRadius(23);
  downloadBtn->setHighlightCornerRadius(23);
  downloadBtn->setBackgroundColor(swui::surfaceRaised());
  downloadBtn->setMarginRight(14);
  downloadBtn->addGestureRecognizer(new brls::TapGestureRecognizer(downloadBtn));

  downloadBadge = new swui::DownloadBadge(26.0f);
  downloadBtn->addView(downloadBadge);

  downloadBtn->registerClickAction([this](brls::View*) {
    handleDownloadCurrentTrack();
    return true;
  });
  downloadBtn->registerAction("Descargar", brls::BUTTON_A, [this](brls::View*) {
    handleDownloadCurrentTrack();
    return true;
  });
  addView(downloadBtn);

    auto* previous = swui::makePrevButton([](brls::View*) {
    switube::YtEngine::getInstance().previous();
    return true;
  });
  previous->setWidth(54);
  previous->setHeight(46);
  previous->setCornerRadius(23);
  previous->setMarginRight(10);

  auto* play = swui::makePlayPauseButton(
      [](brls::View*) {
        switube::YtEngine::getInstance().playPause();
        return true;
      },
      &playPauseIcon, true);
  play->setWidth(68);
  play->setHeight(46);
  play->setCornerRadius(23);
  play->setMarginRight(10);

  auto* next = swui::makeNextButton([](brls::View*) {
    switube::YtEngine::getInstance().next();
    return true;
  });
  next->setWidth(54);
  next->setHeight(46);
  next->setCornerRadius(23);

  addView(previous);
  addView(play);
  addView(next);

  auto token = alive;
  observerId = switube::YtEngine::getInstance().addObserver([this, token] {
    brls::sync([this, token] {
      if (token->load()) refresh();
    });
  });
  i18nObserverId = switube::I18n::getInstance().addObserver([this, token] {
    brls::sync([this, token] {
      if (token->load()) refresh();
    });
  });
  downloadObsId = switube::DownloadManager::getInstance().addObserver([this, token] {
    if (token->load()) refresh();
  });
  refresh();
}

MiniPlayer::~MiniPlayer() {
  alive->store(false);
  switube::YtEngine::getInstance().removeObserver(observerId);
  switube::I18n::getInstance().removeObserver(i18nObserverId);
  if (downloadObsId != 0) {
    switube::DownloadManager::getInstance().removeObserver(downloadObsId);
    downloadObsId = 0;
  }
}

void MiniPlayer::draw(NVGcontext* vg, float x, float y, float width, float height,
                      brls::Style style, brls::FrameContext* ctx) {
  const float r = 16.0f;

  nvgSave(vg);
  // Sombra suave flotante
  NVGpaint shadow = nvgBoxGradient(vg, x, y + 3.0f, width, height, r, 14.0f,
                                   nvgRGBA(0, 0, 0, 130), nvgRGBA(0, 0, 0, 0));
  nvgBeginPath(vg);
  nvgRect(vg, x - 12.0f, y - 8.0f, width + 24.0f, height + 24.0f);
  nvgRoundedRect(vg, x, y, width, height, r);
  nvgPathWinding(vg, NVG_HOLE);
  nvgFillPaint(vg, shadow);
  nvgFill(vg);

  // Fondo obsidiana translúcido + borde cristalino 1px
  nvgBeginPath(vg);
  nvgRoundedRect(vg, x, y, width, height, r);
  nvgFillColor(vg, nvgRGBA(18, 19, 27, 240));
  nvgFill(vg);

  nvgStrokeWidth(vg, 1.0f);
  nvgStrokeColor(vg, nvgRGBA(255, 255, 255, 24));
  nvgStroke(vg);

  // Barra de progreso en vivo en el borde inferior
  const auto now = switube::YtEngine::getInstance().getNowPlaying();
  if (!now.id.empty() && now.durationMs > 0) {
    float progress = std::clamp(
        static_cast<float>(now.positionMs) / static_cast<float>(now.durationMs),
        0.0f, 1.0f);
    float barPad = 14.0f;
    float barW = std::max(0.0f, width - barPad * 2.0f);
    float barY = y + height - 3.5f;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x + barPad, barY, barW, 2.5f, 1.25f);
    nvgFillColor(vg, nvgRGBA(255, 255, 255, 22));
    nvgFill(vg);

    if (progress > 0.005f) {
      nvgBeginPath(vg);
      nvgRoundedRect(vg, x + barPad, barY, barW * progress, 2.5f, 1.25f);
      nvgFillColor(vg, swui::accent());
      nvgFill(vg);
    }
  }
  nvgRestore(vg);

  brls::Box::draw(vg, x, y, width, height, style, ctx);
}

void MiniPlayer::handleDownloadCurrentTrack() {
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

void MiniPlayer::refresh() {
  auto& engine = switube::YtEngine::getInstance();
  const auto snapshot = engine.getNowPlaying();
  auto playState = engine.getState();

  if (!snapshot.id.empty()) {
    title->setText(snapshot.title);
    std::string detail = snapshot.artist;
    if (!snapshot.album.empty()) detail += "  |  " + snapshot.album;
    subtitle->setText(detail);

    if (playPauseIcon) {
      playPauseIcon->setPlaying(playState == switube::PlaybackState::Playing);
    }
    if (playState == switube::PlaybackState::Loading) {
      state->setText(switube::tr("state.loading"));
      
    } else if (playState == switube::PlaybackState::Playing) {
      state->setText(switube::tr("state.playing"));
      
    } else if (playState == switube::PlaybackState::Paused) {
      state->setText(switube::tr("state.paused"));
      
    } else {
      state->setText(switube::tr("state.stopped"));
      
    }

    if (!snapshot.imageUrl.empty() && snapshot.imageUrl != loadedArtworkUrl) {
      loadedArtworkUrl = snapshot.imageUrl;
      swui::loadRemoteImage(artwork, snapshot.imageUrl, 240);
    }

    if (downloadBtn && downloadBadge) {
      downloadBtn->setVisibility(brls::Visibility::VISIBLE);
      auto& dm = switube::DownloadManager::getInstance();
      bool downloaded = dm.isDownloaded(snapshot.id);
      bool downloading = dm.isDownloading(snapshot.id);

      if (downloaded) {
        downloadBadge->setState(swui::DownloadVisualState::Downloaded);
      } else if (downloading) {
        downloadBadge->setState(swui::DownloadVisualState::Downloading);
      } else {
        downloadBadge->setState(swui::DownloadVisualState::NotDownloaded);
      }

      std::string hint = downloaded ? "Eliminar descarga" : (downloading ? "Descargando..." : "Descargar");
      downloadBtn->updateActionHint(brls::BUTTON_A, hint);
    }
  } else {
    if (downloadBtn) downloadBtn->setVisibility(brls::Visibility::GONE);
    title->setText("Switube");
    subtitle->setText(switube::tr("mini_player.default_subtitle"));
    state->setText(switube::tr("state.ready"));
    
  }
}
