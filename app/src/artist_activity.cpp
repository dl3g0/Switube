#include "artist_activity.hpp"

#include "playback_shortcuts.hpp"
#include "track_list_activity.hpp"
#include "ui_helpers.hpp"
#include "thread_helper.hpp"
#include "NXConsoleLog.h"
#include "i18n.hpp"

using switube::tr;

ArtistActivity::ArtistActivity(std::string artistBrowseId,
                               std::string fallbackName,
                               std::string fallbackImageUrl)
    : artistBrowseId(std::move(artistBrowseId)),
      fallbackName(std::move(fallbackName)),
      fallbackImageUrl(std::move(fallbackImageUrl)),
      alive(std::make_shared<std::atomic_bool>(true)) {}

ArtistActivity::~ArtistActivity() {
  alive->store(false);
}

brls::View* ArtistActivity::createContentView() {
  auto* root = new brls::Box();
  root->setAxis(brls::Axis::COLUMN);
  root->setGrow(1.0f);
  root->setBackgroundColor(swui::background());

  // Cabecera Hero con Foto Circular del Artista
  auto* hero = new brls::Box();
  hero->setAxis(brls::Axis::ROW);
  hero->setAlignItems(brls::AlignItems::CENTER);
  hero->setPadding(24, 48, 16, 48);

  artwork = new brls::Image();
  artwork->setDimensions(140, 140);
  artwork->setCornerRadius(70); // Foto de artista circular
  artwork->setBackgroundColor(swui::surfaceRaised());
  artwork->setScalingType(brls::ImageScalingType::FIT);
  if (!fallbackImageUrl.empty()) {
    swui::loadRemoteImage(artwork, fallbackImageUrl, 500);
  }
  hero->addView(artwork);

  auto* infoCol = new brls::Box();
  infoCol->setAxis(brls::Axis::COLUMN);
  infoCol->setGrow(1.0f);
  infoCol->setMarginLeft(28);

  nameLabel = swui::makeLabel(fallbackName.empty() ? tr("artist.artist") : fallbackName, 32, swui::textPrimary(), true);
  infoCol->addView(nameLabel);

  statsLabel = swui::makeLabel(tr("artist.loading_profile"), 16, swui::textSecondary(), true);
  statsLabel->setMarginTop(4);
  infoCol->addView(statsLabel);

  playBtn = swui::makeActionButton(
      tr("artist.play_music"),
      [this](brls::View*) {
        if (!artistPage.topSongs.empty()) {
          switube::YtEngine::getInstance().playQueue(artistPage.topSongs, 0);
        }
        return true;
      },
      false);
  playBtn->setWidth(210);
  playBtn->setMarginTop(14);
  infoCol->addView(playBtn);

  hero->addView(infoCol);
  root->addView(hero);

  // Contenedor con Scroll vertical para Canciones y Carruseles
  auto* scroll = new brls::ScrollingFrame();
  scroll->setGrow(1.0f);
  scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);

  content = new brls::Box();
  content->setAxis(brls::Axis::COLUMN);
  content->setPadding(4, 48, 36, 48);

  statusPanel = swui::makeStatePanel(
      tr("artist.artist"),
      tr("artist.loading_discography"));
  content->addView(statusPanel);

  scroll->setContentView(content);
  root->addView(scroll);

  frame = new brls::AppletFrame(root);
  frame->setHeaderVisibility(brls::Visibility::GONE);
  frame->setFooterVisibility(brls::Visibility::VISIBLE);
  return frame;
}

void ArtistActivity::onContentAvailable() {
  registerPlaybackShortcuts(this);

  // Cargar datos en segundo plano
  auto token = alive;
  auto browseId = artistBrowseId;
  switube::launchWorkerWithStack([this, token, browseId]() {
    switube::YtArtistPage page;
    bool ok = switube::YtEngine::getInstance().getClient().getArtistPage(browseId, page);

    brls::sync([this, token, ok, page = std::move(page)]() mutable {
      if (!token->load()) return;
      if (ok && (!page.name.empty() || !page.topSongs.empty() || !page.sections.empty())) {
        renderArtist(page);
      } else {
        if (content) {
          swui::safeClearViews(content, nullptr);
          content->addView(swui::makeStatePanel(
              tr("artist.error_title"),
              tr("artist.error_desc")));
        }
      }
    });
  });
}

void ArtistActivity::renderArtist(const switube::YtArtistPage& page) {
  artistPage = page;
  if (!content) return;
  swui::safeClearViews(content, nullptr);

  if (!page.name.empty() && nameLabel) {
    nameLabel->setText(page.name);
  }
  if (!page.subscriberCount.empty() && statsLabel) {
    statsLabel->setText(page.subscriberCount + " · YouTube Music");
  } else if (statsLabel) {
    statsLabel->setText(tr("artist.youtube_music"));
  }

  if (!page.thumbnailUrl.empty() && artwork) {
    swui::loadRemoteImage(artwork, page.thumbnailUrl, 500);
  }

  if (!artistPage.topSongs.empty() && playBtn) {
    playBtn->setBackgroundColor(swui::accent());
    playBtn->setFocusable(true);
  }

  // 1. Canciones principales (Top Songs)
  if (!artistPage.topSongs.empty()) {
    auto* topHeaderBox = new brls::Box();
    topHeaderBox->setAxis(brls::Axis::ROW);
    topHeaderBox->setAlignItems(brls::AlignItems::CENTER);
    topHeaderBox->setMarginTop(8);
    topHeaderBox->setMarginBottom(10);

    auto* topHeader = new brls::Header();
    topHeader->setTitle(tr("artist.top_songs"));
    topHeader->setGrow(1.0f);
    topHeader->setHeight(36);
    topHeaderBox->addView(topHeader);

    // Botón "Ver todas" si YouTube Music provee el browseId de la lista completa
    if (!artistPage.allSongsBrowseId.empty()) {
      auto* seeAllBtn = swui::makeActionButton(
          tr("artist.see_all"),
          [this](brls::View*) {
            switube::YtTrack allSongsCollection;
            allSongsCollection.id = artistPage.allSongsBrowseId;
            allSongsCollection.title = artistPage.name + " - " + tr("filter.songs");
            allSongsCollection.artist = artistPage.name;
            allSongsCollection.imageUrl = artistPage.thumbnailUrl;
            allSongsCollection.isPlaylist = true;
            brls::Application::pushActivity(new TrackListActivity(allSongsCollection));
            return true;
          },
          false);
      seeAllBtn->setWidth(130);
      topHeaderBox->addView(seeAllBtn);
    }
    content->addView(topHeaderBox);

    for (size_t i = 0; i < artistPage.topSongs.size(); ++i) {
      const auto& track = artistPage.topSongs[i];
      auto* row = new swui::TrackRow();
      row->setTrack(track, i, [this, i]() {
        switube::YtEngine::getInstance().playQueue(artistPage.topSongs, i);
      });
      content->addView(row);
    }
  }

  // 2. Secciones / Carruseles (Álbumes, Singles & EPs, Vídeos, Directo, Destacado en, Listas, Similares)
  for (const auto& shelf : artistPage.sections) {
    if (shelf.tracks.empty()) continue;

    auto* section = new swui::HorizontalSection(shelf.title, "");
    section->setMarginTop(14);

    for (size_t i = 0; i < shelf.tracks.size(); ++i) {
      const auto& item = shelf.tracks[i];
      auto* card = new swui::MediaCard();
      card->setContent(item.title, item.artist, item.imageUrl);

      card->setAction([item] {
        if (item.isArtist) {
          brls::Application::pushActivity(new ArtistActivity(item.id, item.title, item.imageUrl));
        } else if (item.isPlaylist) {
          brls::Application::pushActivity(new TrackListActivity(item));
        } else {
          switube::YtEngine::getInstance().playTrack(item);
        }
      });

      section->addCard(card);
    }

    content->addView(section);
  }
}
