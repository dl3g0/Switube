#include "i18n.hpp"

#include <fstream>
#include <unordered_map>
#ifdef __SWITCH__
#include <switch.h>
#endif
#ifndef SWITUBE_BACKGROUND_SERVICE
#include <borealis.hpp>
#endif
#include <nlohmann/json.hpp>
#include "NXConsoleLog.h"

using json = nlohmann::json;

namespace switube {

namespace {

const char* kConfigPaths[] = {
#ifdef __SWITCH__
    "/config/Switube/settings.json",
    "sdmc:/config/Switube/settings.json",
#endif
    "settings.json",
    "config.json"
};

const std::unordered_map<std::string, std::pair<const char*, const char*>> kTranslations = {
    // Tabs & Shell
    {"tab.home", {"Inicio", "Home"}},
    {"tab.search", {"Buscar", "Search"}},
    {"tab.library", {"Biblioteca", "Library"}},
    {"tab.now_playing", {"En reproducción", "Now Playing"}},
    {"tab.downloads", {"Descargas", "Downloads"}},
    {"tab.settings", {"Ajustes", "Settings"}},
    {"downloads.title", {"Descargas", "Downloads"}},
    {"downloads.subtitle", {"Música disponible sin conexión", "Music available offline"}},
    {"downloads.subtitle_empty", {"0 canciones descargadas", "0 downloaded songs"}},
    {"downloads.empty_title", {"No tienes canciones descargadas", "No downloaded songs"}},
    {"downloads.empty_desc", {"Toca el icono de descarga (⬇) o pulsa (X) en cualquier canción para escucharla sin conexión.", "Tap the download icon (⬇) or press (X) on any song to listen offline."}},
    {"downloads.play_all", {"Reproducir todo", "Play All"}},

    // Home Tab
    {"home.title", {"YouTube Music", "YouTube Music"}},
    {"home.subtitle", {"Música destacada y recomendaciones", "Featured music and recommendations"}},
    {"home.refresh", {"Actualizar", "Refresh"}},
    {"home.loading", {"Cargando recomendaciones de YouTube Music...", "Loading YouTube Music recommendations..."}},
    {"home.error", {"No se pudo cargar el inicio. Pulsa 'Actualizar' o verifica tu conexión.", "Could not load Home feed. Press 'Refresh' or check your connection."}},
    {"home.empty_title", {"Sin contenido disponible", "No content available"}},
    {"home.empty_desc", {"No se encontraron secciones en YouTube Music. Pulsa 'Actualizar' para reintentar.", "No sections found on YouTube Music. Press 'Refresh' to retry."}},

    // Search Tab
    {"search.title", {"Buscar", "Search"}},
    {"search.subtitle", {"Canciones, artistas y listas en YouTube Music", "Songs, artists, and playlists on YouTube Music"}},
    {"search.open_keyboard", {"Abrir teclado", "Open keyboard"}},
    {"search.keyboard_title", {"Buscar en YouTube Music", "Search on YouTube Music"}},
    {"search.hint", {"Escribe el nombre de una canción o artista para buscar", "Type the name of a song or artist to search"}},
    {"search.searching", {"Buscando: \"{}\"...", "Searching: \"{}\"..."}},
    {"search.results_count", {"Resultados para: \"{}\" ({} resultados)", "Results for: \"{}\" ({} results)"}},
    {"search.no_results", {"No se encontraron resultados para: \"{}\"", "No results found for: \"{}\""}},
    {"search.load_more", {"Cargar más resultados", "Load more results"}},
    {"filter.all", {"Todo", "All"}},
    {"filter.songs", {"Canciones", "Songs"}},
    {"filter.videos", {"Vídeos", "Videos"}},
    {"filter.albums", {"Álbumes", "Albums"}},
    {"filter.artists", {"Artistas", "Artists"}},
    {"filter.playlists", {"Listas", "Playlists"}},

    // Library Tab
    {"library.title", {"Tu Biblioteca", "Your Library"}},
    {"library.subtitle", {"Canciones guardadas como favoritas", "Songs saved as favorites"}},
    {"library.empty_title", {"No tienes favoritos aún", "No favorites yet"}},
    {"library.empty_desc", {"Añade canciones tocando el botón de corazón (♡) en 'En reproducción'.", "Add songs by tapping the heart button (♡) in 'Now Playing'."}},

    // Now Playing Tab
    {"now_playing.title", {"En reproducción", "Now Playing"}},
    {"now_playing.no_playback", {"No hay reproducción", "No playback active"}},
    {"now_playing.choose_track", {"Elige una pista en Inicio o Búsqueda", "Choose a track from Home or Search"}},
    {"now_playing.ready", {"Listo para reproducir YouTube Music", "Ready to play YouTube Music"}},
    {"now_playing.context_from", {"Reproduciendo desde YouTube Music", "Playing from YouTube Music"}},
    {"now_playing.lyrics", {"Letras", "Lyrics"}},
    {"now_playing.device", {"Switch Audio (48 kHz)", "Switch Audio (48 kHz)"}},
    {"now_playing.queue", {"Cola", "Queue"}},
    {"now_playing.seek_back", {"Retroceder 10s", "Rewind 10s"}},
    {"now_playing.seek_fwd", {"Avanzar 10s", "Forward 10s"}},
    {"now_playing.vol_down", {"Bajar volumen", "Volume down"}},
    {"now_playing.vol_up", {"Subir volumen", "Volume up"}},

    // Artist View
    {"artist.artist", {"Artista", "Artist"}},
    {"artist.loading_profile", {"Cargando perfil de artista...", "Loading artist profile..."}},
    {"artist.loading_discography", {"Obteniendo canciones principales, álbumes y vídeos de YouTube Music...", "Fetching top songs, albums, and videos from YouTube Music..."}},
    {"artist.play_music", {"Reproducir música", "Play music"}},
    {"artist.top_songs", {"Canciones principales", "Top songs"}},
    {"artist.see_all", {"Ver todas", "See all"}},
    {"artist.youtube_music", {"Artista en YouTube Music", "Artist on YouTube Music"}},
    {"artist.error_title", {"No se pudo cargar el perfil", "Could not load artist profile"}},
    {"artist.error_desc", {"Verifica tu conexión a internet o intenta de nuevo más tarde.", "Check your internet connection or try again later."}},

    // TrackList View
    {"tracklist.collection", {"Colección", "Collection"}},
    {"tracklist.loading", {"Cargando canciones...", "Loading tracks..."}},
    {"tracklist.loading_panel_title", {"Cargando pistas", "Loading tracks"}},
    {"tracklist.loading_panel_desc", {"Obteniendo canciones de YouTube Music...", "Fetching songs from YouTube Music..."}},
    {"tracklist.play_all", {"Reproducir todo", "Play all"}},
    {"tracklist.songs_count", {"{} canciones", "{} songs"}},
    {"tracklist.empty_title", {"Sin canciones", "No songs"}},
    {"tracklist.empty_desc", {"No se pudieron encontrar canciones reproducibles en este álbum o lista.", "No playable tracks found in this album or playlist."}},

    // Settings Tab
    {"settings.title", {"Ajustes", "Settings"}},
    {"settings.subtitle", {"Configuración y diagnóstico del reproductor", "Player configuration and diagnostics"}},
    {"settings.language_header", {"Idioma / Language", "Language / Idioma"}},
    {"settings.language_desc", {"Selecciona el idioma preferido para la interfaz y recomendaciones", "Select preferred language for interface and recommendations"}},
    {"settings.lang_es", {"Español", "Spanish"}},
    {"settings.lang_en", {"English", "English"}},
    {"settings.about_title", {"Switube v0.1.0", "Switube v0.1.0"}},
    {"settings.about_desc", {
        "Cliente nativo de YouTube Music para Nintendo Switch.\n"
        "Soporte nativo para carruseles, búsquedas, perfiles de artista, descargas offline y audio Opus 48 kHz.\n"
        "Créditos: Switify (interfaz) y BitChord (referencia).",
        "Native YouTube Music client for Nintendo Switch.\n"
        "Native support for carousels, search, artist profiles, offline downloads, and 48 kHz Opus audio.\n"
        "Credits: Switify (UI) and BitChord (reference)."
    }},
    {"settings.notify_lang", {"Idioma cambiado a Español", "Language changed to English"}},

    // MiniPlayer & Playback States
    {"mini_player.default_subtitle", {"YouTube Music para Switch", "YouTube Music for Switch"}},
    {"state.ready", {"LISTO", "READY"}},
    {"state.loading", {"CARGANDO", "LOADING"}},
    {"state.playing", {"REPRODUCIENDO", "PLAYING"}},
    {"state.paused", {"EN PAUSA", "PAUSED"}},
    {"state.stopped", {"DETENIDO", "STOPPED"}},
    {"state.error", {"ERROR", "ERROR"}},

    // Common
    {"common.nothing_here", {"Nada por aquí aún", "Nothing here yet"}},
    {"now_playing.artist_fallback", {"YouTube Music", "YouTube Music"}}
};

} // namespace

I18n& I18n::getInstance() {
  static I18n instance;
  return instance;
}

I18n::I18n() = default;

void I18n::init() {
  loadConfig();
}

void I18n::loadConfig() {
  for (const char* path : kConfigPaths) {
    std::ifstream file(path);
    if (file.is_open()) {
      try {
        json j;
        file >> j;
        if (j.contains("language")) {
          std::string l = j["language"];
          if (l == "en") currentLanguage_ = Language::English;
          else if (l == "es") currentLanguage_ = Language::Spanish;
          nxlog::pushf("I18n: loaded language '%s' from config", l.c_str());
#ifndef SWITUBE_BACKGROUND_SERVICE
          if (brls::Application::getPlatform() != nullptr) {
            brls::setLocale(currentLanguage_ == Language::Spanish ? brls::LOCALE_ES : brls::LOCALE_EN_US);
          }
#endif
          return;
        }
      } catch (...) {}
    }
  }

  // Si no hay configuración guardada, detectar del sistema de forma segura
  // (incluso si se llama antes de brls::Application::init(), donde getPlatform() es nullptr)
#ifndef SWITUBE_BACKGROUND_SERVICE
  std::string sysLocale = "es";
  if (brls::Application::getPlatform() != nullptr) {
    sysLocale = brls::Application::getLocale();
  }
#ifdef __SWITCH__
  else {
    uint64_t languageCode = 0;
    if (R_SUCCEEDED(setGetSystemLanguage(&languageCode)) && languageCode != 0) {
      sysLocale = std::string(reinterpret_cast<const char*>(&languageCode));
    }
  }
#endif
  if (sysLocale.rfind("es", 0) == 0) {
    currentLanguage_ = Language::Spanish;
  } else {
    currentLanguage_ = Language::English;
  }
  if (brls::Application::getPlatform() != nullptr) {
    brls::setLocale(currentLanguage_ == Language::Spanish ? brls::LOCALE_ES : brls::LOCALE_EN_US);
  }
#else
  std::string sysLocale = "es";
  currentLanguage_ = Language::Spanish;
#endif
  nxlog::pushf("I18n: system locale '%s', selected language: %s",
               sysLocale.c_str(), currentLanguage_ == Language::Spanish ? "Español" : "English");
}

void I18n::saveConfig() {
#ifdef __SWITCH__
  const char* savePath = "/config/Switube/settings.json";
#else
  const char* savePath = "settings.json";
#endif

  try {
    json j;
    j["language"] = (currentLanguage_ == Language::Spanish) ? "es" : "en";
    std::ofstream file(savePath);
    if (file.is_open()) {
      file << j.dump(2);
      nxlog::pushf("I18n: saved language '%s' to '%s'", j["language"].get<std::string>().c_str(), savePath);
    }
  } catch (const std::exception& e) {
    nxlog::pushf("I18n saveConfig error: %s", e.what());
  }
}

void I18n::setLanguage(Language lang) {
  if (currentLanguage_ == lang) return;
  currentLanguage_ = lang;
#ifndef SWITUBE_BACKGROUND_SERVICE
  brls::setLocale(currentLanguage_ == Language::Spanish ? brls::LOCALE_ES : brls::LOCALE_EN_US);
#endif
  saveConfig();

  for (const auto& observer : observers_) {
    if (observer.second) observer.second();
  }
}

std::string I18n::get(const std::string& key) const {
  auto it = kTranslations.find(key);
  if (it != kTranslations.end()) {
    return (currentLanguage_ == Language::Spanish) ? it->second.first : it->second.second;
  }
  return key;
}

std::string I18n::format(const std::string& key, const std::string& arg1) const {
  std::string templateStr = get(key);
  size_t pos = templateStr.find("{}");
  if (pos != std::string::npos) {
    templateStr.replace(pos, 2, arg1);
  }
  return templateStr;
}

std::string I18n::format(const std::string& key, const std::string& arg1, const std::string& arg2) const {
  std::string templateStr = get(key);
  size_t pos = templateStr.find("{}");
  if (pos != std::string::npos) {
    templateStr.replace(pos, 2, arg1);
    pos = templateStr.find("{}", pos + arg1.length());
    if (pos != std::string::npos) {
      templateStr.replace(pos, 2, arg2);
    }
  }
  return templateStr;
}

size_t I18n::addObserver(std::function<void()> observer) {
  size_t id = nextObserverId_++;
  observers_.emplace_back(id, std::move(observer));
  return id;
}

void I18n::removeObserver(size_t id) {
  observers_.erase(
      std::remove_if(observers_.begin(), observers_.end(),
                     [id](const auto& pair) { return pair.first == id; }),
      observers_.end());
}

} // namespace switube
