#include <mutex>
#pragma once

#include <string>
#include <vector>
#include <optional>
#include <functional>
#include <cstdint>

namespace switube {

enum class YtSearchFilter {
  All,
  Songs,
  Videos,
  Albums,
  Artists,
  Playlists
};

struct YtTrack {
  std::string id;          // videoId ej. "dQw4w9WgXcQ", playlistId "VLPL..." / "MPRE...", o artistId "UC..."
  std::string title;
  std::string artist;
  std::string album;
  std::string durationStr; // ej. "3:45"
  uint32_t durationMs = 0;
  std::string imageUrl;
  bool isPlaylist = false;
  bool isArtist = false;
};

struct YtShelf {
  std::string title;
  std::vector<YtTrack> tracks;
};

struct YtArtistPage {
  std::string id;
  std::string name;
  std::string description;
  std::string thumbnailUrl;
  std::string subscriberCount;
  std::string allSongsBrowseId;
  std::vector<YtTrack> topSongs;
  std::vector<YtShelf> sections; // Álbumes, Singles & EPs, Vídeos, Directo, Destacado en, Listas, Similares
};

struct AudioStreamUrl {
  std::string url;
  std::string mimeType;   // ej. "audio/webm; codecs=\"opus\""
  std::string codec;      // "opus" o "aac"
  int bitrate = 0;        // ej. 160000
  uint32_t approxDurationMs = 0;
  uint64_t contentLength = 0;
  bool isDirect = true;
};

class YtMusicClient {
 public:
  YtMusicClient();
  ~YtMusicClient();

  bool init();

  // 1. Resuelve la URL directa de audio Opus/AAC mediante el cliente VISIONOS
  bool resolveAudioStream(const std::string& videoId, AudioStreamUrl& outStream);

  // 2. Búsqueda de pistas en YouTube Music con filtros y paginación
  bool searchTracks(const std::string& query,
                    YtSearchFilter filter,
                    std::vector<YtTrack>& outTracks,
                    std::string& outContinuationToken,
                    const std::string& continuationToken = "",
                    size_t limit = 25);

  bool searchTracks(const std::string& query, std::vector<YtTrack>& outTracks, size_t limit = 25) {
    std::string dummyToken;
    return searchTracks(query, YtSearchFilter::All, outTracks, dummyToken, "", limit);
  }

  // 3. Home Feed (Recomendaciones, temas populares)
  bool getHomeFeed(std::vector<YtShelf>& outShelves);

  // 4. Obtener pistas dentro de una playlist o álbum (browseId)
  bool getPlaylistTracks(const std::string& browseId, std::vector<YtTrack>& outTracks);

  // 5. Obtener página completa de artista (Canciones principales, álbumes, singles, vídeos, directo, listas, similares)
  bool getArtistPage(const std::string& artistBrowseId, YtArtistPage& outPage);

  void setAuthCookies(const std::string& cookies);

 private:
  std::string authCookies_;
  std::string visitorData_;
  std::mutex clientMutex_;

  void ensureVisitorData();

  std::string postInnerTube(const std::string& endpoint, const std::string& jsonBody,
                           const std::string& baseHost = "https://music.youtube.com",
                           const std::string& userAgentOverride = "",
                           bool sendOrigin = true);
};

} // namespace switube
