#include "update.hpp"

#include <borealis.hpp>
#include <curl/curl.h>
#include <nlohmann/json.hpp>

#ifdef __SWITCH__
#include <switch.h>
#include <minizip/unzip.h>
#endif

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/stat.h>

#include "NXConsoleLog.h"
#include "i18n.hpp"
#include "thread_helper.hpp"
#include "ui_helpers.hpp"

#ifndef APP_VERSION
#define APP_VERSION "0.1.0"
#endif

#ifndef UPDATE_API_URL
#define UPDATE_API_URL "https://api.github.com/repos/dl3g0/Switube/releases/latest"
#endif

#ifndef UPDATE_TAG_URL
#define UPDATE_TAG_URL "https://api.github.com/repos/dl3g0/Switube/releases/tags/"
#endif

using json = nlohmann::json;

namespace switube::update {
namespace {

std::string selfNro;

std::string pendingPath() {
  return selfNro.empty() ? "" : selfNro + ".new";
}

void parseVersion(const std::string& s, int out[3]) {
  out[0] = out[1] = out[2] = 0;
  size_t i = (!s.empty() && (s[0] == 'v' || s[0] == 'V')) ? 1 : 0;
  for (int part = 0; part < 3 && i < s.size(); part++) {
    int n = 0;
    bool any = false;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
      n = n * 10 + (s[i++] - '0');
      any = true;
    }
    if (!any) break;
    out[part] = n;
    if (i >= s.size() || s[i] != '.') break;
    i++;
  }
}

bool isNewer(const std::string& candidate, const std::string& current) {
  int a[3], b[3];
  parseVersion(candidate, a);
  parseVersion(current, b);
  for (int i = 0; i < 3; i++) {
    if (a[i] != b[i]) return a[i] > b[i];
  }
  return false;
}

size_t writeToString(void* contents, size_t size, size_t nmemb, void* userp) {
  size_t total = size * nmemb;
  static_cast<std::string*>(userp)->append(static_cast<const char*>(contents), total);
  return total;
}

bool httpGet(const std::string& url, std::string& outBody, std::string& outErr) {
  outBody.clear();
  outErr.clear();

  CURL* curl = curl_easy_init();
  if (!curl) {
    outErr = "curl_easy_init failed";
    return false;
  }

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Accept: application/vnd.github+json");
  headers = curl_slist_append(headers, "User-Agent: Switube/" APP_VERSION);

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 20000L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToString);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outBody);

  CURLcode res = curl_easy_perform(curl);
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    outErr = curl_easy_strerror(res);
    return false;
  }
  if (httpCode < 200 || httpCode >= 300) {
    outErr = "HTTP " + std::to_string(httpCode);
    return false;
  }
  return true;
}

struct DownloadContext {
  FILE* fp = nullptr;
  std::function<void(int64_t, int64_t)> progress;
};

size_t writeToFile(void* contents, size_t size, size_t nmemb, void* userp) {
  auto* ctx = static_cast<DownloadContext*>(userp);
  if (!ctx || !ctx->fp) return 0;
  return std::fwrite(contents, size, nmemb, ctx->fp);
}

int xferProgress(void* clientp, curl_off_t dltotal, curl_off_t dlnow,
                 curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
  auto* ctx = static_cast<DownloadContext*>(clientp);
  if (ctx && ctx->progress) {
    ctx->progress(static_cast<int64_t>(dlnow), static_cast<int64_t>(dltotal));
  }
  return 0;
}

bool httpDownload(const std::string& url, const std::string& destPath,
                  std::string& outErr,
                  std::function<void(int64_t, int64_t)> onProgress) {
  outErr.clear();
  std::string tmpPath = destPath + ".part";
  std::remove(tmpPath.c_str());

  FILE* fp = std::fopen(tmpPath.c_str(), "wb");
  if (!fp) {
    outErr = std::string("Cannot write ") + tmpPath + ": " + std::strerror(errno);
    return false;
  }

  CURL* curl = curl_easy_init();
  if (!curl) {
    std::fclose(fp);
    std::remove(tmpPath.c_str());
    outErr = "curl_easy_init failed";
    return false;
  }

  DownloadContext ctx{fp, std::move(onProgress)};
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "User-Agent: Switube/" APP_VERSION);

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, 10000L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 20L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeToFile);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &ctx);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferProgress);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx);

  CURLcode res = curl_easy_perform(curl);
  long httpCode = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  std::fclose(fp);

  if (res != CURLE_OK) {
    std::remove(tmpPath.c_str());
    outErr = curl_easy_strerror(res);
    return false;
  }
  if (httpCode < 200 || httpCode >= 300) {
    std::remove(tmpPath.c_str());
    outErr = "HTTP " + std::to_string(httpCode);
    return false;
  }

  std::remove(destPath.c_str());
  if (std::rename(tmpPath.c_str(), destPath.c_str()) != 0) {
    std::remove(tmpPath.c_str());
    outErr = std::string("Rename failed: ") + std::strerror(errno);
    return false;
  }
  return true;
}

void ensureParentDirs(const std::string& filePath) {
  size_t pos = 0;
  // Skip sdmc:/ prefix if present
  if (filePath.rfind("sdmc:/", 0) == 0) {
    pos = 6;
  } else if (!filePath.empty() && filePath[0] == '/') {
    pos = 1;
  }
  while ((pos = filePath.find('/', pos)) != std::string::npos) {
    std::string dir = filePath.substr(0, pos);
    if (!dir.empty()) {
      mkdir(dir.c_str(), 0755);
    }
    ++pos;
  }
}

bool extractUpdateZip(const std::string& zipPath,
                      const std::string& nroPendingPath,
                      std::string& outErr) {
#ifndef __SWITCH__
  (void)zipPath;
  (void)nroPendingPath;
  outErr = "ZIP extraction only supported on Switch";
  return false;
#else
  unzFile uf = unzOpen64(zipPath.c_str());
  if (!uf) {
    outErr = "Failed to open update zip";
    return false;
  }

  unz_global_info64 gi{};
  if (unzGetGlobalInfo64(uf, &gi) != UNZ_OK) {
    unzClose(uf);
    outErr = "Failed to read zip header";
    return false;
  }

  bool extractedNro = false;
  char buf[32 * 1024];

  int rc = unzGoToFirstFile(uf);
  while (rc == UNZ_OK) {
    unz_file_info64 fileInfo{};
    char rawName[512] = {};
    if (unzGetCurrentFileInfo64(uf, &fileInfo, rawName, sizeof(rawName) - 1,
                                nullptr, 0, nullptr, 0) != UNZ_OK) {
      unzClose(uf);
      outErr = "Failed to read zip entry";
      return false;
    }

    std::string entry(rawName);
    std::replace(entry.begin(), entry.end(), '\\', '/');
    while (!entry.empty() && entry.front() == '/') entry.erase(entry.begin());

    if (!entry.empty() && entry.back() != '/' && entry.find("..") == std::string::npos) {
      bool isMainNro = (entry.size() >= 4 && entry.compare(entry.size() - 4, 4, ".nro") == 0);
      std::string destPath = isMainNro ? nroPendingPath : (std::string("sdmc:/") + entry);
      std::string tmpPath = destPath + ".part";

      ensureParentDirs(destPath);
      std::remove(tmpPath.c_str());

      if (unzOpenCurrentFile(uf) == UNZ_OK) {
        FILE* out = std::fopen(tmpPath.c_str(), "wb");
        if (out) {
          bool writeOk = true;
          int bytesRead = 0;
          while ((bytesRead = unzReadCurrentFile(uf, buf, sizeof(buf))) > 0) {
            if (std::fwrite(buf, 1, static_cast<size_t>(bytesRead), out) != static_cast<size_t>(bytesRead)) {
              writeOk = false;
              break;
            }
          }
          std::fclose(out);

          if (bytesRead < 0) writeOk = false;

          if (writeOk) {
            std::remove(destPath.c_str());
            if (std::rename(tmpPath.c_str(), destPath.c_str()) == 0) {
              if (isMainNro) extractedNro = true;
              nxlog::pushf("[update] extracted %s -> %s", entry.c_str(), destPath.c_str());
            } else {
              std::remove(tmpPath.c_str());
              nxlog::pushf("[update] warning: could not replace %s (%s)", destPath.c_str(), std::strerror(errno));
            }
          } else {
            std::remove(tmpPath.c_str());
            nxlog::pushf("[update] warning: write failed for %s", destPath.c_str());
          }
        }
        unzCloseCurrentFile(uf);
      }
    }

    rc = unzGoToNextFile(uf);
  }

  unzClose(uf);
  if (!extractedNro) {
    outErr = "Update zip did not contain a .nro file";
    return false;
  }
  return true;
#endif
}

class ChangelogActivity : public brls::Activity {
 public:
  ChangelogActivity(std::string title, std::string notes)
      : title(std::move(title)), notes(std::move(notes)) {}

  brls::View* createContentView() override {
    auto* scroll = new brls::ScrollingFrame();
    scroll->setGrow(1.0f);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::NATURAL);

    auto* pad = new brls::Box();
    pad->setAxis(brls::Axis::COLUMN);
    pad->setPadding(30.0f, 60.0f, 50.0f, 60.0f);

    auto* label = new brls::Label();
    label->setSingleLine(false);
    label->setText(notes);
    label->setFontSize(18.0f);
    label->setLineHeight(1.4f);
    pad->addView(label);

    scroll->setContentView(pad);

    auto* frame = new brls::AppletFrame();
    frame->pushContentView(scroll);
    frame->setTitle(tr("update.changelog_prefix") + title);
    return frame;
  }

 private:
  std::string title;
  std::string notes;
};

void showNoteDialog(const std::string& msg) {
  auto* d = new brls::Dialog(msg);
  d->addButton(tr("update.ok"), []() {});
  d->open();
}

std::string humanMB(int64_t bytes) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
  return buf;
}

} // namespace

void init(int argc, char** argv) {
  if (argc > 0 && argv && argv[0] && argv[0][0]) {
    selfNro = argv[0];
  }
  nxlog::pushf("[update] running from %s",
               selfNro.empty() ? "(unknown)" : selfNro.c_str());
}

std::string selfPath() {
  return selfNro;
}

bool hasPending() {
  std::string p = pendingPath();
  if (p.empty()) return false;
  if (FILE* f = std::fopen(p.c_str(), "rb")) {
    std::fclose(f);
    return true;
  }
  return false;
}

void checkAsync(std::function<void(Release)> done) {
  if (selfNro.empty()) {
    Release r;
    r.error = tr("update.unknown_path");
    brls::sync([done, r]() { done(r); });
    return;
  }

  launchWorkerWithStack([done]() {
    Release r;
    std::string resp, err;
    if (!httpGet(UPDATE_API_URL, resp, err)) {
      r.error = err;
      nxlog::pushf("[update] check failed: %s", err.c_str());
      brls::sync([done, r]() { done(r); });
      return;
    }

    try {
      json j = json::parse(resp);
      std::string tag = j.value("tag_name", "");
      if (tag.empty()) {
        r.error = j.value("message", "");
        if (r.error.empty()) r.error = tr("update.no_release");
        nxlog::pushf("[update] no tag_name: %s", r.error.c_str());
        brls::sync([done, r]() { done(r); });
        return;
      }

      r.ok = true;
      r.version = (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
      r.notes = j.value("body", "");
      r.title = j.value("name", "");
      if (r.title.empty()) r.title = tag;

      if (j.contains("assets") && j["assets"].is_array()) {
        std::string nroUrl;
        int64_t nroSize = 0;
        std::string zipUrl;
        int64_t zipSize = 0;
        for (const auto& a : j["assets"]) {
          std::string name = a.value("name", "");
          std::string dlUrl = a.value("browser_download_url", "");
          int64_t sz = a.value("size", int64_t{0});
          if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".zip") == 0) {
            if (zipUrl.empty() || name.find("Complete-SD") != std::string::npos) {
              zipUrl = dlUrl;
              zipSize = sz;
            }
          } else if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".nro") == 0) {
            if (nroUrl.empty()) {
              nroUrl = dlUrl;
              nroSize = sz;
            }
          }
        }
        if (!zipUrl.empty()) {
          r.url = zipUrl;
          r.size = zipSize;
          r.isZip = true;
        } else if (!nroUrl.empty()) {
          r.url = nroUrl;
          r.size = nroSize;
          r.isZip = false;
        }
      }

      r.newer = isNewer(r.version, APP_VERSION);
      nxlog::pushf("[update] latest=%s running=%s newer=%d asset=%s",
                   r.version.c_str(), APP_VERSION, r.newer ? 1 : 0,
                   r.url.empty() ? "(none)" : r.url.c_str());
    } catch (const std::exception& e) {
      r.ok = false;
      r.error = e.what();
      nxlog::pushf("[update] JSON parse error: %s", e.what());
    }

    brls::sync([done, r]() { done(r); });
  });
}

void fetchNotesAsync(
    const std::string& version,
    std::function<void(bool, std::string, std::string, std::string)> done) {
  int v[3];
  parseVersion(version, v);

  launchWorkerWithStack([v0 = v[0], v1 = v[1], top = v[2], done]() {
    std::string doc, title, firstErr;
    bool any = false;

    for (int p = top; p >= 0; p--) {
      char tagbuf[32];
      std::snprintf(tagbuf, sizeof(tagbuf), "v%d.%d.%d", v0, v1, p);
      std::string tag = tagbuf;
      std::string url = std::string(UPDATE_TAG_URL) + tag;

      std::string resp, err;
      if (!httpGet(url, resp, err)) {
        std::string urlNoV = std::string(UPDATE_TAG_URL) + tag.substr(1);
        if (!httpGet(urlNoV, resp, err)) {
          if (firstErr.empty()) firstErr = err;
          continue;
        }
      }

      try {
        json j = json::parse(resp);
        std::string body = j.value("body", "");
        if (body.empty()) continue;
        std::string name = j.value("name", "");
        if (name.empty()) name = tag;

        if (!any) {
          title = name;
          doc = body;
          any = true;
        } else {
          doc += "\n\n\n" + name + "\n\n" + body;
        }
      } catch (...) {}
    }

    if (!any) {
      FILE* f = std::fopen("romfs:/changelog.txt", "rb");
      if (f) {
        std::fseek(f, 0, SEEK_END);
        long sz = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (sz > 0) {
          std::string localDoc(static_cast<size_t>(sz), '\0');
          std::fread(&localDoc[0], 1, static_cast<size_t>(sz), f);
          std::fclose(f);
          brls::sync([done, localDoc]() {
            done(true, "Switube v" APP_VERSION, localDoc, "");
          });
          return;
        }
        std::fclose(f);
      }

      std::string m = firstErr.empty() ? tr("update.no_changelog") : firstErr;
      brls::sync([done, m]() { done(false, "", "", m); });
      return;
    }

    brls::sync([done, title, doc]() { done(true, title, doc, ""); });
  });
}

void showNotes(const std::string& title, const std::string& notes) {
  brls::Application::pushActivity(new ChangelogActivity(title, notes));
}

void downloadAsync(const Release& r,
                   std::function<void(float)> progress,
                   std::function<void(std::string)> done) {
  std::string url = r.url;
  std::string dest = pendingPath();
  bool isZip = r.isZip;
  if (dest.empty() || url.empty()) {
    brls::sync([done]() { done(tr("update.nothing_to_dl")); });
    return;
  }

  launchWorkerWithStack([url, dest, isZip, progress, done]() {
    auto last = std::make_shared<int>(-1);
    std::string err;
    std::string dlTarget = isZip ? "/config/Switube/update.zip" : dest;
    bool ok = httpDownload(url, dlTarget, err, [progress, last](int64_t now, int64_t total) {
      float f = total > 0 ? static_cast<float>(now) / static_cast<float>(total) : -1.0f;
      int pct = total > 0 ? static_cast<int>(f * 100.0f) : -1;
      if (pct != *last) {
        *last = pct;
        brls::sync([progress, f]() { progress(f); });
      }
    });
    if (ok && isZip) {
      ok = extractUpdateZip(dlTarget, dest, err);
      std::remove(dlTarget.c_str());
    }
    std::string e = ok ? "" : err;
    if (!ok) {
      nxlog::pushf("[update] download/extract failed: %s", err.c_str());
    }
    brls::sync([done, e]() { done(e); });
  });
}

void showChangelog(const std::string& version) {
  auto* box = new brls::Box();
  box->setAxis(brls::Axis::COLUMN);
  box->setAlignItems(brls::AlignItems::CENTER);
  box->setJustifyContent(brls::JustifyContent::CENTER);
  box->setPadding(28.0f, 44.0f, 28.0f, 44.0f);
  box->setFocusable(true);
  box->setHideHighlight(true);

  auto* sp = new brls::ProgressSpinner(brls::ProgressSpinnerSize::LARGE);
  sp->setDimensions(52.0f, 52.0f);
  sp->animate(true);
  box->addView(sp);

  auto* label = new brls::Label();
  label->setText(tr("update.loading_changelog"));
  label->setFontSize(19.0f);
  label->setMarginTop(20.0f);
  label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
  box->addView(label);

  auto* loading = new brls::Dialog(box);
  loading->setCancelable(false);
  loading->open();

  fetchNotesAsync(version, [loading](bool ok, std::string title,
                                     std::string notes, std::string err) {
    loading->close([ok, title, notes, err]() {
      if (!ok) {
        showNoteDialog(tr("update.changelog_err") + err);
      } else {
        showNotes(title, notes);
      }
    });
  });
}

void showChangelog(const Release& r) {
  if (!r.notes.empty()) {
    showNotes(r.title, r.notes);
    return;
  }
  showChangelog(r.version);
}

void promptInstall(const Release& r) {
  std::string msg = tr("update.available_msg", r.version);
  if (r.size > 0) msg += "  (" + humanMB(r.size) + ")";
  if (!r.title.empty()) msg += "\n\n" + r.title;

  auto* ask = new brls::Dialog(msg);
  ask->addButton(tr("update.later"), []() {});
  ask->addButton(tr("update.view_changelog"), [r]() {
    promptInstall(r);
    showChangelog(r);
  });
  ask->addButton(tr("update.update_btn"), [r]() {
    auto* box = new brls::Box();
    box->setAxis(brls::Axis::COLUMN);
    box->setAlignItems(brls::AlignItems::CENTER);
    box->setPadding(24.0f, 30.0f, 24.0f, 30.0f);

    auto* label = new brls::Label();
    label->setText(tr("update.downloading_pct", "0"));
    label->setFontSize(20.0f);
    label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
    box->addView(label);

    auto* track = new brls::Box();
    track->setWidth(320.0f);
    track->setHeight(8.0f);
    track->setShrink(0.0f);
    track->setCornerRadius(4.0f);
    track->setMarginTop(18.0f);
    track->setBackgroundColor(nvgRGBA(255, 255, 255, 38));

    auto* fill = new brls::Box();
    fill->setHeight(8.0f);
    fill->setWidth(0.0f);
    fill->setCornerRadius(4.0f);
    fill->setBackgroundColor(swui::accent());
    track->addView(fill);
    box->addView(track);

    auto* prog = new brls::Dialog(box);
    prog->setCancelable(false);
    prog->open();

    downloadAsync(
        r,
        [label, track, fill](float f) {
          if (f < 0.0f) {
            label->setText(tr("update.downloading"));
            return;
          }
          if (f > 1.0f) f = 1.0f;
          int pct = static_cast<int>(f * 100.0f);
          label->setText(tr("update.downloading_pct", std::to_string(pct)));
          float w = track->getWidth();
          if (w > 0.0f) fill->setWidth(w * f);
        },
        [prog](std::string err) {
          prog->close([err]() {
            if (!err.empty()) {
              showNoteDialog(tr("update.failed") + err);
              return;
            }
            auto* d = new brls::Dialog(tr("update.installed_msg"));
            d->setCancelable(false);
            d->addButton(tr("update.restart_btn"), []() {
              if (!restartNow()) {
                showNoteDialog(tr("update.reopen_msg"));
              }
            });
            d->open();
          });
        });
  });

  if (brls::View* up = ask->getView("brls/dialog/button3")) {
    ask->setLastFocusedView(up);
  }
  ask->open();
}

bool restartNow() {
#ifdef __SWITCH__
  if (selfNro.empty() || !envHasNextLoad()) {
    nxlog::push("[update] loader cannot relaunch us");
    return false;
  }
  envSetNextLoad(selfNro.c_str(), selfNro.c_str());
  brls::Application::quit();
  return true;
#else
  return false;
#endif
}

void applyPending() {
  std::string src = pendingPath();
  if (src.empty() || !hasPending()) return;

#ifdef __SWITCH__
  // Desmontar romfs antes de renombrar el .nro en ejecución.
  romfsExit();
#endif

  std::string bak = selfNro + ".old";
  std::remove(bak.c_str());

  bool moved = (std::rename(selfNro.c_str(), bak.c_str()) == 0);
  if (!moved) {
    nxlog::pushf("[update] could not move old .nro aside: %s", std::strerror(errno));
  }

  if (std::rename(src.c_str(), selfNro.c_str()) == 0) {
    std::remove(bak.c_str());
    nxlog::push("[update] installed, active on next launch");
    return;
  }

  nxlog::pushf("[update] could not install %s: %s", src.c_str(), std::strerror(errno));
  if (moved) {
    std::rename(bak.c_str(), selfNro.c_str());
  }
}

} // namespace switube::update
