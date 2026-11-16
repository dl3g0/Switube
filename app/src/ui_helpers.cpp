#include "ui_helpers.hpp"
#include "download_manager.hpp"
#include "i18n.hpp"

#include <algorithm>
#include <cctype>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <pthread.h>
#include <queue>
#include <thread>
#include <unordered_map>

#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#endif

#include <curl/curl.h>

static size_t CurlWriteImageData(void* contents, size_t size, size_t nmemb, void* userp) { size_t total = size * nmemb; std::string* s = static_cast<std::string*>(userp); if (s->size() + total > 1024*1024) return 0; s->append(static_cast<char*>(contents), total); return total; }

static std::string fetchRemoteImageData(const std::string& url, CURL* reusedCurl = nullptr) {
  if (url.rfind("/", 0) == 0 || url.rfind("sdmc:/", 0) == 0) {
    std::ifstream file(url, std::ios::binary | std::ios::ate);
    if (!file.is_open() && url.rfind("/", 0) == 0) {
      file.open("sdmc:" + url, std::ios::binary | std::ios::ate);
    }
    if (!file.good()) return {};
    const std::streamoff len = file.tellg();
    if (len <= 0 || len > 1024 * 1024) return {};
    std::string data(static_cast<size_t>(len), '\0');
    file.seekg(0);
    file.read(data.data(), len);
    return file.good() ? data : std::string{};
  }
  CURL* curl = reusedCurl ? reusedCurl : curl_easy_init();
  if (!curl) return {};
  if (reusedCurl) {
    curl_easy_reset(curl);
  }
  std::string buffer;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Accept: image/jpeg, image/png;q=0.9");
  headers = curl_slist_append(headers, "User-Agent: Switube/0.1.0");
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, CurlWriteImageData);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
  curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);
  curl_easy_setopt(curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
  curl_easy_setopt(curl, CURLOPT_SSLVERSION, CURL_SSLVERSION_TLSv1_2);
  curl_easy_setopt(curl, CURLOPT_MAXCONNECTS, 1L);
  curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
  CURLcode res = curl_easy_perform(curl);
  curl_slist_free_all(headers);
  if (!reusedCurl) {
    curl_easy_cleanup(curl);
  }
  if (res == CURLE_OK && !buffer.empty()) return buffer;
  return {};
}
#include "NXConsoleLog.h"
#include <borealis/core/cache_helper.hpp>

namespace swui {
namespace {
std::mutex imageCacheMutex;
std::unordered_map<std::string, std::string> imageCache;
std::deque<std::string> imageCacheOrder;
std::deque<std::string> persistentImageCacheOrder;
bool persistentImageCacheLoaded = false;
std::mutex imageRequestMutex;
std::unordered_map<brls::Image*, std::string> imageRequests;
constexpr size_t kImageCacheEntries = 48;
constexpr size_t kPersistentImageCacheEntries = 128;
constexpr size_t kMaxEncodedImageBytes = 1024 * 1024;

#ifdef __SWITCH__
constexpr const char* kPersistentImageCacheDirectory =
    "/config/Switube/artwork-cache";
#else
constexpr const char* kPersistentImageCacheDirectory =
    "switube-artwork-cache";
#endif

std::string persistentCacheKey(const std::string& url) {
  uint64_t hash = 14695981039346656037ULL;
  for (const unsigned char byte : url) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  char encoded[17] = {};
  std::snprintf(encoded, sizeof(encoded), "%016llx",
                static_cast<unsigned long long>(hash));
  return encoded;
}

std::string persistentCachePath(const std::string& key) {
  return std::string(kPersistentImageCacheDirectory) + "/" + key + ".img";
}

std::string persistentCacheIndexPath() {
  return std::string(kPersistentImageCacheDirectory) + "/index";
}

void ensurePersistentCacheDirectory() {
#ifdef __SWITCH__
  mkdir("/config", 0755);
  mkdir("/config/Switube", 0755);
  mkdir(kPersistentImageCacheDirectory, 0755);
#elif defined(_WIN32)
  _mkdir(kPersistentImageCacheDirectory);
#else
  mkdir(kPersistentImageCacheDirectory, 0755);
#endif
}

bool isSupportedImageData(const std::string& data) {
  const auto* bytes = reinterpret_cast<const unsigned char*>(data.data());
  const bool jpeg = data.size() >= 3 && bytes[0] == 0xff &&
                    bytes[1] == 0xd8 && bytes[2] == 0xff;
  const bool png = data.size() >= 8 && bytes[0] == 0x89 &&
                   bytes[1] == 0x50 && bytes[2] == 0x4e &&
                   bytes[3] == 0x47 && bytes[4] == 0x0d &&
                   bytes[5] == 0x0a && bytes[6] == 0x1a &&
                   bytes[7] == 0x0a;
  return jpeg || png;
}

void savePersistentCacheIndexLocked() {
  if (!persistentImageCacheLoaded) return;
  ensurePersistentCacheDirectory();
  const std::string path = persistentCacheIndexPath();
  const std::string temporary = path + ".tmp";
  std::ofstream output(temporary, std::ios::trunc);
  if (!output.good()) return;
  for (const auto& key : persistentImageCacheOrder) output << key << '\n';
  output.close();
  if (!output.good()) {
    std::remove(temporary.c_str());
    return;
  }
  std::remove(path.c_str());
  if (std::rename(temporary.c_str(), path.c_str()) != 0)
    std::remove(temporary.c_str());
}

void loadPersistentCacheIndexLocked() {
  if (persistentImageCacheLoaded) return;
  persistentImageCacheLoaded = true;
  ensurePersistentCacheDirectory();
  std::ifstream input(persistentCacheIndexPath());
  std::string key;
  while (std::getline(input, key)) {
    if (!key.empty() && key.back() == '\r') key.pop_back();
    if (key.size() != 16 ||
        key.find_first_not_of("0123456789abcdef") != std::string::npos)
      continue;
    if (std::find(persistentImageCacheOrder.begin(),
                  persistentImageCacheOrder.end(), key) !=
        persistentImageCacheOrder.end())
      continue;
    std::ifstream file(persistentCachePath(key), std::ios::binary);
    if (file.good()) persistentImageCacheOrder.push_back(key);
  }
  while (persistentImageCacheOrder.size() > kPersistentImageCacheEntries) {
    const std::string oldest = persistentImageCacheOrder.front();
    persistentImageCacheOrder.pop_front();
    std::remove(persistentCachePath(oldest).c_str());
  }
}

void touchPersistentCacheKeyLocked(const std::string& key) {
  const auto found = std::find(persistentImageCacheOrder.begin(),
                               persistentImageCacheOrder.end(), key);
  if (found != persistentImageCacheOrder.end())
    persistentImageCacheOrder.erase(found);
  persistentImageCacheOrder.push_back(key);
}

void removePersistentImageLocked(const std::string& url) {
  loadPersistentCacheIndexLocked();
  const std::string key = persistentCacheKey(url);
  const auto found = std::find(persistentImageCacheOrder.begin(),
                               persistentImageCacheOrder.end(), key);
  if (found != persistentImageCacheOrder.end())
    persistentImageCacheOrder.erase(found);
  std::remove(persistentCachePath(key).c_str());
  imageCache.erase(url);
  const auto memoryOrder =
      std::find(imageCacheOrder.begin(), imageCacheOrder.end(), url);
  if (memoryOrder != imageCacheOrder.end()) imageCacheOrder.erase(memoryOrder);
  savePersistentCacheIndexLocked();
}

std::string readPersistentImageLocked(const std::string& url) {
  loadPersistentCacheIndexLocked();
  const std::string key = persistentCacheKey(url);
  const std::string path = persistentCachePath(key);
  std::ifstream input(path, std::ios::binary | std::ios::ate);
  if (!input.good()) return {};
  const std::streamoff length = input.tellg();
  if (length <= 0 ||
      length > static_cast<std::streamoff>(kMaxEncodedImageBytes)) {
    removePersistentImageLocked(url);
    return {};
  }
  std::string data(static_cast<size_t>(length), '\0');
  input.seekg(0);
  input.read(data.data(), length);
  if (!input.good() || !isSupportedImageData(data)) {
    removePersistentImageLocked(url);
    return {};
  }
  touchPersistentCacheKeyLocked(key);
  return data;
}

void writePersistentImageLocked(const std::string& url,
                                const std::string& data) {
  if (data.empty() || data.size() > kMaxEncodedImageBytes ||
      !isSupportedImageData(data))
    return;
  loadPersistentCacheIndexLocked();
  const std::string key = persistentCacheKey(url);
  const std::string path = persistentCachePath(key);
  const std::string temporary = path + ".tmp";
  std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
  if (!output.good()) return;
  output.write(data.data(), static_cast<std::streamsize>(data.size()));
  output.close();
  if (!output.good()) {
    std::remove(temporary.c_str());
    return;
  }
  std::remove(path.c_str());
  if (std::rename(temporary.c_str(), path.c_str()) != 0) {
    std::remove(temporary.c_str());
    return;
  }
  touchPersistentCacheKeyLocked(key);
  while (persistentImageCacheOrder.size() > kPersistentImageCacheEntries) {
    const std::string oldest = persistentImageCacheOrder.front();
    persistentImageCacheOrder.pop_front();
    std::remove(persistentCachePath(oldest).c_str());
  }
  savePersistentCacheIndexLocked();
}

void storeMemoryImageLocked(const std::string& url, const std::string& data) {
  if (data.empty() || imageCache.find(url) != imageCache.end()) return;
  while (imageCacheOrder.size() >= kImageCacheEntries) {
    imageCache.erase(imageCacheOrder.front());
    imageCacheOrder.pop_front();
  }
  imageCacheOrder.push_back(url);
  imageCache.emplace(url, data);
}

class ImageWorkerPool {
 public:
  ImageWorkerPool() {
    for (int index = 0; index < 4; ++index) {
      pthread_t pt{};
      pthread_attr_t attr;
      pthread_attr_init(&attr);
      pthread_attr_setstacksize(&attr, 512 * 1024);
      if (pthread_create(&pt, &attr, &ImageWorkerPool::threadEntry, this) == 0) {
        workers.push_back(pt);
      }
      pthread_attr_destroy(&attr);
    }
  }

  void submit(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (stopping) return;
      tasks.push(std::move(task));
    }
    wake.notify_one();
  }

  void stop() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      if (stopping) return;
      stopping = true;
      std::queue<std::function<void()>> empty;
      tasks.swap(empty);
    }
    wake.notify_all();
    for (auto& worker : workers) {
      pthread_join(worker, nullptr);
    }
    workers.clear();
  }

 private:
  static void* threadEntry(void* arg) {
    static_cast<ImageWorkerPool*>(arg)->run();
    return nullptr;
  }

  void run() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait(lock, [this] { return stopping || !tasks.empty(); });
        if (stopping && tasks.empty()) return;
        task = std::move(tasks.front());
        tasks.pop();
      }
      try {
        if (task) task();
      } catch (const std::exception& e) {
        nxlog::pushf("ImageWorkerPool exception: %s", e.what());
      } catch (...) {
        nxlog::push("ImageWorkerPool unknown exception");
      }
    }
  }

  std::mutex mutex;
  std::condition_variable wake;
  std::queue<std::function<void()>> tasks;
  std::vector<pthread_t> workers;
  bool stopping = false;
};

std::mutex poolMutex;
std::unique_ptr<ImageWorkerPool> pool;

ImageWorkerPool* imageWorkers() {
  std::lock_guard<std::mutex> lock(poolMutex);
  if (!pool) pool = std::make_unique<ImageWorkerPool>();
  return pool.get();
}
}  // namespace

NVGcolor background() { return nvgRGBA(0, 0, 0, 0); }
NVGcolor surface() { return nvgRGB(22, 25, 30); }
NVGcolor surfaceRaised() { return nvgRGB(31, 35, 42); }
NVGcolor textPrimary() { return nvgRGB(246, 247, 249); }
NVGcolor textSecondary() { return nvgRGB(168, 175, 186); }
NVGcolor accent() { return nvgRGB(255, 45, 60); }
NVGcolor danger() { return nvgRGB(235, 60, 60); }

GradientBox::GradientBox() {
  setAxis(brls::Axis::COLUMN);
  setGrow(1.0f);
}

void GradientBox::draw(NVGcontext* vg, float x, float y, float width, float height,
                       brls::Style style, brls::FrameContext* ctx) {
  nvgSave(vg);
  nvgBeginPath(vg);
  nvgRect(vg, x, y, width, height);
  // Degradado radial rojo YouTube a negro obsidiana (idéntico a la arquitectura de DEMO 3 SWITCH)
  NVGpaint bg = nvgRadialGradient(vg, x + width * 0.65f, y + height * 0.25f,
                                  height * 0.16f, width * 0.95f,
                                  nvgRGB(0x3e, 0x0a, 0x12), nvgRGB(0x07, 0x07, 0x0b));
  nvgFillPaint(vg, bg);
  nvgFill(vg);
  nvgRestore(vg);

  brls::Box::draw(vg, x, y, width, height, style, ctx);
}

std::string formatDuration(uint32_t milliseconds) {
  if (milliseconds == 0) return "";
  const uint32_t seconds = milliseconds / 1000;
  return std::to_string(seconds / 60) + ":" +
         (seconds % 60 < 10 ? "0" : "") + std::to_string(seconds % 60);
}

std::string upgradeThumbnailUrl(const std::string& rawUrl, int targetSize) {
  if (rawUrl.empty() || targetSize < 120) return rawUrl;
  if (rawUrl.rfind("/", 0) == 0 || rawUrl.rfind("sdmc:/", 0) == 0) return rawUrl;
  std::string url = rawUrl;
  if (url.find("googleusercontent.com") != std::string::npos ||
      url.find("ggpht.com") != std::string::npos) {
    const std::string targetWh = "=w" + std::to_string(targetSize) + "-h" + std::to_string(targetSize);
    auto whPos = url.find("=w");
    if (whPos != std::string::npos) {
      size_t p = whPos + 2;
      while (p < url.size() && std::isdigit(static_cast<unsigned char>(url[p]))) ++p;
      if (p + 2 <= url.size() && url[p] == '-' && url[p + 1] == 'h') {
        size_t q = p + 2;
        while (q < url.size() && std::isdigit(static_cast<unsigned char>(url[q]))) ++q;
        if (q > p + 2) {
          url.replace(whPos, q - whPos, targetWh);
        }
      }
    }
    auto sPos = url.find("=s");
    if (sPos != std::string::npos) {
      size_t p = sPos + 2;
      while (p < url.size() && std::isdigit(static_cast<unsigned char>(url[p]))) ++p;
      if (p > sPos + 2) {
        url.replace(sPos, p - sPos, "=s" + std::to_string(targetSize));
      }
    }
    if (url.find("=w") == std::string::npos && url.find("=s") == std::string::npos && url.find('?') == std::string::npos) {
      url += targetWh + "-l90-rj";
    }
  } else if (url.find("i.ytimg.com") != std::string::npos) {
    if (auto pos = url.find("/default.jpg"); pos != std::string::npos) {
      url.replace(pos, 12, "/hqdefault.jpg");
    } else if (auto pos2 = url.find("/mqdefault.jpg"); pos2 != std::string::npos) {
      url.replace(pos2, 14, "/hqdefault.jpg");
    }
  }
  return url;
}

void loadRemoteImage(brls::Image* image, const std::string& url,
                     int preferredSize) {
  if (!image || url.empty()) return;
  std::string resolvedUrl = (preferredSize >= 120) ? upgradeThumbnailUrl(url, preferredSize) : url;

  image->setScalingType(brls::ImageScalingType::FIT);
  const int cachedTexture =
      brls::TextureCache::instance().getCache(resolvedUrl);
  if (cachedTexture > 0) {
    // Invalidate an older in-flight request for this view before showing the
    // cache hit, otherwise the older download could overwrite the new art.
    {
      std::lock_guard<std::mutex> lock(imageRequestMutex);
      imageRequests.erase(image);
    }
    image->setFreeTexture(false);
    image->innerSetImage(cachedTexture);
    return;
  }
  {
    std::lock_guard<std::mutex> lock(imageRequestMutex);
    imageRequests[image] = resolvedUrl;
  }
  image->ptrLock();
  imageWorkers()->submit([image, url = std::move(resolvedUrl), rawUrl = url]() mutable {
      std::string data;
      const bool isLocalFile = (url.rfind("/", 0) == 0 || url.rfind("sdmc:/", 0) == 0);
      {
        std::lock_guard<std::mutex> lock(imageCacheMutex);
        const auto cached = imageCache.find(url);
        if (cached != imageCache.end()) data = cached->second;
        if (data.empty() && !isLocalFile) {
          data = readPersistentImageLocked(url);
          if (!data.empty()) storeMemoryImageLocked(url, data);
        }
      }
      if (data.empty()) {
        thread_local struct CurlHolder {
          CURL* handle = curl_easy_init();
          ~CurlHolder() { if (handle) curl_easy_cleanup(handle); }
        } curlHolder;
        data = fetchRemoteImageData(url, curlHolder.handle);
        if (!data.empty() && data.size() <= kMaxEncodedImageBytes && isSupportedImageData(data)) {
          std::lock_guard<std::mutex> lock(imageCacheMutex);
          storeMemoryImageLocked(url, data);
          if (!isLocalFile) {
            writePersistentImageLocked(url, data);
          }
        } else {
          data.clear();
        }
      }
      // Fallback offline: si la URL mejorada (ej. =w544-h544) falló sin internet, usar la versión base en caché
      if (data.empty() && !isLocalFile && rawUrl != url) {
        std::lock_guard<std::mutex> lock(imageCacheMutex);
        const auto cached = imageCache.find(rawUrl);
        if (cached != imageCache.end()) data = cached->second;
        if (data.empty()) {
          data = readPersistentImageLocked(rawUrl);
          if (!data.empty()) storeMemoryImageLocked(rawUrl, data);
        }
      }
      brls::sync([image, url, data = std::move(data)] {
        bool currentRequest = false;
        {
          std::lock_guard<std::mutex> lock(imageRequestMutex);
          const auto request = imageRequests.find(image);
          currentRequest = request != imageRequests.end() && request->second == url;
          if (currentRequest) imageRequests.erase(request);
        }
        if (currentRequest && !data.empty() && image->hasParent()) {
          int texture = brls::TextureCache::instance().getCache(url);
          if (texture == 0) {
            texture = nvgCreateImageMem(
                brls::Application::getNVGContext(), 0,
                reinterpret_cast<unsigned char*>(
                    const_cast<char*>(data.data())),
                static_cast<int>(data.size()));
            if (texture > 0)
              brls::TextureCache::instance().addCache(url, texture);
            else
              nxlog::pushf("Artwork decode failed (%d bytes): %s",
                           static_cast<int>(data.size()), url.c_str());
          }
          if (texture > 0) {
            image->setFreeTexture(false);
            image->innerSetImage(texture);
          } else {
            std::lock_guard<std::mutex> lock(imageCacheMutex);
            removePersistentImageLocked(url);
          }
        }
        image->ptrUnlock();
      });
  });
}

void shutdownRemoteImages() {
  std::unique_ptr<ImageWorkerPool> active;
  {
    std::lock_guard<std::mutex> lock(poolMutex);
    active = std::move(pool);
  }
  if (active) active->stop();
  {
    std::lock_guard<std::mutex> lock(imageCacheMutex);
    savePersistentCacheIndexLocked();
  }
}

brls::Label* makeLabel(const std::string& text, float size, NVGcolor color,
                       bool singleLine) {
  auto* label = new brls::Label();
  label->setText(text);
  label->setFontSize(size);
  label->setTextColor(color);
  label->setSingleLine(singleLine);
  return label;
}

brls::Box* makeActionButton(const std::string& text,
                            std::function<bool(brls::View*)> action,
                            bool primary, brls::Label** labelOut) {
  auto* button = new brls::Box();
  button->setFocusable(true);
  button->setAxis(brls::Axis::ROW);
  button->setAlignItems(brls::AlignItems::CENTER);
  button->setJustifyContent(brls::JustifyContent::CENTER);
  button->setPadding(10, 18, 10, 18);
  button->setCornerRadius(9);
  button->setHighlightCornerRadius(9);
  button->setBackgroundColor(primary ? accent() : surfaceRaised());
  button->addGestureRecognizer(new brls::TapGestureRecognizer(button));
  auto* label = makeLabel(text, 18, textPrimary(), true);
  button->addView(label);
  if (primary) {
    // Borealis draws a dark focus fill over custom primary buttons.
    button->getFocusEvent()->subscribe(
        [label](brls::View*) { label->setTextColor(textPrimary()); });
    button->getFocusLostEvent()->subscribe([label](brls::View*) {
      label->setTextColor(textPrimary());
    });
  }
  button->registerClickAction(std::move(action));
  if (labelOut) *labelOut = label;
  return button;
}

FilterChip::FilterChip(const std::string& text, std::function<void()> action) {
  setFocusable(true);
  setAxis(brls::Axis::ROW);
  setAlignItems(brls::AlignItems::CENTER);
  setJustifyContent(brls::JustifyContent::CENTER);
  setHeight(44);
  setPadding(8, 20, 8, 20);
  setCornerRadius(22);
  setHighlightCornerRadius(22);
  addGestureRecognizer(new brls::TapGestureRecognizer(this));
  label = makeLabel(text, 17, textPrimary(), true);
  addView(label);
  registerClickAction([action = std::move(action)](brls::View*) {
    action();
    return true;
  });
  getFocusEvent()->subscribe(
      [this](brls::View*) { applyStyle(true); });
  getFocusLostEvent()->subscribe(
      [this](brls::View*) { applyStyle(false); });
  applyStyle(false);
}

void FilterChip::setSelected(bool nextSelected) {
  selected = nextSelected;
  applyStyle(isFocused());
}

void FilterChip::setText(const std::string& nextText) {
  if (label) label->setText(nextText);
}

void FilterChip::applyStyle(bool focused) {
  setBackgroundColor(selected ? accent() : surfaceRaised());
  setBorderThickness(selected ? 0.0f : 1.0f);
  setBorderColor(nvgRGBA(255, 255, 255, focused ? 80 : 22));
  label->setTextColor(textPrimary());
}

RepeatIcon::RepeatIcon() {
  setDimensions(34, 34);
  setFocusable(false);
}

void RepeatIcon::setMode(RepeatMode nextMode) {
  if (mode == nextMode) return;
  mode = nextMode;
}

void RepeatIcon::draw(NVGcontext* vg, float x, float y, float width,
                      float height, brls::Style, brls::FrameContext*) {
  const NVGcolor color = mode == RepeatMode::Off
                             ? textPrimary()
                             : accent();
  const float left = x + 4.0f;
  const float right = x + width - 4.0f;
  const float top = y + 10.0f;
  const float bottom = y + height - 10.0f;

  nvgSave(vg);
  nvgStrokeColor(vg, color);
  nvgStrokeWidth(vg, 2.6f);
  nvgLineCap(vg, NVG_ROUND);
  nvgLineJoin(vg, NVG_ROUND);

  nvgBeginPath(vg);
  nvgMoveTo(vg, left, top);
  nvgLineTo(vg, right - 2.0f, top);
  nvgMoveTo(vg, right - 8.0f, top - 5.0f);
  nvgLineTo(vg, right - 2.0f, top);
  nvgLineTo(vg, right - 8.0f, top + 5.0f);
  nvgStroke(vg);

  nvgBeginPath(vg);
  nvgMoveTo(vg, right, bottom);
  nvgLineTo(vg, left + 2.0f, bottom);
  nvgMoveTo(vg, left + 8.0f, bottom - 5.0f);
  nvgLineTo(vg, left + 2.0f, bottom);
  nvgLineTo(vg, left + 8.0f, bottom + 5.0f);
  nvgStroke(vg);

  if (mode == RepeatMode::One) {
    nvgFontFaceId(vg, brls::Application::getFont(brls::FONT_REGULAR));
    nvgFontSize(vg, 13.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, color);
    nvgText(vg, x + width / 2.0f, y + height / 2.0f + 0.5f, "1", nullptr);
  }
  nvgRestore(vg);
}

brls::Box* makeRepeatButton(std::function<bool(brls::View*)> action,
                            RepeatIcon** iconOut) {
  auto* button = makeActionButton("", std::move(action));
  button->clearViews();
  auto* icon = new RepeatIcon();
  button->addView(icon);
  if (iconOut) *iconOut = icon;
  return button;
}

ShuffleIcon::ShuffleIcon() {
  setDimensions(34, 34);
  setFocusable(false);
}

void ShuffleIcon::setActive(bool nextActive) { active = nextActive; }

void ShuffleIcon::draw(NVGcontext* vg, float x, float y, float width,
                       float height, brls::Style, brls::FrameContext*) {
  const NVGcolor color = active ? accent() : textPrimary();
  const float left = x + 4.0f;
  const float right = x + width - 4.0f;
  const float top = y + 9.0f;
  const float bottom = y + height - 9.0f;
  const float center = x + width / 2.0f;

  nvgSave(vg);
  nvgStrokeColor(vg, color);
  nvgStrokeWidth(vg, 2.6f);
  nvgLineCap(vg, NVG_ROUND);
  nvgLineJoin(vg, NVG_ROUND);

  // Upper input crosses down to the lower output.
  nvgBeginPath(vg);
  nvgMoveTo(vg, left, top);
  nvgBezierTo(vg, center - 5.0f, top, center + 3.0f, bottom,
              right - 2.0f, bottom);
  nvgMoveTo(vg, right - 8.0f, bottom - 5.0f);
  nvgLineTo(vg, right - 2.0f, bottom);
  nvgLineTo(vg, right - 8.0f, bottom + 5.0f);
  nvgStroke(vg);

  // Lower input crosses up to the upper output.
  nvgBeginPath(vg);
  nvgMoveTo(vg, left, bottom);
  nvgBezierTo(vg, center - 5.0f, bottom, center + 3.0f, top,
              right - 2.0f, top);
  nvgMoveTo(vg, right - 8.0f, top - 5.0f);
  nvgLineTo(vg, right - 2.0f, top);
  nvgLineTo(vg, right - 8.0f, top + 5.0f);
  nvgStroke(vg);
  nvgRestore(vg);
}

brls::Box* makeShuffleButton(std::function<bool(brls::View*)> action,
                             ShuffleIcon** iconOut) {
  auto* button = makeActionButton("", std::move(action));
  button->clearViews();
  auto* icon = new ShuffleIcon();
  button->addView(icon);
  if (iconOut) *iconOut = icon;
  return button;
}

PlayPauseIcon::PlayPauseIcon(float size) : iconSize(size) {
  setDimensions(size, size);
  setFocusable(false);
}

void PlayPauseIcon::setPlaying(bool playing) {
  isPlaying = playing;
}

void PlayPauseIcon::draw(NVGcontext* vg, float x, float y, float width, float height,
                         brls::Style, brls::FrameContext*) {
  float cx = x + width * 0.5f;
  float cy = y + height * 0.5f;

  nvgSave(vg);
  nvgFillColor(vg, nvgRGB(255, 255, 255));

  if (isPlaying) {
    float bh = height * 0.62f;
    float bw = height * 0.20f;
    float gap = height * 0.18f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, cx - gap * 0.5f - bw, cy - bh * 0.5f, bw, bh, 2.0f);
    nvgRoundedRect(vg, cx + gap * 0.5f, cy - bh * 0.5f, bw, bh, 2.0f);
    nvgFill(vg);
  } else {
    float r = height * 0.36f;
    float nudgeX = r * 0.16f;
    nvgBeginPath(vg);
    nvgMoveTo(vg, cx - r * 0.70f + nudgeX, cy - r);
    nvgLineTo(vg, cx + r * 1.05f + nudgeX, cy);
    nvgLineTo(vg, cx - r * 0.70f + nudgeX, cy + r);
    nvgClosePath(vg);
    nvgFill(vg);
  }
  nvgRestore(vg);
}

PrevTrackIcon::PrevTrackIcon(float size) : iconSize(size) {
  setDimensions(size, size);
  setFocusable(false);
}

void PrevTrackIcon::draw(NVGcontext* vg, float x, float y, float width, float height,
                         brls::Style, brls::FrameContext*) {
  float cx = x + width * 0.5f;
  float cy = y + height * 0.5f;

  nvgSave(vg);
  nvgFillColor(vg, nvgRGB(255, 255, 255));

  float r = height * 0.32f;
  float barW = 3.2f;
  float barH = r * 2.0f;
  float gap = 3.0f;

  nvgBeginPath(vg);
  nvgRoundedRect(vg, cx - r * 0.85f - barW, cy - barH * 0.5f, barW, barH, 1.5f);
  nvgMoveTo(vg, cx + r * 0.85f, cy - r);
  nvgLineTo(vg, cx - r * 0.85f + gap, cy);
  nvgLineTo(vg, cx + r * 0.85f, cy + r);
  nvgClosePath(vg);
  nvgFill(vg);

  nvgRestore(vg);
}

NextTrackIcon::NextTrackIcon(float size) : iconSize(size) {
  setDimensions(size, size);
  setFocusable(false);
}

void NextTrackIcon::draw(NVGcontext* vg, float x, float y, float width, float height,
                         brls::Style, brls::FrameContext*) {
  float cx = x + width * 0.5f;
  float cy = y + height * 0.5f;

  nvgSave(vg);
  nvgFillColor(vg, nvgRGB(255, 255, 255));

  float r = height * 0.32f;
  float barW = 3.2f;
  float barH = r * 2.0f;
  float gap = 3.0f;

  nvgBeginPath(vg);
  nvgMoveTo(vg, cx - r * 0.85f, cy - r);
  nvgLineTo(vg, cx + r * 0.85f - gap, cy);
  nvgLineTo(vg, cx - r * 0.85f, cy + r);
  nvgClosePath(vg);
  nvgRoundedRect(vg, cx + r * 0.85f, cy - barH * 0.5f, barW, barH, 1.5f);
  nvgFill(vg);

  nvgRestore(vg);
}

namespace {
brls::Box* makeIconActionButton(brls::View* icon,
                                std::function<bool(brls::View*)> action,
                                bool primary) {
  auto* button = new brls::Box();
  button->setFocusable(true);
  button->setAxis(brls::Axis::ROW);
  button->setAlignItems(brls::AlignItems::CENTER);
  button->setJustifyContent(brls::JustifyContent::CENTER);
  button->setPadding(10, 18, 10, 18);
  button->setCornerRadius(9);
  button->setHighlightCornerRadius(9);
  button->setBackgroundColor(primary ? accent() : surfaceRaised());
  button->addGestureRecognizer(new brls::TapGestureRecognizer(button));
  button->registerClickAction(std::move(action));
  button->addView(icon);
  return button;
}
}  // namespace

brls::Box* makePlayPauseButton(std::function<bool(brls::View*)> action,
                               PlayPauseIcon** iconOut, bool primary) {
  auto* icon = new PlayPauseIcon(primary ? 26.0f : 20.0f);
  if (iconOut) *iconOut = icon;
  return makeIconActionButton(icon, std::move(action), primary);
}

brls::Box* makePrevButton(std::function<bool(brls::View*)> action,
                          PrevTrackIcon** iconOut) {
  auto* icon = new PrevTrackIcon(20.0f);
  if (iconOut) *iconOut = icon;
  return makeIconActionButton(icon, std::move(action), false);
}

brls::Box* makeNextButton(std::function<bool(brls::View*)> action,
                          NextTrackIcon** iconOut) {
  auto* icon = new NextTrackIcon(20.0f);
  if (iconOut) *iconOut = icon;
  return makeIconActionButton(icon, std::move(action), false);
}

brls::Box* makeStatePanel(const std::string& title,
                          const std::string& detail, NVGcolor tint,
                          brls::Label** titleOut,
                          brls::Label** detailOut) {
  auto* panel = new brls::Box();
  panel->setAxis(brls::Axis::COLUMN);
  panel->setPadding(18, 20, 18, 20);
  panel->setCornerRadius(14);
  panel->setBorderThickness(1.0f);
  panel->setBorderColor(nvgRGBA(255, 255, 255, 20));
  panel->setBackgroundColor(tint);
  auto* heading = makeLabel(title, 20, textPrimary(), false);
  auto* body = makeLabel(detail, 16, textSecondary(), false);
  body->setMarginTop(6);
  panel->addView(heading);
  panel->addView(body);
  if (titleOut) *titleOut = heading;
  if (detailOut) *detailOut = body;
  return panel;
}

MediaCard::MediaCard() {
  setAxis(brls::Axis::COLUMN);
  setFocusable(true);
  setDimensions(180, 244);
  setCornerRadius(12);
  setHighlightCornerRadius(12);
  setBorderThickness(1.0f);
  setBorderColor(nvgRGBA(255, 255, 255, 16));
  setBackgroundColor(surface());
  setPadding(8, 8, 10, 8);
  addGestureRecognizer(new brls::TapGestureRecognizer(this));

  artwork = new brls::Image();
  artwork->setDimensions(164, 164);
  artwork->setBackgroundColor(surfaceRaised());
  artwork->setCornerRadius(8);
  artwork->setScalingType(brls::ImageScalingType::FIT);
  addView(artwork);

  titleLabel = makeLabel("", 15, textPrimary(), true);
  titleLabel->setMarginTop(8);
  titleLabel->setMarginLeft(4);
  titleLabel->setMarginRight(4);
  titleLabel->setSingleLine(true);
  addView(titleLabel);

  subtitleLabel = makeLabel("", 13, textSecondary(), true);
  subtitleLabel->setMarginTop(2);
  subtitleLabel->setMarginLeft(4);
  subtitleLabel->setMarginRight(4);
  subtitleLabel->setSingleLine(true);
  addView(subtitleLabel);

  registerClickAction([this](brls::View*) {
    if (onSelected) onSelected();
    return true;
  });
}

void MediaCard::setContent(const std::string& title,
                           const std::string& subtitle,
                           const std::string& imageUrl) {
  titleLabel->setText(title);
  subtitleLabel->setText(subtitle);
  if (!imageUrl.empty()) loadRemoteImage(artwork, imageUrl, 360);
}

void MediaCard::setAction(std::function<void()> action) {
  onSelected = std::move(action);
}

DownloadBadge::DownloadBadge(float size) : size_(size) {
  setDimensions(size, size);
  setFocusable(false);
}

void DownloadBadge::setState(DownloadVisualState state) {
  state_ = state;
}

void DownloadBadge::draw(NVGcontext* vg, float x, float y, float width, float height,
                         brls::Style, brls::FrameContext*) {
  float cx = x + width * 0.5f;
  float cy = y + height * 0.5f;
  float r = width * 0.46f;

  nvgSave(vg);

  nvgBeginPath(vg);
  nvgCircle(vg, cx, cy, r);
  if (state_ == DownloadVisualState::Downloaded) {
    nvgFillColor(vg, nvgRGBA(46, 160, 67, 240));
  } else if (state_ == DownloadVisualState::Downloading) {
    nvgFillColor(vg, nvgRGBA(230, 140, 20, 240));
  } else {
    nvgFillColor(vg, nvgRGBA(80, 80, 85, 200));
  }
  nvgFill(vg);

  if (state_ == DownloadVisualState::Downloaded) {
    nvgStrokeWidth(vg, 2.5f);
    nvgStrokeColor(vg, nvgRGB(255, 255, 255));
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
    nvgBeginPath(vg);
    nvgMoveTo(vg, cx - 6.0f, cy + 0.5f);
    nvgLineTo(vg, cx - 1.5f, cy + 5.0f);
    nvgLineTo(vg, cx + 6.5f, cy - 4.0f);
    nvgStroke(vg);
  } else if (state_ == DownloadVisualState::Downloading) {
    nvgFillColor(vg, nvgRGB(255, 255, 255));
    nvgBeginPath(vg);
    nvgCircle(vg, cx - 5.5f, cy, 2.0f);
    nvgCircle(vg, cx, cy, 2.0f);
    nvgCircle(vg, cx + 5.5f, cy, 2.0f);
    nvgFill(vg);
  } else {
    nvgStrokeWidth(vg, 2.2f);
    nvgStrokeColor(vg, nvgRGB(255, 255, 255));
    nvgLineCap(vg, NVG_ROUND);
    nvgLineJoin(vg, NVG_ROUND);
    nvgBeginPath(vg);
    nvgMoveTo(vg, cx, cy - 6.0f);
    nvgLineTo(vg, cx, cy + 2.5f);
    nvgMoveTo(vg, cx - 4.0f, cy - 1.5f);
    nvgLineTo(vg, cx, cy + 3.0f);
    nvgLineTo(vg, cx + 4.0f, cy - 1.5f);
    nvgStroke(vg);

    nvgBeginPath(vg);
    nvgMoveTo(vg, cx - 5.5f, cy + 6.5f);
    nvgLineTo(vg, cx + 5.5f, cy + 6.5f);
    nvgStroke(vg);
  }

  nvgRestore(vg);
}

TrackRow::TrackRow()
    : alive_(std::make_shared<std::atomic_bool>(true)) {
  setAxis(brls::Axis::ROW);
  setFocusable(true);
  setAlignItems(brls::AlignItems::CENTER);
  setHeight(72);
  setPadding(7, 12, 7, 12);
  setCornerRadius(8);
  setHighlightCornerRadius(8);
  addGestureRecognizer(new brls::TapGestureRecognizer(this));

  indexLabel = makeLabel("", 15, textSecondary(), true);
  indexLabel->setWidth(30);
  indexLabel->setHorizontalAlign(brls::HorizontalAlign::CENTER);
  addView(indexLabel);

  artwork = new brls::Image();
  artwork->setDimensions(56, 56);
  artwork->setBackgroundColor(surfaceRaised());
  artwork->setCornerRadius(5);
  artwork->setScalingType(brls::ImageScalingType::FIT);
  addView(artwork);

  auto* text = new brls::Box();
  text->setAxis(brls::Axis::COLUMN);
  text->setGrow(1.0f);
  text->setMarginLeft(14);
  titleLabel = makeLabel("", 18, textPrimary(), true);
  subtitleLabel = makeLabel("", 14, textSecondary(), true);
  subtitleLabel->setMarginTop(3);
  text->addView(titleLabel);
  text->addView(subtitleLabel);
  addView(text);

  durationLabel = makeLabel("", 15, textSecondary(), true);
  durationLabel->setMarginLeft(14);
  addView(durationLabel);

  downloadBadge = new DownloadBadge(32.0f);
  downloadBadge->setMarginLeft(14);
  addView(downloadBadge);

  downloadBadge->addGestureRecognizer(new brls::TapGestureRecognizer(downloadBadge, [this]() {
    handleDownloadToggle();
  }));

  // [A] Reproducir / Abrir en el footer de Borealis
  registerAction("Reproducir", brls::BUTTON_A, [this](brls::View*) {
    if (onSelected) onSelected();
    return true;
  });

  auto token = alive_;
  downloadObsId_ = switube::DownloadManager::getInstance().addObserver([this, token]() {
    if (token->load()) {
      updateDownloadStatus();
    }
  });

  registerClickAction([this](brls::View*) {
    if (onSelected) onSelected();
    return true;
  });
}

TrackRow::~TrackRow() {
  alive_->store(false);
  if (downloadObsId_ != 0) {
    switube::DownloadManager::getInstance().removeObserver(downloadObsId_);
    downloadObsId_ = 0;
  }
}

void TrackRow::handleDownloadToggle() {
  if (!isDownloadable()) return;
  auto& dm = switube::DownloadManager::getInstance();
  if (dm.isDownloaded(track_.id)) {
    dm.deleteDownload(track_.id);
  } else if (!dm.isDownloading(track_.id)) {
    dm.startDownload(track_);
  }
  updateDownloadStatus();
}

void TrackRow::setTrack(const switube::YtTrack& track, size_t index,
                        std::function<void()> action) {
  track_ = track;
  indexLabel->setText(std::to_string(index + 1));
  titleLabel->setText(track.title);
  std::string detail = track.artist;
  if (!track.album.empty()) detail += "  |  " + track.album;
  
  subtitleLabel->setText(detail);
  const bool canDownload = isDownloadable();
  durationLabel->setText(canDownload ? formatDuration(track.durationMs) : "");
  durationLabel->setVisibility(canDownload ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
  if (artworkEnabled && !track.imageUrl.empty())
    loadRemoteImage(artwork, track.imageUrl, 240);
  onSelected = std::move(action);

  if (canDownload) {
    if (downloadBadge) {
      downloadBadge->setVisibility(brls::Visibility::VISIBLE);
    }
    updateActionHint(brls::BUTTON_A, "Reproducir");
    if (downloadActionId_ == ACTION_NONE) {
      downloadActionId_ = registerAction("Descargar", brls::BUTTON_X, [this](brls::View*) {
        handleDownloadToggle();
        return true;
      });
    }
    updateDownloadStatus();
  } else {
    if (downloadBadge) {
      downloadBadge->setVisibility(brls::Visibility::GONE);
    }
    updateActionHint(brls::BUTTON_A, "Abrir");
    if (downloadActionId_ != ACTION_NONE) {
      unregisterAction(downloadActionId_);
      downloadActionId_ = ACTION_NONE;
    }
    if (this->isFocused()) {
      auto token = alive_;
      brls::sync([this, token]() {
        if (token->load() && this->isFocused()) {
          brls::Application::getGlobalHintsUpdateEvent()->fire();
        }
      });
    }
  }
}

void TrackRow::updateDownloadStatus() {
  if (!isDownloadable()) return;
  auto& dm = switube::DownloadManager::getInstance();
  bool downloaded = dm.isDownloaded(track_.id);
  bool downloading = dm.isDownloading(track_.id);

  if (downloadBadge) {
    if (downloaded) {
      downloadBadge->setState(DownloadVisualState::Downloaded);
    } else if (downloading) {
      downloadBadge->setState(DownloadVisualState::Downloading);
    } else {
      downloadBadge->setState(DownloadVisualState::NotDownloaded);
    }
  }

  // Actualiza el texto de accion en el menu inferior de Borealis dinamicamente
  std::string hint = downloaded ? "Eliminar descarga" : (downloading ? "Descargando..." : "Descargar");
  updateActionHint(brls::BUTTON_X, hint);
  if (this->isFocused()) {
    auto token = alive_;
    brls::sync([this, token]() {
      if (token->load() && this->isFocused()) {
        brls::Application::getGlobalHintsUpdateEvent()->fire();
      }
    });
  }
}

void TrackRow::setCompact(bool compact) {
  artworkEnabled = !compact;
  artwork->setVisibility(compact ? brls::Visibility::GONE
                                 : brls::Visibility::VISIBLE);
  setHeight(compact ? 58 : 72);
}

void TrackRow::setCurrent(bool current) {
  setBackgroundColor(current ? nvgRGB(50, 20, 24) : nvgRGBA(0, 0, 0, 0));
  indexLabel->setTextColor(current ? accent() : textSecondary());
  titleLabel->setTextColor(current ? accent() : textPrimary());
}

HorizontalSection::HorizontalSection(const std::string& title,
                                     const std::string& subtitle) {
  setAxis(brls::Axis::COLUMN);
  setMarginBottom(18);
  auto* header = new brls::Header();
  header->setTitle(title);
  if (!subtitle.empty()) header->setSubtitle(subtitle);
  header->setHeight(38);
  addView(header);

  scroller = new brls::HScrollingFrame();
  scroller->setHeight(256);
  scroller->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
  scroller->setScrollingIndicatorVisible(false);
  row = new brls::Box();
  row->setAxis(brls::Axis::ROW);
  row->setPadding(4, 4, 4, 4);
  scroller->setContentView(row);
  addView(scroller);
}

void HorizontalSection::addCard(MediaCard* card) {
  card->setMarginRight(14);
  row->addView(card);
}

void HorizontalSection::showEmpty(const std::string& text) {
  auto* panel = makeStatePanel(switube::tr("common.nothing_here"), text);
  panel->setWidth(430);
  panel->setMarginRight(14);
  row->addView(panel);
}

void safeClearViews(brls::Box* container, brls::View* fallbackFocus) {
  if (!container) return;
  if (container->isFocused() || container->isChildFocused()) {
    brls::Application::giveFocus(fallbackFocus);
  }
  container->clearViews();
}

void safeRemoveView(brls::Box* container, brls::View* child, brls::View* fallbackFocus) {
  if (!container || !child) return;
  auto* box = dynamic_cast<brls::Box*>(child);
  if (child->isFocused() || (box && box->isChildFocused())) {
    brls::Application::giveFocus(fallbackFocus);
  }
  container->removeView(child);
}

brls::View* nearestFocusableChild(brls::Box* container, size_t preferredIdx) {
  if (!container) return nullptr;
  const auto& children = container->getChildren();
  if (children.empty()) return nullptr;
  size_t idx = std::min(preferredIdx, children.size() - 1);
  for (size_t i = idx; i < children.size(); ++i) {
    if (children[i] && children[i]->isFocusable()) return children[i];
  }
  for (size_t i = idx; i > 0; --i) {
    if (children[i - 1] && children[i - 1]->isFocusable()) return children[i - 1];
  }
  return nullptr;
}

}  // namespace swui
