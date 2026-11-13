#ifdef SWITUBE_BACKGROUND_SERVICE
#include <regex>
namespace swui {
inline std::string upgradeThumbnailUrl(const std::string& rawUrl, int targetSize = 544) {
  if (rawUrl.empty() || targetSize < 300) return rawUrl;
  std::string url = rawUrl;
  if (url.find("googleusercontent.com") != std::string::npos ||
      url.find("ggpht.com") != std::string::npos) {
    static const std::regex whRegex("=w\\d+-h\\d+");
    url = std::regex_replace(url, whRegex, "=w" + std::to_string(targetSize) + "-h" + std::to_string(targetSize));
    static const std::regex sRegex("=s\\d+");
    url = std::regex_replace(url, sRegex, "=s" + std::to_string(targetSize));
    if (url.find("=w") == std::string::npos && url.find("=s") == std::string::npos && url.find('?') == std::string::npos) {
      url += "=w" + std::to_string(targetSize) + "-h" + std::to_string(targetSize) + "-l90-rj";
    }
  } else if (url.find("i.ytimg.com") != std::string::npos) {
    auto pos = url.find("/default.jpg");
    if (pos != std::string::npos) {
      url.replace(pos, 12, "/hqdefault.jpg");
    }
  }
  return url;
}
}
#else
#include "ui_helpers.hpp"
#endif
#include "yt_music_client.hpp"
#include "i18n.hpp"

#include <iostream>
#include <sstream>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include "NXConsoleLog.h"

using json = nlohmann::json;

namespace switube {

namespace {

size_t CurlWriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
  size_t totalSize = size * nmemb;
  std::string* str = static_cast<std::string*>(userp);
  str->append(static_cast<char*>(contents), totalSize);
  return totalSize;
}

// Búsqueda recursiva robusta de videoId en cualquier nivel del JSON
std::string findVideoId(const json& j, int depth = 0) {
  if (depth > 8) return "";
  if (j.is_object()) {
    if (j.contains("videoId") && j["videoId"].is_string()) {
      return j["videoId"].get<std::string>();
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (it.key() == "trackingParams") continue;
      std::string id = findVideoId(it.value(), depth + 1);
      if (!id.empty()) return id;
    }
  } else if (j.is_array()) {
    for (const auto& elem : j) {
      std::string id = findVideoId(elem, depth + 1);
      if (!id.empty()) return id;
    }
  }
  return "";
}

// Búsqueda recursiva de browseId (Playlists, Álbumes, Artistas)
std::string findBrowseId(const json& j, int depth = 0) {
  if (depth > 8) return "";
  if (j.is_object()) {
    if (j.contains("browseId") && j["browseId"].is_string()) {
      return j["browseId"].get<std::string>();
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (it.key() == "trackingParams") continue;
      std::string id = findBrowseId(it.value(), depth + 1);
      if (!id.empty()) return id;
    }
  } else if (j.is_array()) {
    for (const auto& elem : j) {
      std::string id = findBrowseId(elem, depth + 1);
      if (!id.empty()) return id;
    }
  }
  return "";
}

// Convierte cadenas "mm:ss" o "hh:mm:ss" a milisegundos
uint32_t parseDurationToMs(const std::string& str) {
  if (str.empty()) return 0;
  int parts[3] = {0, 0, 0};
  int partCount = 0;
  int current = 0;
  bool hasDigits = false;

  for (char c : str) {
    if (c >= '0' && c <= '9') {
      current = current * 10 + (c - '0');
      hasDigits = true;
    } else if (c == ':') {
      if (partCount < 2 && hasDigits) {
        parts[partCount++] = current;
        current = 0;
        hasDigits = false;
      } else {
        return 0;
      }
    } else {
      return 0;
    }
  }

  if (!hasDigits) return 0;
  parts[partCount++] = current;

  if (partCount == 2) {
    // mm:ss
    return static_cast<uint32_t>((parts[0] * 60 + parts[1]) * 1000);
  } else if (partCount == 3) {
    // hh:mm:ss
    return static_cast<uint32_t>((parts[0] * 3600 + parts[1] * 60 + parts[2]) * 1000);
  }
  return 0;
}

// Helper para extraer duración de flexColumns / fixedColumns
void extractDurationFromRow(const json& row, YtTrack& track) {
  if (row.contains("fixedColumns") && row["fixedColumns"].is_array()) {
    for (const auto& col : row["fixedColumns"]) {
      if (col.contains("musicResponsiveListItemFixedColumnRenderer")) {
        const auto& fixed = col["musicResponsiveListItemFixedColumnRenderer"];
        if (fixed.contains("text") && fixed["text"].contains("runs")) {
          const auto& runs = fixed["text"]["runs"];
          for (const auto& r : runs) {
            std::string txt = r.value("text", "");
            uint32_t ms = parseDurationToMs(txt);
            if (ms > 0) {
              track.durationMs = ms;
              track.durationStr = txt;
              return;
            }
          }
        }
      }
    }
  }

  if (row.contains("flexColumns") && row["flexColumns"].is_array()) {
    for (const auto& col : row["flexColumns"]) {
      if (col.contains("musicResponsiveListItemFlexColumnRenderer")) {
        const auto& flex = col["musicResponsiveListItemFlexColumnRenderer"];
        if (flex.contains("text") && flex["text"].contains("runs")) {
          const auto& runs = flex["text"]["runs"];
          for (const auto& r : runs) {
            std::string txt = r.value("text", "");
            uint32_t ms = parseDurationToMs(txt);
            if (ms > 0) {
              track.durationMs = ms;
              track.durationStr = txt;
              return;
            }
          }
        }
      }
    }
  }
}

// Búsqueda recursiva del token de continuación
std::string findContinuationToken(const json& j, int depth = 0) {
  if (depth > 10) return "";
  if (j.is_object()) {
    if (j.contains("nextContinuationData") && j["nextContinuationData"].contains("continuation")) {
      return j["nextContinuationData"]["continuation"].get<std::string>();
    }
    if (j.contains("continuationCommand") && j["continuationCommand"].contains("token")) {
      return j["continuationCommand"]["token"].get<std::string>();
    }
    for (auto it = j.begin(); it != j.end(); ++it) {
      if (it.key() == "trackingParams") continue;
      std::string tok = findContinuationToken(it.value(), depth + 1);
      if (!tok.empty()) return tok;
    }
  } else if (j.is_array()) {
    for (const auto& elem : j) {
      std::string tok = findContinuationToken(elem, depth + 1);
      if (!tok.empty()) return tok;
    }
  }
  return "";
}

} // namespace

YtMusicClient::YtMusicClient() {}

YtMusicClient::~YtMusicClient() {}

bool YtMusicClient::init() {
  curl_global_init(CURL_GLOBAL_ALL);
  return true;
}

void YtMusicClient::setAuthCookies(const std::string& cookies) {
  authCookies_ = cookies;
}

void YtMusicClient::ensureVisitorData() {
  if (!visitorData_.empty()) return;

  CURL* curl = curl_easy_init();
  if (!curl) return;

  std::string responseStr;
  char errorBuffer[CURL_ERROR_SIZE] = {0};

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36");

  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
  curl_easy_setopt(curl, CURLOPT_URL, "https://www.youtube.com/sw.js_data");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
  curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 5000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 8000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseStr);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

  nxlog::push("ensureVisitorData: fetching fresh visitorData from sw.js_data...");
  CURLcode res = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res == CURLE_OK && !responseStr.empty()) {
    size_t pos = 0;
    while ((pos = responseStr.find("Cg", pos)) != std::string::npos) {
      size_t end = pos + 2;
      while (end < responseStr.size() &&
             (isalnum(static_cast<unsigned char>(responseStr[end])) ||
              responseStr[end] == '_' || responseStr[end] == '-' || responseStr[end] == '%')) {
        end++;
      }
      if (end - pos >= 40) {
        visitorData_ = responseStr.substr(pos, end - pos);
        nxlog::pushf("ensureVisitorData: acquired visitorData (%zu chars)", visitorData_.size());
        return;
      }
      pos = end;
    }
  }
}

std::string YtMusicClient::postInnerTube(const std::string& endpoint,
                                         const std::string& jsonBody,
                                         const std::string& baseHost,
                                         const std::string& userAgentOverride,
                                         bool sendOrigin) {
  CURL* curl = curl_easy_init();
  if (!curl) {
    nxlog::push("postInnerTube: curl_easy_init failed!");
    return "";
  }

  std::string responseStr;
  std::string url = (endpoint.rfind("http", 0) == 0) ? endpoint : (baseHost + "/youtubei/v1/" + endpoint);
  char errorBuffer[CURL_ERROR_SIZE] = {0};

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");

  std::string ua = userAgentOverride.empty()
      ? "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Safari/537.36"
      : userAgentOverride;
  std::string uaHdr = "User-Agent: " + ua;
  headers = curl_slist_append(headers, uaHdr.c_str());

  if (sendOrigin) {
    std::string originHdr = "Origin: " + baseHost;
    std::string refererHdr = "Referer: " + baseHost + "/";
    headers = curl_slist_append(headers, originHdr.c_str());
    headers = curl_slist_append(headers, refererHdr.c_str());
  }

  if (!authCookies_.empty()) {
    std::string cookieHdr = "Cookie: " + authCookies_;
    headers = curl_slist_append(headers, cookieHdr.c_str());
  }

  curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, errorBuffer);
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonBody.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(jsonBody.length()));

  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
  curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);

  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);

  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseStr);

  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);

  nxlog::pushf("postInnerTube: performing POST to %s...", endpoint.c_str());
  const CURLcode res = curl_easy_perform(curl);

  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    nxlog::pushf("postInnerTube failed (code %d): %s (detail: %s)",
                 static_cast<int>(res), curl_easy_strerror(res), errorBuffer);
    return "";
  }

  nxlog::pushf("postInnerTube success: HTTP %ld, %zu bytes received", httpCode, responseStr.size());

  // Capturar visitorData de la respuesta si aún no la tenemos (sin parsear todo el JSON para no agotar el heap)
  if (visitorData_.empty() && responseStr.size() > 50) {
    size_t vpos = responseStr.find("\"visitorData\":\"");
    if (vpos != std::string::npos) {
      vpos += 15;
      size_t vend = responseStr.find('"', vpos);
      if (vend != std::string::npos && vend > vpos) {
        visitorData_ = responseStr.substr(vpos, vend - vpos);
        nxlog::pushf("postInnerTube: captured visitorData (%zu chars)", visitorData_.size());
      }
    }
  }

  return responseStr;
}

bool YtMusicClient::resolveAudioStream(const std::string& videoId, AudioStreamUrl& outStream) {
  std::lock_guard<std::mutex> lock(clientMutex_);
  if (videoId.empty() || videoId.size() != 11) {
    nxlog::pushf("resolveAudioStream rejected: '%s' is not an 11-char videoId", videoId.c_str());
    return false;
  }

  auto attemptResolve = [this, &videoId, &outStream]() -> bool {
    ensureVisitorData();

    json clientObj = {
        {"clientName", "VISIONOS"},
        {"clientVersion", "1.02"},
        {"deviceMake", "Apple"},
        {"deviceModel", "RealityDevice17,1"},
        {"osName", "visionOS"},
        {"osVersion", "26.5.23O471"},
        {"hl", I18n::getInstance().getHl()}, {"gl", I18n::getInstance().getGl()}
    };

    if (!visitorData_.empty()) {
      clientObj["visitorData"] = visitorData_;
    }

    json payload = {
        {"context", {
            {"client", clientObj}
        }},
        {"videoId", videoId},
        {"contentCheckOk", true},
        {"racyCheckOk", true}
    };

    std::string visionUa = "Mozilla/5.0 (Macintosh; Intel Mac OS X 15_7_3) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/26.0 Safari/605.1.15";
    std::string resp = postInnerTube("player", payload.dump(), "https://www.youtube.com", visionUa, false);
    if (resp.empty()) return false;

    try {
      auto j = json::parse(resp);
      if (!j.contains("streamingData")) {
        std::string status = j.value("playabilityStatus", json::object()).value("status", "UNKNOWN");
        std::string reason = j.value("playabilityStatus", json::object()).value("reason", "");
        nxlog::pushf("resolveAudioStream: no streamingData (status: %s, reason: %s)", status.c_str(), reason.c_str());
        return false;
      }

      const auto& streamingData = j["streamingData"];
      if (!streamingData.contains("adaptiveFormats")) {
        nxlog::push("resolveAudioStream: streamingData does not contain adaptiveFormats");
        return false;
      }

      const auto& formats = streamingData["adaptiveFormats"];

      // Prioridad 1: Opus (itag 251)
      for (const auto& fmt : formats) {
        if (fmt.contains("mimeType") && fmt.contains("url")) {
          std::string mime = fmt["mimeType"];
          if (mime.find("audio/webm; codecs=\"opus\"") != std::string::npos) {
            outStream.url = fmt["url"];
            outStream.mimeType = mime;
            outStream.codec = "opus";
            outStream.bitrate = fmt.value("bitrate", 160000);
            if (fmt.contains("approxDurationMs")) {
              std::string durStr = fmt["approxDurationMs"];
              outStream.approxDurationMs = static_cast<uint32_t>(std::stoul(durStr));
            }
            if (fmt.contains("contentLength")) {
              try {
                outStream.contentLength = std::stoull(fmt["contentLength"].get<std::string>());
              } catch (...) {}
            }
            outStream.isDirect = true;
            nxlog::pushf("resolveAudioStream SUCCESS: direct Opus URL found (bitrate: %d, bytes: %llu)",
                         outStream.bitrate, static_cast<unsigned long long>(outStream.contentLength));
            return true;
          }
        }
      }

      // Prioridad 2: AAC (itag 140 - mp4a)
      for (const auto& fmt : formats) {
        if (fmt.contains("mimeType") && fmt.contains("url")) {
          std::string mime = fmt["mimeType"];
          if (mime.find("audio/mp4; codecs=\"mp4a") != std::string::npos) {
            outStream.url = fmt["url"];
            outStream.mimeType = mime;
            outStream.codec = "aac";
            outStream.bitrate = fmt.value("bitrate", 128000);
            if (fmt.contains("approxDurationMs")) {
              std::string durStr = fmt["approxDurationMs"];
              outStream.approxDurationMs = static_cast<uint32_t>(std::stoul(durStr));
            }
            if (fmt.contains("contentLength")) {
              try {
                outStream.contentLength = std::stoull(fmt["contentLength"].get<std::string>());
              } catch (...) {}
            }
            outStream.isDirect = true;
            nxlog::pushf("resolveAudioStream SUCCESS: direct AAC URL found (bitrate: %d, bytes: %llu)",
                         outStream.bitrate, static_cast<unsigned long long>(outStream.contentLength));
            return true;
          }
        }
      }
    } catch (const std::exception& e) {
      nxlog::pushf("resolveAudioStream parse error: %s", e.what());
      return false;
    }

    return false;
  };

  nxlog::pushf("resolveAudioStream: requesting stream for videoId '%s' (VisionOS)...", videoId.c_str());
  if (attemptResolve()) return true;

  // Si VisionOS falló, probar con cliente ANDROID_VR
  auto attemptAndroidVr = [this, &videoId, &outStream]() -> bool {
    json clientObj = {
        {"clientName", "ANDROID_VR"},
        {"clientVersion", "1.61.48"},
        {"deviceMake", "Oculus"},
        {"deviceModel", "Quest 3"},
        {"osName", "Android"},
        {"osVersion", "12"},
        {"hl", I18n::getInstance().getHl()}, {"gl", I18n::getInstance().getGl()}
    };
    if (!visitorData_.empty()) clientObj["visitorData"] = visitorData_;

    json payload = {
        {"context", {{"client", clientObj}}},
        {"videoId", videoId},
        {"contentCheckOk", true},
        {"racyCheckOk", true}
    };
    std::string vrUa = "Mozilla/5.0 (Linux; Android 12; Quest 3) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/128.0.0.0 Mobile Safari/537.36";
    std::string resp = postInnerTube("player", payload.dump(), "https://www.youtube.com", vrUa, false);
    if (resp.empty()) return false;

    try {
      auto j = json::parse(resp);
      if (!j.contains("streamingData")) return false;
      const auto& formats = j["streamingData"]["adaptiveFormats"];
      for (const auto& fmt : formats) {
        if (fmt.contains("mimeType") && fmt.contains("url")) {
          std::string mime = fmt["mimeType"];
          if (mime.find("audio/webm; codecs=\"opus\"") != std::string::npos ||
              mime.find("audio/mp4; codecs=\"mp4a") != std::string::npos) {
            outStream.url = fmt["url"];
            outStream.mimeType = mime;
            outStream.codec = (mime.find("opus") != std::string::npos) ? "opus" : "aac";
            outStream.bitrate = fmt.value("bitrate", 128000);
            if (fmt.contains("approxDurationMs")) {
              outStream.approxDurationMs = static_cast<uint32_t>(std::stoul(fmt["approxDurationMs"].get<std::string>()));
            }
            if (fmt.contains("contentLength")) {
              try { outStream.contentLength = std::stoull(fmt["contentLength"].get<std::string>()); } catch (...) {}
            }
            outStream.isDirect = true;
            nxlog::pushf("resolveAudioStream SUCCESS: ANDROID_VR fallback stream found (bitrate: %d)", outStream.bitrate);
            return true;
          }
        }
      }
    } catch (...) {}
    return false;
  };

  nxlog::push("resolveAudioStream: trying ANDROID_VR fallback...");
  if (attemptAndroidVr()) return true;

  // Intento de recuperación final: renovar visitorData y reintentar VisionOS
  nxlog::push("resolveAudioStream: initial attempts failed, refreshing visitorData and retrying...");
  visitorData_.clear();
  ensureVisitorData();
  return attemptResolve();
}

bool YtMusicClient::getPlaylistTracks(const std::string& browseId, std::vector<YtTrack>& outTracks) {
  nxlog::pushf("getPlaylistTracks: fetching tracks for browseId '%s'...", browseId.c_str());
  json clientObj = {
      {"clientName", "WEB_REMIX"},
      {"clientVersion", "1.20240101.01.00"},
      {"hl", I18n::getInstance().getHl()},
      {"gl", I18n::getInstance().getGl()}
  };
  if (!visitorData_.empty()) {
    clientObj["visitorData"] = visitorData_;
  }
  json payload = {
      {"context", {{"client", clientObj}}},
      {"browseId", browseId}
  };

  std::string resp = postInnerTube("browse", payload.dump());
  if (resp.empty()) return false;

  try {
    auto j = json::parse(resp);

    // Extraer metadatos de cabecera del álbum / single / playlist (artista, título y portada por defecto)
    std::string defaultAlbum;
    std::string defaultArtist;
    std::string defaultThumb;

    std::function<void(const json&)> findHeaderMeta = [&](const json& node) {
      if (!node.is_object() && !node.is_array()) return;
      if (node.is_object()) {
        const json* hdr = nullptr;
        if (node.contains("musicResponsiveHeaderRenderer")) {
          hdr = &node["musicResponsiveHeaderRenderer"];
        } else if (node.contains("musicDetailHeaderRenderer")) {
          hdr = &node["musicDetailHeaderRenderer"];
        }
        if (hdr && hdr->is_object()) {
          if (defaultAlbum.empty() && hdr->contains("title") && (*hdr)["title"].contains("runs") &&
              (*hdr)["title"]["runs"].is_array() && !(*hdr)["title"]["runs"].empty()) {
            defaultAlbum = (*hdr)["title"]["runs"][0].value("text", "");
          }
          if (defaultArtist.empty() && hdr->contains("straplineTextOne") &&
              (*hdr)["straplineTextOne"].contains("runs") &&
              (*hdr)["straplineTextOne"]["runs"].is_array() &&
              !(*hdr)["straplineTextOne"]["runs"].empty()) {
            defaultArtist = (*hdr)["straplineTextOne"]["runs"][0].value("text", "");
          }
          if (defaultArtist.empty() && hdr->contains("subtitle") &&
              (*hdr)["subtitle"].contains("runs") &&
              (*hdr)["subtitle"]["runs"].is_array()) {
            const auto& sruns = (*hdr)["subtitle"]["runs"];
            if (sruns.size() >= 3) {
              defaultArtist = sruns[2].value("text", "");
            } else if (!sruns.empty()) {
              defaultArtist = sruns[0].value("text", "");
            }
          }
          if (defaultThumb.empty() && hdr->contains("thumbnail") &&
              (*hdr)["thumbnail"].contains("musicThumbnailRenderer")) {
            const auto& mtr = (*hdr)["thumbnail"]["musicThumbnailRenderer"];
            if (mtr.contains("thumbnail") && mtr["thumbnail"].contains("thumbnails") &&
                mtr["thumbnail"]["thumbnails"].is_array() && !mtr["thumbnail"]["thumbnails"].empty()) {
              defaultThumb = mtr["thumbnail"]["thumbnails"].back().value("url", "");
            }
          }
        }
        for (auto it = node.begin(); it != node.end(); ++it) {
          if (it.key() == "trackingParams") continue;
          findHeaderMeta(it.value());
        }
      } else if (node.is_array()) {
        for (const auto& item : node) {
          findHeaderMeta(item);
        }
      }
    };
    findHeaderMeta(j);

    std::function<void(const json&)> findTrackRows = [&](const json& node) {
      if (node.is_object()) {
        if (node.contains("musicResponsiveListItemRenderer")) {
          const auto& row = node["musicResponsiveListItemRenderer"];
          std::string vid = findVideoId(row);
          if (!vid.empty() && vid.size() == 11) {
            YtTrack t;
            t.id = vid;
            t.isPlaylist = false;
            t.isArtist = false;
            if (row.contains("flexColumns") && row["flexColumns"].is_array() && !row["flexColumns"].empty()) {
              const auto& col0 = row["flexColumns"][0];
              if (col0.contains("musicResponsiveListItemFlexColumnRenderer")) {
                const auto& flex0 = col0["musicResponsiveListItemFlexColumnRenderer"];
                if (flex0.contains("text") && flex0["text"].contains("runs") &&
                    flex0["text"]["runs"].is_array() && !flex0["text"]["runs"].empty()) {
                  t.title = flex0["text"]["runs"][0].value("text", "Unknown");
                }
              }
              if (row["flexColumns"].size() > 1) {
                const auto& col1 = row["flexColumns"][1];
                if (col1.contains("musicResponsiveListItemFlexColumnRenderer")) {
                  const auto& flex1 = col1["musicResponsiveListItemFlexColumnRenderer"];
                  if (flex1.contains("text") && flex1["text"].contains("runs") &&
                      flex1["text"]["runs"].is_array() && !flex1["text"]["runs"].empty()) {
                    const auto& runs = flex1["text"]["runs"];
                    t.artist = runs[0].value("text", "");
                    if (runs.size() >= 3) t.album = runs[2].value("text", "");
                  }
                }
              }
            }
            if (t.artist.empty()) t.artist = defaultArtist.empty() ? "Unknown Artist" : defaultArtist;
            if (t.album.empty() && !defaultAlbum.empty()) t.album = defaultAlbum;

            if (row.contains("thumbnail") && row["thumbnail"].contains("musicThumbnailRenderer")) {
              const auto& mtr = row["thumbnail"]["musicThumbnailRenderer"];
              if (mtr.contains("thumbnail") && mtr["thumbnail"].contains("thumbnails") &&
                  mtr["thumbnail"]["thumbnails"].is_array() && !mtr["thumbnail"]["thumbnails"].empty()) {
                t.imageUrl = mtr["thumbnail"]["thumbnails"].back().value("url", "");
              }
            }
            if (t.imageUrl.empty() && !defaultThumb.empty()) {
              t.imageUrl = defaultThumb;
            }

            // Extraer duración precisa
            extractDurationFromRow(row, t);

            outTracks.push_back(t);
          }
        } else {
          for (auto it = node.begin(); it != node.end(); ++it) {
            if (it.key() == "trackingParams") continue;
            findTrackRows(it.value());
          }
        }
      } else if (node.is_array()) {
        for (const auto& item : node) {
          findTrackRows(item);
        }
      }
    };

    findTrackRows(j);
    nxlog::pushf("getPlaylistTracks: extracted %zu tracks from '%s'", outTracks.size(), browseId.c_str());
    return !outTracks.empty();
  } catch (const std::exception& e) {
    nxlog::pushf("getPlaylistTracks error: %s", e.what());
    return false;
  }
}

bool YtMusicClient::getArtistPage(const std::string& artistBrowseId, YtArtistPage& outPage) {
  outPage.id = artistBrowseId;
  nxlog::pushf("getArtistPage: fetching artist profile for '%s'...", artistBrowseId.c_str());

  json payload = {
      {"context", {
          {"client", {
              {"clientName", "WEB_REMIX"},
              {"clientVersion", "1.20240101.01.00"},
              {"hl", I18n::getInstance().getHl()}, {"gl", I18n::getInstance().getGl()}
          }}
      }},
      {"browseId", artistBrowseId}
  };

  std::string resp = postInnerTube("browse", payload.dump());
  if (resp.empty()) return false;

  try {
    auto j = json::parse(resp);

    // 1. Cabecera (Header: nombre, foto, suscriptores)
    const json* headerObj = nullptr;
    if (j.contains("header")) {
      const auto& h = j["header"];
      if (h.contains("musicImmersiveHeaderRenderer")) {
        headerObj = &h["musicImmersiveHeaderRenderer"];
      } else if (h.contains("musicVisualHeaderRenderer")) {
        headerObj = &h["musicVisualHeaderRenderer"];
      } else if (h.contains("musicHeaderRenderer")) {
        headerObj = &h["musicHeaderRenderer"];
      }
    }

    if (headerObj) {
      if (headerObj->contains("title") && (*headerObj)["title"].contains("runs") && !(*headerObj)["title"]["runs"].empty()) {
        outPage.name = (*headerObj)["title"]["runs"][0].value("text", "");
      }
      if (headerObj->contains("description") && (*headerObj)["description"].contains("runs") && !(*headerObj)["description"]["runs"].empty()) {
        outPage.description = (*headerObj)["description"]["runs"][0].value("text", "");
      }

      const json* thumbNode = nullptr;
      if (headerObj->contains("thumbnail")) thumbNode = &(*headerObj)["thumbnail"];
      else if (headerObj->contains("foregroundThumbnail")) thumbNode = &(*headerObj)["foregroundThumbnail"];

      if (thumbNode && thumbNode->contains("musicThumbnailRenderer")) {
        const auto& thumbs = (*thumbNode)["musicThumbnailRenderer"]["thumbnail"]["thumbnails"];
        if (!thumbs.empty()) {
          outPage.thumbnailUrl = swui::upgradeThumbnailUrl(thumbs.back().value("url", ""), 544);
        }
      }

      if (headerObj->contains("subscriptionButton") && (*headerObj)["subscriptionButton"].contains("subscribeButtonRenderer")) {
        const auto& btn = (*headerObj)["subscriptionButton"]["subscribeButtonRenderer"];
        if (btn.contains("subscriberCountText") && btn["subscriberCountText"].contains("runs") && !btn["subscriberCountText"]["runs"].empty()) {
          outPage.subscriberCount = btn["subscriberCountText"]["runs"][0].value("text", "");
        } else if (btn.contains("subscriberCountWithSubscribeText") && btn["subscriberCountWithSubscribeText"].contains("runs") && !btn["subscriberCountWithSubscribeText"]["runs"].empty()) {
          outPage.subscriberCount = btn["subscriberCountWithSubscribeText"]["runs"][0].value("text", "");
        }
      }
    }

    // 2. Secciones
    if (!j.contains("contents")) return !outPage.name.empty();
    const auto& singleCol = j["contents"].value("singleColumnBrowseResultsRenderer", json::object());
    if (!singleCol.contains("tabs") || singleCol["tabs"].empty()) return !outPage.name.empty();

    const auto& tab = singleCol["tabs"][0].value("tabRenderer", json::object());
    const auto& contentObj = tab.value("content", json::object());
    const auto& sectionList = contentObj.value("sectionListRenderer", json::object());
    const auto& sections = sectionList.value("contents", json::array());

    for (const auto& sec : sections) {
      // 2a. Canciones principales (musicShelfRenderer)
      if (sec.contains("musicShelfRenderer")) {
        const auto& shelf = sec["musicShelfRenderer"];
        if (shelf.contains("bottomEndpoint") && shelf["bottomEndpoint"].contains("browseEndpoint")) {
          outPage.allSongsBrowseId = shelf["bottomEndpoint"]["browseEndpoint"].value("browseId", "");
        } else if (shelf.contains("title") && shelf["title"].contains("runs") && !shelf["title"]["runs"].empty()) {
          const auto& run0 = shelf["title"]["runs"][0];
          if (run0.contains("navigationEndpoint") && run0["navigationEndpoint"].contains("browseEndpoint")) {
            outPage.allSongsBrowseId = run0["navigationEndpoint"]["browseEndpoint"].value("browseId", "");
          }
        }

        if (shelf.contains("contents")) {
          for (const auto& item : shelf["contents"]) {
            if (!item.contains("musicResponsiveListItemRenderer")) continue;
            const auto& row = item["musicResponsiveListItemRenderer"];
            std::string vid = findVideoId(row);
            if (vid.empty() || vid.size() != 11) continue;

            YtTrack t;
            t.id = vid;
            t.artist = outPage.name;
            t.isPlaylist = false;
            t.isArtist = false;

            if (row.contains("flexColumns") && row["flexColumns"].is_array() && !row["flexColumns"].empty()) {
              const auto& col0 = row["flexColumns"][0];
              if (col0.contains("musicResponsiveListItemFlexColumnRenderer")) {
                const auto& flex0 = col0["musicResponsiveListItemFlexColumnRenderer"];
                if (flex0.contains("text") && flex0["text"].contains("runs") &&
                    flex0["text"]["runs"].is_array() && !flex0["text"]["runs"].empty()) {
                  t.title = flex0["text"]["runs"][0].value("text", "Unknown");
                }
              }
              if (row["flexColumns"].size() > 1) {
                const auto& col1 = row["flexColumns"][1];
                if (col1.contains("musicResponsiveListItemFlexColumnRenderer")) {
                  const auto& flex1 = col1["musicResponsiveListItemFlexColumnRenderer"];
                  if (flex1.contains("text") && flex1["text"].contains("runs") &&
                      flex1["text"]["runs"].is_array() && !flex1["text"]["runs"].empty()) {
                    const auto& runs = flex1["text"]["runs"];
                    t.artist = runs[0].value("text", outPage.name);
                    if (runs.size() >= 3) t.album = runs[2].value("text", "");
                  }
                }
              }
            }

            if (row.contains("thumbnail") && row["thumbnail"].contains("musicThumbnailRenderer")) {
              const auto& mtr = row["thumbnail"]["musicThumbnailRenderer"];
              if (mtr.contains("thumbnail") && mtr["thumbnail"].contains("thumbnails") &&
                  mtr["thumbnail"]["thumbnails"].is_array() && !mtr["thumbnail"]["thumbnails"].empty()) {
                t.imageUrl = mtr["thumbnail"]["thumbnails"].back().value("url", "");
              }
            }

            extractDurationFromRow(row, t);
            outPage.topSongs.push_back(t);
          }
        }
      }
      // 2b. Carruseles (Álbumes, Singles, Vídeos, Directo, Destacado en, Listas, Similares)
      else if (sec.contains("musicCarouselShelfRenderer")) {
        const auto& carousel = sec["musicCarouselShelfRenderer"];
        YtShelf shelf;

        if (carousel.contains("header") && carousel["header"].contains("musicCarouselShelfBasicHeaderRenderer")) {
          const auto& hdr = carousel["header"]["musicCarouselShelfBasicHeaderRenderer"];
          if (hdr.contains("title") && hdr["title"].contains("runs") &&
              hdr["title"]["runs"].is_array() && !hdr["title"]["runs"].empty()) {
            shelf.title = hdr["title"]["runs"][0].value("text", "");
          }
        }
        if (shelf.title.empty()) continue;

        if (carousel.contains("contents") && carousel["contents"].is_array()) {
          for (const auto& citem : carousel["contents"]) {
            const json* rowPtr = nullptr;
            if (citem.contains("musicTwoRowItemRenderer")) {
              rowPtr = &citem["musicTwoRowItemRenderer"];
            } else if (citem.contains("musicResponsiveListItemRenderer")) {
              rowPtr = &citem["musicResponsiveListItemRenderer"];
            }
            if (!rowPtr || !rowPtr->is_object()) continue;
            const auto& row = *rowPtr;

            YtTrack track;
            if (row.contains("title") && row["title"].contains("runs") &&
                row["title"]["runs"].is_array() && !row["title"]["runs"].empty()) {
              track.title = row["title"]["runs"][0].value("text", "");
            }
            if (row.contains("subtitle") && row["subtitle"].contains("runs") &&
                row["subtitle"]["runs"].is_array() && !row["subtitle"]["runs"].empty()) {
              std::string sub;
              for (const auto& r : row["subtitle"]["runs"]) {
                sub += r.value("text", "");
              }
              track.artist = sub;
            }

            const json* thumbObj = nullptr;
            if (row.contains("thumbnail") && row["thumbnail"].contains("musicThumbnailRenderer")) {
              thumbObj = &row["thumbnail"]["musicThumbnailRenderer"];
            } else if (row.contains("thumbnailRenderer") && row["thumbnailRenderer"].contains("musicThumbnailRenderer")) {
              thumbObj = &row["thumbnailRenderer"]["musicThumbnailRenderer"];
            }
            if (thumbObj && (*thumbObj).contains("thumbnail") && (*thumbObj)["thumbnail"].contains("thumbnails")) {
              const auto& thumbs = (*thumbObj)["thumbnail"]["thumbnails"];
              if (!thumbs.empty()) track.imageUrl = thumbs.back().value("url", "");
            }

            extractDurationFromRow(row, track);

            std::string vid;
            if (row.contains("navigationEndpoint") && row["navigationEndpoint"].contains("watchEndpoint")) {
              vid = row["navigationEndpoint"]["watchEndpoint"].value("videoId", "");
            }
            if (vid.empty() && row.contains("playlistItemData")) {
              vid = row["playlistItemData"].value("videoId", "");
            }

            std::string bid;
            std::string pageType;
            if (row.contains("navigationEndpoint") && row["navigationEndpoint"].contains("browseEndpoint")) {
              const auto& bep = row["navigationEndpoint"]["browseEndpoint"];
              bid = bep.value("browseId", "");
              if (bep.contains("browseEndpointContextSupportedConfigs")) {
                const auto& cfg = bep["browseEndpointContextSupportedConfigs"];
                if (cfg.contains("browseEndpointContextMusicConfig")) {
                  pageType = cfg["browseEndpointContextMusicConfig"].value("pageType", "");
                }
              }
            }

            if (!vid.empty() && vid.size() == 11) {
              track.id = vid;
              track.isPlaylist = false;
              track.isArtist = false;
            } else if (!bid.empty()) {
              track.id = bid;
              if (pageType.find("ARTIST") != std::string::npos || (bid.rfind("UC", 0) == 0 && vid.empty())) {
                track.isArtist = true;
                track.isPlaylist = false;
              } else {
                track.isPlaylist = true;
                track.isArtist = false;
              }
            } else {
              continue;
            }

            if (!track.id.empty() && !track.title.empty()) {
              shelf.tracks.push_back(track);
            }
          }
        }

        if (!shelf.tracks.empty()) {
          outPage.sections.push_back(shelf);
        }
      }
    }

    nxlog::pushf("getArtistPage: artist '%s' loaded (topSongs: %zu, sections: %zu)",
                 outPage.name.c_str(), outPage.topSongs.size(), outPage.sections.size());
    return true;
  } catch (const std::exception& e) {
    nxlog::pushf("getArtistPage parse error: %s", e.what());
    return false;
  }
}

bool YtMusicClient::searchTracks(const std::string& query,
                                 YtSearchFilter filter,
                                 std::vector<YtTrack>& outTracks,
                                 std::string& outContinuationToken,
                                 const std::string& continuationToken,
                                 size_t limit) {
  outContinuationToken.clear();

  std::string endpoint = "search";
  json payload = {
      {"context", {
          {"client", {
              {"clientName", "WEB_REMIX"},
              {"clientVersion", "1.20240101.01.00"},
              {"hl", I18n::getInstance().getHl()}, {"gl", I18n::getInstance().getGl()}
          }}
      }}
  };

  if (continuationToken.empty()) {
    payload["query"] = query;

    // BitChord exact InnerTube filter params
    std::string params = "";
    switch (filter) {
      case YtSearchFilter::Songs:
        params = "EgWKAQIIAWoKEAkQChAFEAMQBA==";
        break;
      case YtSearchFilter::Videos:
        params = "EgWKAQIQAWoKEAkQChAFEAMQBA==";
        break;
      case YtSearchFilter::Albums:
        params = "EgWKAQIYAWoKEAkQChAFEAMQBA==";
        break;
      case YtSearchFilter::Artists:
        params = "EgWKAQIgAWoKEAkQChAFEAMQBA==";
        break;
      case YtSearchFilter::Playlists:
        params = "EgWKAQIoAWoKEAkQChAFEAMQBA==";
        break;
      case YtSearchFilter::All:
      default:
        break;
    }
    if (!params.empty()) {
      payload["params"] = params;
    }
    nxlog::pushf("searchTracks: querying '%s' (filter: %d)...", query.c_str(), static_cast<int>(filter));
  } else {
    endpoint = "search?continuation=" + continuationToken + "&ctoken=" + continuationToken + "&type=next";
    payload["continuation"] = continuationToken;
    nxlog::pushf("searchTracks: fetching next page with continuation token (%zu chars)...", continuationToken.size());
  }

  std::string resp = postInnerTube(endpoint, payload.dump());
  if (resp.empty()) return false;

  try {
    auto j = json::parse(resp);

    // Extraer token de continuación si existe
    std::string nextTok = findContinuationToken(j);
    if (!nextTok.empty() && nextTok != continuationToken) {
      outContinuationToken = nextTok;
      nxlog::pushf("searchTracks: found next continuationToken (%zu chars)", outContinuationToken.size());
    }

    std::function<void(const json&)> findSearchTracks = [&](const json& node) {
      if (outTracks.size() >= limit) return;
      if (node.is_object()) {
        if (node.contains("musicResponsiveListItemRenderer")) {
          const auto& row = node["musicResponsiveListItemRenderer"];
          std::string bid;
          std::string pageType;
          if (row.contains("navigationEndpoint") && row["navigationEndpoint"].contains("browseEndpoint")) {
            const auto& bep = row["navigationEndpoint"]["browseEndpoint"];
            bid = bep.value("browseId", "");
            if (bep.contains("browseEndpointContextSupportedConfigs")) {
              const auto& cfg = bep["browseEndpointContextSupportedConfigs"];
              if (cfg.contains("browseEndpointContextMusicConfig")) {
                pageType = cfg["browseEndpointContextMusicConfig"].value("pageType", "");
              }
            }
          }
          if (bid.empty() && row.contains("flexColumns") && !row["flexColumns"].empty()) {
            const auto& col0 = row["flexColumns"][0];
            if (col0.contains("musicResponsiveListItemFlexColumnRenderer")) {
              const auto& flex0 = col0["musicResponsiveListItemFlexColumnRenderer"];
              if (flex0.contains("text") && flex0["text"].contains("runs") && !flex0["text"]["runs"].empty()) {
                const auto& r0 = flex0["text"]["runs"][0];
                if (r0.contains("navigationEndpoint") && r0["navigationEndpoint"].contains("browseEndpoint")) {
                  const auto& bep = r0["navigationEndpoint"]["browseEndpoint"];
                  bid = bep.value("browseId", "");
                  if (bep.contains("browseEndpointContextSupportedConfigs")) {
                    const auto& cfg = bep["browseEndpointContextSupportedConfigs"];
                    if (cfg.contains("browseEndpointContextMusicConfig")) {
                      pageType = cfg["browseEndpointContextMusicConfig"].value("pageType", "");
                    }
                  }
                }
              }
            }
          }

          const bool forceBrowseFilter =
              (filter == YtSearchFilter::Albums ||
               filter == YtSearchFilter::Artists ||
               filter == YtSearchFilter::Playlists);
          if (bid.empty() && forceBrowseFilter) {
            bid = findBrowseId(row);
          }

          std::string vid;
          if (bid.empty() && !forceBrowseFilter) {
            if (row.contains("playlistItemData") && row["playlistItemData"].is_object()) {
              vid = row["playlistItemData"].value("videoId", "");
            }
            if (vid.empty() && row.contains("navigationEndpoint") && row["navigationEndpoint"].contains("watchEndpoint")) {
              vid = row["navigationEndpoint"]["watchEndpoint"].value("videoId", "");
            }
            if (vid.empty()) {
              vid = findVideoId(row);
            }
          }

          YtTrack track;
          if (!bid.empty() || forceBrowseFilter) {
            if (bid.empty()) bid = findBrowseId(row);
            track.id = bid;
            if (filter == YtSearchFilter::Artists ||
                pageType.find("ARTIST") != std::string::npos ||
                pageType.find("USER_CHANNEL") != std::string::npos ||
                bid.rfind("UC", 0) == 0) {
              track.isArtist = true;
              track.isPlaylist = false;
            } else {
              track.isPlaylist = true;
              track.isArtist = false;
            }
          } else if (!vid.empty() && vid.size() == 11) {
            track.id = vid;
            track.isPlaylist = false;
            track.isArtist = false;
          } else {
            std::string fallbackBid = findBrowseId(row);
            if (!fallbackBid.empty()) {
              track.id = fallbackBid;
              if (fallbackBid.rfind("UC", 0) == 0) {
                track.isArtist = true;
                track.isPlaylist = false;
              } else {
                track.isPlaylist = true;
                track.isArtist = false;
              }
            }
          }

          if (!track.id.empty()) {
            if (row.contains("flexColumns") && row["flexColumns"].is_array() && !row["flexColumns"].empty()) {
              const auto& col0 = row["flexColumns"][0];
              if (col0.contains("musicResponsiveListItemFlexColumnRenderer")) {
                const auto& flex0 = col0["musicResponsiveListItemFlexColumnRenderer"];
                if (flex0.contains("text") && flex0["text"].contains("runs") &&
                    flex0["text"]["runs"].is_array() && !flex0["text"]["runs"].empty()) {
                  track.title = flex0["text"]["runs"][0].value("text", "Unknown");
                }
              }
              if (row["flexColumns"].size() > 1) {
                const auto& col1 = row["flexColumns"][1];
                if (col1.contains("musicResponsiveListItemFlexColumnRenderer")) {
                  const auto& flex1 = col1["musicResponsiveListItemFlexColumnRenderer"];
                  if (flex1.contains("text") && flex1["text"].contains("runs") &&
                      flex1["text"]["runs"].is_array() && !flex1["text"]["runs"].empty()) {
                    const auto& runs = flex1["text"]["runs"];
                    if (track.isPlaylist || track.isArtist || filter == YtSearchFilter::All) {
                      std::string sub;
                      for (const auto& r : runs) {
                        sub += r.value("text", "");
                      }
                      track.artist = sub;
                    } else {
                      track.artist = runs[0].value("text", "Unknown Artist");
                      if (runs.size() >= 3) track.album = runs[2].value("text", "");
                    }
                  }
                }
              }
            }

            if (track.title.empty() && row.contains("title") && row["title"].contains("runs") &&
                row["title"]["runs"].is_array() && !row["title"]["runs"].empty()) {
              track.title = row["title"]["runs"][0].value("text", "");
            }
            if (track.artist.empty() && row.contains("subtitle") && row["subtitle"].contains("runs") &&
                row["subtitle"]["runs"].is_array() && !row["subtitle"]["runs"].empty()) {
              std::string sub;
              for (const auto& r : row["subtitle"]["runs"]) {
                sub += r.value("text", "");
              }
              track.artist = sub;
            }

            if (row.contains("thumbnail") && row["thumbnail"].contains("musicThumbnailRenderer")) {
              const auto& mtr = row["thumbnail"]["musicThumbnailRenderer"];
              if (mtr.contains("thumbnail") && mtr["thumbnail"].contains("thumbnails") &&
                  mtr["thumbnail"]["thumbnails"].is_array() && !mtr["thumbnail"]["thumbnails"].empty()) {
                track.imageUrl = mtr["thumbnail"]["thumbnails"].back().value("url", "");
              }
            }

            // Extraer duración
            extractDurationFromRow(row, track);

            outTracks.push_back(track);
          }
        } else {
          for (auto it = node.begin(); it != node.end(); ++it) {
            if (it.key() == "trackingParams") continue;
            findSearchTracks(it.value());
          }
        }
      } else if (node.is_array()) {
        for (const auto& elem : node) {
          findSearchTracks(elem);
        }
      }
    };

    findSearchTracks(j);
    nxlog::pushf("searchTracks: parsed %zu tracks (hasMore: %s)",
                 outTracks.size(), outContinuationToken.empty() ? "NO" : "YES");
    return !outTracks.empty();
  } catch (const std::exception& e) {
    nxlog::pushf("searchTracks parse error: %s", e.what());
    return false;
  }
}

bool YtMusicClient::getHomeFeed(std::vector<YtShelf>& outShelves) {
  auto parseCarouselSections = [this, &outShelves](const json& sections) {
    if (!sections.is_array()) return;
    for (const auto& sec : sections) {
      if (!sec.is_object() || !sec.contains("musicCarouselShelfRenderer")) continue;
      const auto& carousel = sec["musicCarouselShelfRenderer"];
      YtShelf shelf;

      if (carousel.contains("header") && carousel["header"].contains("musicCarouselShelfBasicHeaderRenderer")) {
        const auto& hdr = carousel["header"]["musicCarouselShelfBasicHeaderRenderer"];
        if (hdr.contains("title") && hdr["title"].contains("runs") &&
            hdr["title"]["runs"].is_array() && !hdr["title"]["runs"].empty()) {
          shelf.title = hdr["title"]["runs"][0].value("text", "");
        }
      }
      if (shelf.title.empty() ||
          shelf.title.find("nimo") != std::string::npos ||
          shelf.title.find("neros") != std::string::npos ||
          shelf.title.find("Moods") != std::string::npos ||
          shelf.title.find("Genres") != std::string::npos ||
          shelf.title.find("pisodios") != std::string::npos) {
        continue;
      }

      // Evitar secciones duplicadas
      bool exists = false;
      for (const auto& s : outShelves) {
        if (s.title == shelf.title) {
          exists = true;
          break;
        }
      }
      if (exists) continue;

      if (carousel.contains("contents") && carousel["contents"].is_array()) {
        for (const auto& item : carousel["contents"]) {
          const json* rowPtr = nullptr;
          if (item.contains("musicTwoRowItemRenderer")) {
            rowPtr = &item["musicTwoRowItemRenderer"];
          } else if (item.contains("musicResponsiveListItemRenderer")) {
            rowPtr = &item["musicResponsiveListItemRenderer"];
          }

          if (!rowPtr || !rowPtr->is_object()) continue;
          const auto& row = *rowPtr;
          YtTrack track;

          // 1. Verificar primero navigationEndpoint.browseEndpoint (Álbumes, Singles, Playlists, Artistas)
          std::string bid;
          std::string pageType;
          if (row.contains("navigationEndpoint") && row["navigationEndpoint"].contains("browseEndpoint")) {
            const auto& bep = row["navigationEndpoint"]["browseEndpoint"];
            bid = bep.value("browseId", "");
            if (bep.contains("browseEndpointContextSupportedConfigs")) {
              const auto& cfg = bep["browseEndpointContextSupportedConfigs"];
              if (cfg.contains("browseEndpointContextMusicConfig")) {
                pageType = cfg["browseEndpointContextMusicConfig"].value("pageType", "");
              }
            }
          }

          // Ignorar categorías de ánimo/géneros y episodios de podcast no musicales
          if (bid == "FEmusic_moods_and_genres_category" ||
              bid.rfind("MPED", 0) == 0 ||
              bid.rfind("MPSP", 0) == 0) {
            continue;
          }

          std::string vid;
          if (bid.empty()) {
            if (row.contains("playlistItemData") && row["playlistItemData"].is_object()) {
              vid = row["playlistItemData"].value("videoId", "");
            }
            if (vid.empty() && row.contains("navigationEndpoint") && row["navigationEndpoint"].contains("watchEndpoint")) {
              vid = row["navigationEndpoint"]["watchEndpoint"].value("videoId", "");
            }
            if (vid.empty()) {
              vid = findVideoId(row);
            }
          }

          if (!bid.empty()) {
            track.id = bid;
            if (pageType.find("ARTIST") != std::string::npos ||
                pageType.find("USER_CHANNEL") != std::string::npos ||
                bid.rfind("UC", 0) == 0) {
              track.isArtist = true;
              track.isPlaylist = false;
            } else {
              track.isPlaylist = true;
              track.isArtist = false;
            }
          } else if (!vid.empty() && vid.size() == 11) {
            track.id = vid;
            track.isPlaylist = false;
            track.isArtist = false;
          } else {
            std::string fallbackBid = findBrowseId(row);
            if (fallbackBid.empty() ||
                fallbackBid == "FEmusic_moods_and_genres_category" ||
                fallbackBid.rfind("MPED", 0) == 0 ||
                fallbackBid.rfind("MPSP", 0) == 0) {
              continue;
            }
            track.id = fallbackBid;
            if (fallbackBid.rfind("UC", 0) == 0) {
              track.isArtist = true;
              track.isPlaylist = false;
            } else {
              track.isPlaylist = true;
              track.isArtist = false;
            }
          }

          // 2. Title & Artist (flexColumns para musicResponsiveListItemRenderer)
          if (row.contains("flexColumns") && row["flexColumns"].is_array() && !row["flexColumns"].empty()) {
            const auto& flex = row["flexColumns"];
            if (flex.size() > 0 && flex[0].contains("musicResponsiveListItemFlexColumnRenderer")) {
              const auto& flex0 = flex[0]["musicResponsiveListItemFlexColumnRenderer"];
              if (flex0.contains("text") && flex0["text"].contains("runs") &&
                  flex0["text"]["runs"].is_array() && !flex0["text"]["runs"].empty()) {
                track.title = flex0["text"]["runs"][0].value("text", "");
              }
            }
            if (flex.size() > 1 && flex[1].contains("musicResponsiveListItemFlexColumnRenderer")) {
              const auto& flex1 = flex[1]["musicResponsiveListItemFlexColumnRenderer"];
              if (flex1.contains("text") && flex1["text"].contains("runs") &&
                  flex1["text"]["runs"].is_array() && !flex1["text"]["runs"].empty()) {
                track.artist = flex1["text"]["runs"][0].value("text", "");
              }
            }
          }

          // Fallback a title / subtitle runs (musicTwoRowItemRenderer)
          if (track.title.empty() && row.contains("title") && row["title"].contains("runs") &&
              row["title"]["runs"].is_array() && !row["title"]["runs"].empty()) {
            track.title = row["title"]["runs"][0].value("text", "");
          }
          if (track.artist.empty() && row.contains("subtitle") && row["subtitle"].contains("runs") &&
              row["subtitle"]["runs"].is_array() && !row["subtitle"]["runs"].empty()) {
            std::string sub;
            for (const auto& r : row["subtitle"]["runs"]) {
              sub += r.value("text", "");
            }
            track.artist = sub;
          }

          // 3. Miniatura / Carátula (thumbnail y thumbnailRenderer)
          const json* thumbObj = nullptr;
          if (row.contains("thumbnail") && row["thumbnail"].contains("musicThumbnailRenderer")) {
            thumbObj = &row["thumbnail"]["musicThumbnailRenderer"];
          } else if (row.contains("thumbnailRenderer") && row["thumbnailRenderer"].contains("musicThumbnailRenderer")) {
            thumbObj = &row["thumbnailRenderer"]["musicThumbnailRenderer"];
          }

          if (thumbObj && thumbObj->contains("thumbnail") && (*thumbObj)["thumbnail"].contains("thumbnails") &&
              (*thumbObj)["thumbnail"]["thumbnails"].is_array() && !(*thumbObj)["thumbnail"]["thumbnails"].empty()) {
            track.imageUrl = (*thumbObj)["thumbnail"]["thumbnails"].back().value("url", "");
          }

          // Extraer duración
          extractDurationFromRow(row, track);

          if (!track.id.empty() && !track.title.empty()) {
            shelf.tracks.push_back(track);
          }
        }
      }

      if (!shelf.tracks.empty()) {
        outShelves.push_back(shelf);
      }
    }
  };

  auto fetchBrowseWithContinuations = [this, &parseCarouselSections](const std::string& browseId, int maxContinuations) {
    json clientObj = {
        {"clientName", "WEB_REMIX"},
        {"clientVersion", "1.20240101.01.00"},
        {"hl", I18n::getInstance().getHl()},
        {"gl", I18n::getInstance().getGl()}
    };
    if (!visitorData_.empty()) {
      clientObj["visitorData"] = visitorData_;
    }
    json payload = {
        {"context", {{"client", clientObj}}},
        {"browseId", browseId}
    };

    std::string resp = postInnerTube("browse", payload.dump());
    if (resp.empty()) return;

    std::string ctok;
    try {
      auto j = json::parse(resp);
      if (j.contains("contents")) {
        auto singleCol = j["contents"].value("singleColumnBrowseResultsRenderer", json::object());
        if (singleCol.contains("tabs") && !singleCol["tabs"].empty()) {
          auto tab = singleCol["tabs"][0].value("tabRenderer", json::object());
          auto contentObj = tab.value("content", json::object());
          auto sectionList = contentObj.value("sectionListRenderer", json::object());
          parseCarouselSections(sectionList.value("contents", json::array()));
          if (sectionList.contains("continuations") && sectionList["continuations"].is_array() &&
              !sectionList["continuations"].empty()) {
            const auto& c0 = sectionList["continuations"][0];
            if (c0.contains("nextContinuationData")) {
              ctok = c0["nextContinuationData"].value("continuation", "");
            }
          }
        }
      }
    } catch (const std::exception& e) {
      nxlog::pushf("getHomeFeed parse error (%s): %s", browseId.c_str(), e.what());
    }

    // Seguir páginas de continuación de FEmusic_home (como hace BitChord)
    for (int page = 0; page < maxContinuations && !ctok.empty(); ++page) {
      json contClient = {
          {"clientName", "WEB_REMIX"},
          {"clientVersion", "1.20240101.01.00"},
          {"hl", I18n::getInstance().getHl()},
          {"gl", I18n::getInstance().getGl()}
      };
      if (!visitorData_.empty()) {
        contClient["visitorData"] = visitorData_;
      }
      json contPayload = {
          {"context", {{"client", contClient}}},
          {"continuation", ctok}
      };
      std::string endpoint = "browse?ctoken=" + ctok + "&continuation=" + ctok + "&type=next";
      ctok.clear();

      std::string contResp = postInnerTube(endpoint, contPayload.dump());
      if (contResp.empty()) break;

      try {
        auto cj = json::parse(contResp);
        if (cj.contains("continuationContents") &&
            cj["continuationContents"].contains("sectionListContinuation")) {
          const auto& slc = cj["continuationContents"]["sectionListContinuation"];
          if (slc.contains("contents")) {
            parseCarouselSections(slc["contents"]);
          }
          if (slc.contains("continuations") && slc["continuations"].is_array() &&
              !slc["continuations"].empty()) {
            const auto& c0 = slc["continuations"][0];
            if (c0.contains("nextContinuationData")) {
              ctok = c0["nextContinuationData"].value("continuation", "");
            }
          }
        }
      } catch (const std::exception& e) {
        nxlog::pushf("getHomeFeed continuation parse error: %s", e.what());
        break;
      }
    }
  };

  // 1. Carruseles principales de Inicio + sus páginas de continuación (Selección rápida, Nuevos lanzamientos, etc.)
  fetchBrowseWithContinuations("FEmusic_home", 3);

  // 2. Carruseles de Explorar (Álbumes y sencillos nuevos, Tendencias, Videos musicales nuevos)
  fetchBrowseWithContinuations("FEmusic_explore", 0);

  // 3. Fallback de seguridad
  if (outShelves.empty()) {
    nxlog::push("getHomeFeed: FEmusic yielded no tracks, querying Top Hits for Home...");
    std::vector<YtTrack> topTracks;
    std::string dummyTok;
    if (searchTracks("Top Hits", YtSearchFilter::All, topTracks, dummyTok, "", 20)) {
      YtShelf trendingShelf;
      trendingShelf.title = "Tendencias y Canciones Populares";
      trendingShelf.tracks = topTracks;
      outShelves.push_back(trendingShelf);
    }
  }

  nxlog::pushf("getHomeFeed completed: %zu shelves ready for display", outShelves.size());
  return !outShelves.empty();
}

} // namespace switube
