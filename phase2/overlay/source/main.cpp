// SPDX-License-Identifier: GPL-2.0-only

#define TESLA_INIT_IMPL

#include "artwork_loader.hpp"
#include "Switube/ipc_client.h"

#include <tesla.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace {

#ifdef Switube_LIGHTWEIGHT_OVERLAY
constexpr const char* OverlayVersion = "v0.1.0-lite1";
#else
constexpr const char* OverlayVersion = "v0.1.0";
#endif

constexpr uint64_t SwitubeProgramId = UINT64_C(0x4200000000535755);
constexpr const char* ReadyPath =
    "sdmc:/config/Switube/background-ready.log";
constexpr const char* ArtworkUrlPath =
    "sdmc:/config/Switube/current-artwork-url";
constexpr auto RefreshInterval = std::chrono::milliseconds(500);
constexpr auto BrowseRefreshInterval = std::chrono::milliseconds(200);
constexpr auto InputClaimInterval = std::chrono::milliseconds(200);
constexpr unsigned AutomaticLaunchAttempts = 4;
constexpr auto StartupReadinessTimeout = std::chrono::seconds(6);
constexpr auto StartupRelaunchDelay = std::chrono::milliseconds(750);

enum class ConnectionState {
  Dormant,
  Starting,
  Connected,
  Incompatible,
  Failed,
};

ConnectionState connectionState = ConnectionState::Dormant;
SwitubeIpcStatus currentStatus{};
uint64_t capabilities = 0;
uint64_t runningPid = 0;
uint64_t connectedPid = 0;
std::string serviceText = "Dormant";
std::string feedbackText = "Starts automatically with Switube";
std::string readyText = "Not started";
bool automaticLaunchActive = false;
unsigned automaticLaunchAttempt = 0;
std::chrono::steady_clock::time_point nextAutomaticLaunch{};
uint64_t automaticObservedPid = 0;
std::chrono::steady_clock::time_point automaticObservedAt{};
std::chrono::steady_clock::time_point nextInputClaim{};
std::chrono::steady_clock::time_point nextServiceSupervision{};

void claimExclusiveInput(bool force = false) {
  const auto now = std::chrono::steady_clock::now();
  if (!force && now < nextInputClaim) return;
  tsl::hlp::requestForeground(true, false);
  nextInputClaim = now + InputClaimInterval;
}

std::string readLastLine(const char* path, const char* unavailable) {
  FILE* file = std::fopen(path, "r");
  if (!file) return unavailable;

  char line[96] = {};
  char last[96] = {};
  while (std::fgets(line, sizeof(line), file)) {
    std::snprintf(last, sizeof(last), "%s", line);
  }
  std::fclose(file);

  std::string value = last[0] ? last : "Empty";
  while (!value.empty() &&
         (value.back() == '\n' || value.back() == '\r'))
    value.pop_back();
  return value;
}

std::string readTextFile(const char* path, size_t maximum = 1024) {
  FILE* file = std::fopen(path, "rb");
  if (!file) return {};
  std::string value;
  value.resize(maximum);
  const size_t length = std::fread(value.data(), 1, value.size(), file);
  std::fclose(file);
  value.resize(length);
  while (!value.empty() &&
         (value.back() == '\n' || value.back() == '\r' ||
          value.back() == '\0'))
    value.pop_back();
  return value;
}

std::string resultText(Result result) {
  if (result == KERNELRESULT(OutOfResource))
    return "System resources exhausted";
  char text[40];
  std::snprintf(text, sizeof(text), "0x%08X (M%u D%u)", result,
                R_MODULE(result), R_DESCRIPTION(result));
  return text;
}

std::string formatTime(uint32_t milliseconds) {
  const uint32_t seconds = milliseconds / 1000;
  char text[24];
  std::snprintf(text, sizeof(text), "%u:%02u", seconds / 60, seconds % 60);
  return text;
}

std::string boundedText(const char* value, size_t capacity,
                        const char* fallback = "-") {
  const size_t length = strnlen(value, capacity);
  return length == 0 ? fallback : std::string(value, length);
}

bool processIsRunning(uint64_t* pidOut = nullptr) {
  uint64_t pid = 0;
  const Result result = pmdmntGetProcessId(&pid, SwitubeProgramId);
  if (R_FAILED(result) || pid == 0) return false;
  if (pidOut) *pidOut = pid;
  return true;
}

bool internalIpcIsReady(uint64_t pid) {
  unsigned long long markerPid = 0;
  unsigned result = UINT32_MAX;
  const int parsed = std::sscanf(readyText.c_str(), "pass-v043 %llX 0x%X",
                                 &markerPid, &result);
  return parsed == 2 && markerPid == pid && result == 0;
}

void disconnectIpc() {
  switubeIpcExit();
  connectedPid = 0;
  capabilities = 0;
  std::memset(&currentStatus, 0, sizeof(currentStatus));
}

std::string engineStateText(const SwitubeIpcStatus& status) {
  switch (status.service_state) {
    case SwitubeServiceState_Booting:
      return "Iniciando servicio...";
    case SwitubeServiceState_Connecting:
      return "Conectando a YouTube Music...";
    case SwitubeServiceState_Ready:
      return (status.flags & SwitubeStatusFlag_Connected) ? "En línea"
                                                         : "Listo";
    case SwitubeServiceState_Error:
      return "Error del servicio";
    case SwitubeServiceState_Stopping:
      return "Deteniendo...";
    default:
      return "Listo";
  }
}

std::string queueText(const SwitubeIpcStatus& status) {
  if (status.queue_count == 0 ||
      status.queue_index == SWITUBE_IPC_INVALID_INDEX)
    return "-";
  return std::to_string(status.queue_index + 1) + " de " +
         std::to_string(status.queue_count);
}

bool controlsAvailable() {
  return connectionState == ConnectionState::Connected &&
         currentStatus.service_state == SwitubeServiceState_Ready &&
         (capabilities & SwitubeIpcCapability_PlaybackControl) != 0;
}

void refreshServiceState(bool updateFeedback = true) {
  uint64_t pid = 0;
  if (!processIsRunning(&pid)) {
    disconnectIpc();
    runningPid = 0;
    connectionState = ConnectionState::Dormant;
    serviceText = "Inactivo";
    readyText = "No iniciado";
    if (updateFeedback) feedbackText = "El servicio en segundo plano no se está ejecutando";
    return;
  }

  if (connectedPid != 0 && connectedPid != pid) disconnectIpc();
  runningPid = pid;
  readyText = readLastLine(ReadyPath, "Esperando verificación...");
  if (!internalIpcIsReady(pid)) {
    disconnectIpc();
    connectionState = ConnectionState::Starting;
    serviceText = "Iniciando";
    if (updateFeedback) feedbackText = readyText;
    return;
  }

  // Atmosphere can defer GetService while a service is absent. The readiness
  // marker contains this process's PID and is written only after an internal
  // Ping succeeds, so this call cannot freeze the Ultrahand host.
  if (!switubeIpcIsActive()) {
    const Result connectResult = switubeIpcInitialize();
    if (R_FAILED(connectResult)) {
      connectionState = ConnectionState::Failed;
      serviceText = "Error de conexión IPC";
      if (updateFeedback) feedbackText = resultText(connectResult);
      return;
    }
    connectedPid = pid;
  }

  uint32_t version = 0;
  SwitubeIpcStatus status{};
  Result result = switubeIpcPing();
  if (R_SUCCEEDED(result)) result = switubeIpcGetApiVersion(&version);
  if (R_SUCCEEDED(result)) result = switubeIpcGetCapabilities(&capabilities);
  if (R_SUCCEEDED(result)) result = switubeIpcGetStatus(&status);
  if (R_FAILED(result)) {
    disconnectIpc();
    connectionState = ConnectionState::Failed;
    serviceText = "Error en solicitud IPC";
    if (updateFeedback) feedbackText = resultText(result);
    return;
  }

  if (version != SWITUBE_IPC_API_VERSION ||
      status.struct_size != sizeof(status) ||
      status.abi_version != SWITUBE_IPC_STATUS_ABI_VERSION ||
      !(capabilities & SwitubeIpcCapability_Status)) {
    connectionState = ConnectionState::Incompatible;
    serviceText = "Versión incompatible";
    if (updateFeedback) feedbackText = "La versión del overlay difiere de la del sysmodule";
    return;
  }

  currentStatus = status;
  connectionState = ConnectionState::Connected;
  serviceText = engineStateText(status);
  if (updateFeedback) feedbackText = "Servicio activo y conectado";
}

void launchService() {
  refreshServiceState(false);
  if (runningPid != 0) {
    feedbackText = "Background playback is already running";
    return;
  }

  std::remove(ReadyPath);
  Result result = pmshellInitialize();
  if (R_FAILED(result)) {
    serviceText = "Launch failed";
    feedbackText = "pm:shell " + resultText(result);
    return;
  }

  NcmProgramLocation location{};
  location.program_id = SwitubeProgramId;
  location.storageID = NcmStorageId_None;
  uint64_t pid = 0;
  result = pmshellLaunchProgram(0, &location, &pid);
  pmshellExit();
  if (R_FAILED(result) || pid == 0) {
    serviceText = "Launch failed";
    feedbackText = resultText(result);
    return;
  }

  runningPid = pid;
  connectionState = ConnectionState::Starting;
  serviceText = "Starting";
  feedbackText = "Starting YouTube Music in the background";
  readyText = "Waiting for readiness check";
}

void beginAutomaticStartup() {
  automaticLaunchActive = true;
  const auto now = std::chrono::steady_clock::now();
  // A process discovered without a working readiness marker is an existing
  // startup attempt, not proof that startup succeeded. The old state machine
  // returned forever merely because a PID existed.
  automaticLaunchAttempt = runningPid == 0 ? 0 : 1;
  automaticObservedPid = runningPid;
  automaticObservedAt = now;
  nextAutomaticLaunch = now;
}

bool terminateServiceForAutomaticRecovery() {
  disconnectIpc();
  Result result = pmshellInitialize();
  if (R_FAILED(result)) {
    feedbackText = "Automatic recovery: pm:shell " + resultText(result);
    return false;
  }
  result = pmshellTerminateProgram(SwitubeProgramId);
  pmshellExit();
  if (R_FAILED(result)) {
    feedbackText = "Automatic recovery stop failed: " + resultText(result);
    return false;
  }

  runningPid = 0;
  automaticObservedPid = 0;
  connectionState = ConnectionState::Dormant;
  serviceText = "Restarting";
  readyText = "Restarting after readiness timeout";
  return true;
}

void maintainAutomaticStartup() {
  if (!automaticLaunchActive) return;
  const auto now = std::chrono::steady_clock::now();
  if (connectionState == ConnectionState::Connected) {
    // Connected is the healthy state of a persistent supervisor, not the end
    // of automatic startup. Reset its retry budget so an unexpected service
    // exit during Home/Library/Search can be relaunched immediately.
    automaticLaunchAttempt = 0;
    automaticObservedPid = runningPid;
    automaticObservedAt = now;
    nextAutomaticLaunch = now;
    return;
  }
  if (runningPid != 0) {
    if (automaticObservedPid != runningPid) {
      automaticObservedPid = runningPid;
      automaticObservedAt = now;
      if (automaticLaunchAttempt == 0) automaticLaunchAttempt = 1;
      return;
    }
    if (automaticObservedAt.time_since_epoch().count() == 0)
      automaticObservedAt = now;
    if (now - automaticObservedAt < StartupReadinessTimeout) return;

    const bool exhausted = automaticLaunchAttempt >= AutomaticLaunchAttempts;
    if (!terminateServiceForAutomaticRecovery()) {
      automaticLaunchActive = false;
      serviceText = "Startup recovery failed";
      return;
    }
    if (exhausted) {
      automaticLaunchActive = false;
      serviceText = "Startup failed";
      feedbackText = "Readiness timed out after automatic retries";
      return;
    }
    feedbackText = "Startup timed out; relaunching automatically";
    nextAutomaticLaunch = now + StartupRelaunchDelay;
    return;
  }

  if (now < nextAutomaticLaunch) return;
  if (automaticLaunchAttempt >= AutomaticLaunchAttempts) {
    automaticLaunchActive = false;
    feedbackText = "Automatic startup failed; open Settings to retry";
    return;
  }

  ++automaticLaunchAttempt;
  launchService();
  if (runningPid != 0) {
    automaticObservedPid = runningPid;
    automaticObservedAt = now;
    return;
  }

  const std::string failure = feedbackText;
  if (automaticLaunchAttempt >= AutomaticLaunchAttempts) {
    automaticLaunchActive = false;
    feedbackText = failure + "; automatic retries exhausted";
    return;
  }
  const unsigned delaySeconds = 1U << (automaticLaunchAttempt - 1);
  nextAutomaticLaunch = now + std::chrono::seconds(delaySeconds);
  feedbackText = failure + "; retrying automatically";
}

void superviseAutomaticService(bool force = false) {
  const auto now = std::chrono::steady_clock::now();
  if (!force && now < nextServiceSupervision) return;
  refreshServiceState(false);
  maintainAutomaticStartup();
  nextServiceSupervision = now + RefreshInterval;
}

void stopService() {
  automaticLaunchActive = false;
  automaticObservedPid = 0;
  disconnectIpc();
  if (!processIsRunning()) {
    runningPid = 0;
    connectionState = ConnectionState::Dormant;
    serviceText = "Dormant";
    feedbackText = "Background playback is already stopped";
    return;
  }

  Result result = pmshellInitialize();
  if (R_FAILED(result)) {
    serviceText = "Stop failed";
    feedbackText = "pm:shell " + resultText(result);
    return;
  }
  result = pmshellTerminateProgram(SwitubeProgramId);
  pmshellExit();
  if (R_FAILED(result)) {
    serviceText = "Stop failed";
    feedbackText = resultText(result);
    return;
  }

  runningPid = 0;
  connectionState = ConnectionState::Dormant;
  serviceText = "Dormant";
  feedbackText = "Background playback stopped";
}

template <typename Command>
bool dispatchControl(const char* description, Command command) {
  if (!controlsAvailable()) {
    feedbackText = "Playback controls are not ready";
    return false;
  }
  const Result result = command();
  feedbackText = R_SUCCEEDED(result) ? description : resultText(result);
  return R_SUCCEEDED(result);
}

void openBrowser(Result (*request)());

std::string clipped(std::string value, size_t maximum) {
  if (value.size() <= maximum) return value;
  if (maximum < 4) return value.substr(0, maximum);
  value.resize(maximum - 3);
  return value + "...";
}

std::string browseText(const char* value, size_t capacity) {
  return boundedText(value, capacity, "");
}

template <typename Action>
tsl::elm::ListItem* actionItem(const std::string& text,
                               const std::string& value, Action action) {
  auto* item = new tsl::elm::ListItem(text, value);
  item->setClickListener([action = std::move(action)](uint64_t keys) {
    if (!(keys & HidNpadButton_A)) return false;
    action();
    return true;
  });
  return item;
}

void drawOutline(tsl::gfx::Renderer* renderer, s32 x, s32 y, s32 width,
                 s32 height, tsl::Color color, s32 thickness = 2) {
  renderer->drawRect(x, y, width, thickness, color);
  renderer->drawRect(x, y + height - thickness, width, thickness, color);
  renderer->drawRect(x, y, thickness, height, color);
  renderer->drawRect(x + width - thickness, y, thickness, height, color);
}

class PlayerElement final : public tsl::elm::Element {
 public:
  static constexpr s32 Height = 475;

  tsl::elm::Element* requestFocus(tsl::elm::Element* oldFocus,
                                  tsl::FocusDirection direction) override {
    if (oldFocus != this) {
      if (direction == tsl::FocusDirection::Down ||
          direction == tsl::FocusDirection::Up) {
        focus = 2;
        lastControlFocus = 2;
      }
      consumedDirMask = HidNpadButton_AnyUp | HidNpadButton_AnyDown |
                        HidNpadButton_AnyLeft | HidNpadButton_AnyRight;
    }
    return this;
  }

  void layout(u16, u16, u16, u16) override {
    setBoundaries(getX(), getY(), getWidth(), Height);
  }

  void sync() {
    const std::string next = readTextFile(ArtworkUrlPath);
    if (next != artworkUrl) artworkUrl = next;

    // Starting artwork networking here rather than in initServices() leaves
    // the launch path free of the artwork BSD session and worker thread.  A
    // repeated request is cheap and lets artwork start as soon as a previously
    // booting service becomes healthy.
    if (connectionState == ConnectionState::Connected &&
        currentStatus.service_state == SwitubeServiceState_Ready &&
        !artworkUrl.empty()) {
      switube::artwork::request(artworkUrl, ArtSize);
    }
  }

  bool onClick(u64 keys) override {
    if (keys & HidNpadButton_AnyUp) {
      if (focus == 6) {
        focus = lastControlFocus;
        consumedDirMask = HidNpadButton_AnyUp;
        triggerNavigationFeedback();
        return true;
      }
      if (focus < 5) {
        lastControlFocus = focus;
        focus = 5;
        consumedDirMask = HidNpadButton_AnyUp;
        triggerNavigationFeedback();
        return true;
      }
      consumedDirMask = 0;
      return false;
    }
    if (keys & HidNpadButton_AnyDown) {
      if (focus == 5) {
        focus = lastControlFocus;
        consumedDirMask = HidNpadButton_AnyDown;
        triggerNavigationFeedback();
        return true;
      }
      if (focus < 5) {
        lastControlFocus = focus;
        focus = 6;
        consumedDirMask = HidNpadButton_AnyDown;
        triggerNavigationFeedback();
        return true;
      }
      consumedDirMask = 0;
      return false;
    }
    if (focus == 5) {
      if (keys & HidNpadButton_AnyLeft) {
        seekBy(-10000);
        holdStartNs = lastRepeatNs = ult::nowNs();
        consumedDirMask = HidNpadButton_AnyLeft;
        triggerNavigationFeedback();
        return true;
      }
      if (keys & HidNpadButton_AnyRight) {
        seekBy(10000);
        holdStartNs = lastRepeatNs = ult::nowNs();
        consumedDirMask = HidNpadButton_AnyRight;
        triggerNavigationFeedback();
        return true;
      }
    } else if (focus == 6) {
      if (keys & HidNpadButton_AnyLeft) {
        changeVolume(-50);
        holdStartNs = lastRepeatNs = ult::nowNs();
        consumedDirMask = HidNpadButton_AnyLeft;
        triggerNavigationFeedback();
        return true;
      }
      if (keys & HidNpadButton_AnyRight) {
        changeVolume(50);
        holdStartNs = lastRepeatNs = ult::nowNs();
        consumedDirMask = HidNpadButton_AnyRight;
        triggerNavigationFeedback();
        return true;
      }
    } else {
      if (keys & HidNpadButton_AnyLeft) {
        if (focus > 0) {
          --focus;
          lastControlFocus = focus;
          triggerNavigationFeedback();
        } else {
          shakeHighlight(tsl::FocusDirection::Left);
        }
        consumedDirMask = HidNpadButton_AnyLeft;
        return true;
      }
      if (keys & HidNpadButton_AnyRight) {
        if (focus < 4) {
          ++focus;
          lastControlFocus = focus;
          triggerNavigationFeedback();
        } else {
          shakeHighlight(tsl::FocusDirection::Right);
        }
        consumedDirMask = HidNpadButton_AnyRight;
        return true;
      }
    }
    if (keys & HidNpadButton_A) {
      activate(focus < 5 ? focus : 2);
      triggerEnterFeedback();
      return true;
    }
    if (keys & HidNpadButton_X) {
      toggleRepeat();
      triggerEnterFeedback();
      return true;
    }
    if (keys & HidNpadButton_Y) {
      toggleShuffle();
      triggerEnterFeedback();
      return true;
    }
    return false;
  }

  bool handleInput(u64 keysDown, u64 keysHeld, const HidTouchState&,
                   HidAnalogStickState, HidAnalogStickState) override {
    const u64 allDirs = HidNpadButton_AnyUp | HidNpadButton_AnyDown |
                        HidNpadButton_AnyLeft | HidNpadButton_AnyRight;
    if ((keysHeld & allDirs) == 0) {
      consumedDirMask = 0;
      return false;
    }
    if (keysHeld & (HidNpadButton_AnyLeft | HidNpadButton_AnyRight)) {
      if (keysDown == 0 && (focus == 5 || focus == 6)) {
        const u64 now = ult::nowNs();
        if (holdStartNs != 0 && now - holdStartNs >= 350000000ULL &&
            now - lastRepeatNs >= 120000000ULL) {
          lastRepeatNs = now;
          const int dir = (keysHeld & HidNpadButton_AnyLeft) ? -1 : 1;
          if (focus == 5)
            seekBy(dir * 10000);
          else
            changeVolume(dir * 50);
          triggerNavigationFeedback();
        }
      }
      return true;
    }
    if (consumedDirMask != 0 && (keysHeld & consumedDirMask) != 0) {
      return true;
    }
    return false;
  }

  bool onTouch(tsl::elm::TouchEvent event, s32 currX, s32 currY, s32, s32, s32,
               s32) override {
    if (!inBounds(currX, currY)) return false;
    const s32 edge = static_cast<s32>(ult::layerEdge);
    const s32 localX = currX - edge;
    const s32 x = getX();
    const s32 y = getY();
    const s32 width = getWidth();
    const s32 artY = y + 14;
    const s32 barX = x + 20;
    const s32 barY = artY + ArtSize + 78;
    const s32 barWidth = width - 40;
    const s32 controlsY = artY + ArtSize + 139;
    const s32 spacing = (width - 70) / 4;
    const s32 volumeBarX = x + 112;
    const s32 volumeBarY = controlsY + 66;
    const s32 volumeBarWidth = width - 138;

    // Seek bar touch
    if (currY >= barY - 18 && currY <= barY + 18 && localX >= barX - 8 &&
        localX <= barX + barWidth + 8) {
      focus = 5;
      if (event == tsl::elm::TouchEvent::Release &&
          (currentStatus.flags & SwitubeStatusFlag_CanSeek) &&
          currentStatus.duration_ms > 0) {
        const float ratio = std::clamp(
            static_cast<float>(localX - barX) / static_cast<float>(barWidth),
            0.0f, 1.0f);
        const uint32_t target =
            static_cast<uint32_t>(ratio * currentStatus.duration_ms);
        if (dispatchControl("Posicion cambiada",
                            [target] { return switubeIpcSeek(target); })) {
          currentStatus.position_ms = target;
        }
        triggerNavigationFeedback();
      }
      return true;
    }

    // Volume bar touch
    if (currY >= volumeBarY - 18 && currY <= volumeBarY + 18 &&
        localX >= volumeBarX - 8 &&
        localX <= volumeBarX + volumeBarWidth + 8) {
      focus = 6;
      if (event == tsl::elm::TouchEvent::Release) {
        const float ratio = std::clamp(
            static_cast<float>(localX - volumeBarX) /
                static_cast<float>(volumeBarWidth),
            0.0f, 1.0f);
        const uint32_t value =
            static_cast<uint32_t>(std::lround(ratio * 20.0f)) * 50;
        if (dispatchControl("Volumen cambiado",
                            [value] { return switubeIpcSetVolume(value); })) {
          currentStatus.volume_permille = value;
        }
        triggerNavigationFeedback();
      }
      return true;
    }

    // Transport buttons touch
    if (currY >= controlsY - 26 && currY <= controlsY + 26) {
      for (int index = 0; index < 5; ++index) {
        const s32 controlX = x + 35 + spacing * index;
        if (localX >= controlX - 24 && localX <= controlX + 24) {
          focus = index;
          lastControlFocus = index;
          if (event == tsl::elm::TouchEvent::Release) {
            activate(index);
            triggerEnterFeedback();
          }
          return true;
        }
      }
    }
    return false;
  }

  void draw(tsl::gfx::Renderer* renderer) override {
    const tsl::Color green = a({0xF, 0x1, 0x2, 0xF});
    const tsl::Color dimGreen = a({0x8, 0x1, 0x2, 0xA});
    const tsl::Color text = a(tsl::style::color::ColorText);
    const tsl::Color dim = a(tsl::style::color::ColorHeaderBar);
    const tsl::Color panel = a(tsl::style::color::ColorFrame);
    const s32 x = getX();
    const s32 y = getY();
    const s32 width = getWidth();
    const s32 center = x + width / 2;
    const s32 artX = center - ArtSize / 2;
    const s32 artY = y + 14;

    renderer->drawRect(x + 4, y + 4, width - 8, Height - 8, panel);
    drawOutline(renderer, x + 4, y + 4, width - 8, Height - 8,
                hasFocus() ? green : dimGreen, hasFocus() ? 3 : 2);

    const uint8_t* pixels = nullptr;
    int artWidth = 0;
    int artHeight = 0;
    switube::artwork::borrow(&pixels, &artWidth, &artHeight);
    if (pixels && artWidth > 0 && artHeight > 0) {
      const s32 decodedX = artX + (ArtSize - artWidth) / 2;
      const s32 decodedY = artY + (ArtSize - artHeight) / 2;
      renderer->drawBitmap(decodedX, decodedY, artWidth, artHeight, pixels);
    } else {
      renderer->drawRect(artX, artY, ArtSize, ArtSize,
                         a({0x1, 0x2, 0x2, 0xF}));
      drawOutline(renderer, artX, artY, ArtSize, ArtSize, dimGreen);
      renderer->drawLine(center + 18, artY + 70, center + 18,
                         artY + 142, green);
      renderer->drawLine(center + 18, artY + 70, center + 58,
                         artY + 61, green);
      renderer->drawCircle(center + 3, artY + 148, 15, true, green);
    }

    const bool hasTrack = currentStatus.flags & SwitubeStatusFlag_HasTrack;
    const std::string title = clipped(
        hasTrack ? boundedText(currentStatus.title, sizeof(currentStatus.title))
                 : "Elige una canción",
        31);
    const std::string artist = clipped(
        hasTrack ? boundedText(currentStatus.artist,
                               sizeof(currentStatus.artist))
                 : (connectionState == ConnectionState::Connected ? "Pulsa A en Canciones descargadas" : serviceText),
        36);
    centeredText(renderer, title, center, artY + ArtSize + 31, 23, text);
    centeredText(renderer, artist, center, artY + ArtSize + 54, 17, dim);

    const s32 barX = x + 20;
    const s32 barY = artY + ArtSize + 78;
    const s32 barWidth = width - 40;
    const float fraction = currentStatus.duration_ms == 0
                               ? 0.0f
                               : std::clamp(
                                     static_cast<float>(currentStatus.position_ms) /
                                         currentStatus.duration_ms,
                                     0.0f, 1.0f);
    if (hasFocus() && focus == 5) {
      renderer->drawRect(barX - 4, barY - 7, barWidth + 8, 17,
                         a({0xF, 0x1, 0x2, 0x6}));
      drawOutline(renderer, barX - 4, barY - 7, barWidth + 8, 17, text, 2);
    }
    renderer->drawRect(barX, barY, barWidth, 3, a({0xA, 0xA, 0xA, 0xA}));
    const s32 fill = static_cast<s32>(barWidth * fraction);
    if (fill > 0) renderer->drawRect(barX, barY - 2, fill, 7, green);
    renderer->drawCircle(barX + fill, barY + 1,
                         (hasFocus() && focus == 5) ? 8 : 6, true,
                         (hasFocus() && focus == 5) ? text : green);
    renderer->drawString(formatTime(currentStatus.position_ms), false, barX,
                         barY + 26, 16, text);
    const std::string total = formatTime(currentStatus.duration_ms);
    const auto [totalWidth, totalHeight] = renderer->drawString(
        total, false, 0, 0, 16, tsl::style::color::ColorTransparent);
    (void)totalHeight;
    renderer->drawString(total, false, barX + barWidth - totalWidth,
                         barY + 26, 16, text);

    const s32 controlsY = artY + ArtSize + 139;
    const s32 spacing = (width - 70) / 4;
    for (int index = 0; index < 5; ++index) {
      const s32 controlX = x + 35 + spacing * index;
      if (hasFocus() && focus == index) {
        renderer->drawRect(controlX - 22, controlsY - 22, 44, 44,
                           a({0xF, 0x1, 0x2, 0x7}));
        drawOutline(renderer, controlX - 22, controlsY - 22, 44, 44, text, 2);
      }
      drawControl(renderer, index, controlX, controlsY,
                  index == 0 &&
                          (currentStatus.flags &
                           (SwitubeStatusFlag_RepeatAll |
                            SwitubeStatusFlag_RepeatOne))
                      ? green
                      : (index == 4 &&
                                 (currentStatus.flags &
                                  SwitubeStatusFlag_Shuffle)
                             ? green
                             : text));
    }

    const s32 volumePercentY = controlsY + 43;
    const s32 volumeBarX = x + 112;
    const s32 volumeBarY = controlsY + 66;
    const s32 volumeBarWidth = width - 138;
    const uint32_t volume = std::min(UINT32_C(1000),
                                     currentStatus.volume_permille);
    // Keep the label on the slider's row. Only the percentage belongs above.
    renderer->drawString("Volumen", false, x + 20, volumeBarY + 6, 16, text);
    const std::string volumeText = std::to_string(volume / 10) + "%";
    const auto [volumeTextWidth, volumeTextHeight] = renderer->drawString(
        volumeText, false, 0, 0, 16, tsl::style::color::ColorTransparent);
    (void)volumeTextHeight;
    renderer->drawString(volumeText, false, x + width - 20 - volumeTextWidth,
                         volumePercentY, 16, text);
    if (hasFocus() && focus == 6) {
      renderer->drawRect(volumeBarX - 4, volumeBarY - 7,
                         volumeBarWidth + 8, 17, a({0xF, 0x1, 0x2, 0x6}));
      drawOutline(renderer, volumeBarX - 4, volumeBarY - 7,
                  volumeBarWidth + 8, 17, text, 2);
    }
    renderer->drawRect(volumeBarX, volumeBarY, volumeBarWidth, 3,
                       a({0xA, 0xA, 0xA, 0xA}));
    const s32 volumeFill = static_cast<s32>(volumeBarWidth * volume / 1000.0f);
    if (volumeFill > 0)
      renderer->drawRect(volumeBarX, volumeBarY - 2, volumeFill, 7, green);
    renderer->drawCircle(volumeBarX + volumeFill, volumeBarY + 1,
                         (hasFocus() && focus == 6) ? 8 : 6, true,
                         (hasFocus() && focus == 6) ? text : green);

    if (hasFocus()) {
      const char* hint = "Play/Pausa  ·  A  ·  Izq/Der/Arr/Abj mover";
      switch (focus) {
        case 0:
          hint = "Repetir  ·  A cambiar  ·  Izq/Der mover";
          break;
        case 1:
          hint = "Anterior  ·  A volver  ·  Izq/Der mover";
          break;
        case 2:
          hint = "Play/Pausa  ·  A  ·  Izq/Der/Arr/Abj mover";
          break;
        case 3:
          hint = "Siguiente  ·  A saltar  ·  Izq/Der mover";
          break;
        case 4:
          hint = "Aleatorio  ·  A cambiar  ·  Izq/Der mover";
          break;
        case 5:
          hint = "Tiempo  ·  Izq/Der -10s/+10s  ·  Abj controles";
          break;
        case 6:
          hint = "Volumen  ·  Izq/Der -5%/+5%  ·  Arr controles";
          break;
      }
      centeredText(renderer, hint, center, volumeBarY + 42, 14, dim);
    }
  }

 private:
  static constexpr s32 ArtSize = 176;

  void centeredText(tsl::gfx::Renderer* renderer, const std::string& value,
                    s32 center, s32 baseline, u32 size,
                    tsl::Color color) const {
    const auto [width, height] = renderer->drawString(
        value, false, 0, 0, size, tsl::style::color::ColorTransparent);
    (void)height;
    renderer->drawString(value, false, center - width / 2, baseline, size,
                         color);
  }

  void drawControl(tsl::gfx::Renderer* renderer, int index, s32 x, s32 y,
                   tsl::Color color) const {
    if (index == 0) {
      renderer->drawLine(x - 13, y - 7, x + 11, y - 7, color);
      renderer->drawLine(x + 11, y - 7, x + 6, y - 12, color);
      renderer->drawLine(x + 11, y - 7, x + 6, y - 2, color);
      renderer->drawLine(x + 13, y + 7, x - 11, y + 7, color);
      renderer->drawLine(x - 11, y + 7, x - 6, y + 2, color);
      renderer->drawLine(x - 11, y + 7, x - 6, y + 12, color);
      if (currentStatus.flags & SwitubeStatusFlag_RepeatOne)
        renderer->drawString("1", false, x + 8, y + 2, 12, color);
    } else if (index == 1 || index == 3) {
      const bool next = index == 3;
      renderer->drawRect(x + (next ? 10 : -12), y - 12, 3, 24, color);
      for (s32 line = 0; line <= 20; ++line) {
        const s32 half = next ? (20 - line) * 12 / 20
                              : line * 12 / 20;
        const s32 px = x - 10 + line;
        renderer->drawLine(px, y - half, px, y + half, color);
      }
    } else if (index == 2) {
      if (currentStatus.playback_state == SwitubePlaybackState_Playing) {
        renderer->drawRect(x - 9, y - 13, 6, 26, color);
        renderer->drawRect(x + 4, y - 13, 6, 26, color);
      } else {
        for (s32 line = 0; line <= 24; ++line) {
          const s32 half = (24 - line) * 13 / 24;
          renderer->drawLine(x - 10 + line, y - half, x - 10 + line,
                             y + half, color);
        }
      }
    } else {
      renderer->drawLine(x - 14, y - 9, x - 5, y - 9, color);
      renderer->drawLine(x - 5, y - 9, x + 7, y + 9, color);
      renderer->drawLine(x + 7, y + 9, x + 14, y + 9, color);
      renderer->drawLine(x + 14, y + 9, x + 9, y + 4, color);
      renderer->drawLine(x + 14, y + 9, x + 9, y + 14, color);
      renderer->drawLine(x - 14, y + 9, x - 5, y + 9, color);
      renderer->drawLine(x - 5, y + 9, x + 7, y - 9, color);
      renderer->drawLine(x + 7, y - 9, x + 14, y - 9, color);
      renderer->drawLine(x + 14, y - 9, x + 9, y - 14, color);
      renderer->drawLine(x + 14, y - 9, x + 9, y - 4, color);
    }
  }

  void seekBy(int32_t delta) {
    if (!(currentStatus.flags & SwitubeStatusFlag_CanSeek)) return;
    const int64_t target = std::clamp<int64_t>(
        static_cast<int64_t>(currentStatus.position_ms) + delta, 0,
        currentStatus.duration_ms);
    if (dispatchControl("Posicion cambiada", [target] {
          return switubeIpcSeek(static_cast<uint32_t>(target));
        }))
      currentStatus.position_ms = static_cast<uint32_t>(target);
  }

  void changeVolume(int32_t delta) {
    const int32_t value = std::clamp<int32_t>(
        static_cast<int32_t>(currentStatus.volume_permille) + delta, 0, 1000);
    if (dispatchControl("Volumen cambiado", [value] {
          return switubeIpcSetVolume(static_cast<uint32_t>(value));
        }))
      currentStatus.volume_permille = static_cast<uint32_t>(value);
  }

  void toggleRepeat() {
    SwitubeRepeatMode mode = SwitubeRepeatMode_All;
    const char* message = "Repetir todo";
    if (currentStatus.flags & SwitubeStatusFlag_RepeatAll) {
      mode = SwitubeRepeatMode_One;
      message = "Repetir una";
    } else if (currentStatus.flags & SwitubeStatusFlag_RepeatOne) {
      mode = SwitubeRepeatMode_Off;
      message = "No repetir";
    }
    if (dispatchControl(message,
                        [mode] { return switubeIpcSetRepeatMode(mode); })) {
      currentStatus.flags &= ~(SwitubeStatusFlag_RepeatAll |
                               SwitubeStatusFlag_RepeatOne);
      if (mode == SwitubeRepeatMode_All)
        currentStatus.flags |= SwitubeStatusFlag_RepeatAll;
      else if (mode == SwitubeRepeatMode_One)
        currentStatus.flags |= SwitubeStatusFlag_RepeatOne;
    }
  }

  void toggleShuffle() {
    const bool enabled = !(currentStatus.flags & SwitubeStatusFlag_Shuffle);
    if (dispatchControl(enabled ? "Aleatorio activado" : "Aleatorio desactivado",
                        [enabled] { return switubeIpcSetShuffle(enabled); })) {
      if (enabled)
        currentStatus.flags |= SwitubeStatusFlag_Shuffle;
      else
        currentStatus.flags &= ~SwitubeStatusFlag_Shuffle;
    }
  }

  void activate(int index) {
    switch (index) {
      case 0:
        toggleRepeat();
        break;
      case 1:
        dispatchControl("Cancion anterior", switubeIpcPrevious);
        break;
      case 2: {
        const bool playing = currentStatus.playback_state ==
                             SwitubePlaybackState_Playing;
        if (dispatchControl(playing ? "En pausa" : "Reproduciendo",
                            playing ? switubeIpcPause : switubeIpcPlay)) {
          currentStatus.playback_state =
              playing ? SwitubePlaybackState_Paused
                      : SwitubePlaybackState_Playing;
        }
        break;
      }
      case 3:
        dispatchControl("Siguiente cancion", switubeIpcNext);
        break;
      case 4:
        toggleShuffle();
        break;
    }
  }

  int focus = 2;
  int lastControlFocus = 2;
  u64 consumedDirMask = 0;
  u64 holdStartNs = 0;
  u64 lastRepeatNs = 0;
  std::string artworkUrl;
};

uint64_t currentBrowseGeneration() {
  SwitubeIpcBrowseHeader header{};
  return R_SUCCEEDED(switubeIpcGetBrowseHeader(&header)) ? header.generation
                                                         : 0;
}

class BrowseGui final : public tsl::Gui {
 public:
  explicit BrowseGui(uint64_t previousGeneration = 0)
      : previousGeneration(previousGeneration) {}

  tsl::elm::Element* createUI() override {
    claimExclusiveInput(true);
    superviseAutomaticService(true);
    auto* frame = new tsl::elm::OverlayFrame("Switube", "YouTube Music browser");
    auto* list = new tsl::elm::List();
    Result result = switubeIpcGetBrowseHeader(&header);
    if (R_FAILED(result) ||
        header.struct_size != sizeof(SwitubeIpcBrowseHeader) ||
        header.abi_version != SWITUBE_IPC_BROWSE_ABI_VERSION) {
      renderedUnavailable = true;
      list->addItem(new tsl::elm::CategoryHeader("Browser unavailable"));
      list->addItem(new tsl::elm::ListItem(
          automaticLaunchActive ? "Service is restarting automatically"
                                : "Could not read YouTube Music content",
          resultText(result)));
    } else if (header.generation == previousGeneration &&
               header.state != SwitubeBrowseState_Loading) {
      list->addItem(new tsl::elm::CategoryHeader("Opening YouTube Music"));
      list->addItem(new tsl::elm::ListItem("Loading...", "B Back"));
    } else {
      const std::string title = browseText(header.title, sizeof(header.title));
      const std::string subtitle =
          browseText(header.subtitle, sizeof(header.subtitle));
      list->addItem(new tsl::elm::CategoryHeader(
          title.empty() ? "YouTube Music" : title));
      if (!subtitle.empty())
        list->addItem(
            new tsl::elm::CategoryHeader(clipped(subtitle, 42)));
      if (header.state == SwitubeBrowseState_Loading) {
        list->addItem(new tsl::elm::ListItem("Loading YouTube Music content...", ""));
      } else if (header.state == SwitubeBrowseState_Error) {
        list->addItem(new tsl::elm::ListItem(
            clipped(browseText(header.message, sizeof(header.message)), 44),
            "B Back"));
      } else if (header.state == SwitubeBrowseState_Ready) {
        populateItems(list);
      } else {
        list->addItem(new tsl::elm::ListItem("Nothing to show yet", ""));
      }
    }
    renderedGeneration = header.generation;
    renderedState = header.state;
    renderedCount = header.item_count;
    frame->setContent(list);
    nextRefresh = std::chrono::steady_clock::now() + BrowseRefreshInterval;
    return frame;
  }

  void update() override {
    claimExclusiveInput();
    superviseAutomaticService();
    const auto now = std::chrono::steady_clock::now();
    if (now < nextRefresh) return;
    if (connectionState != ConnectionState::Connected) {
      if (!renderedUnavailable) {
        tsl::swapTo<BrowseGui>(previousGeneration);
        return;
      }
      nextRefresh = now + BrowseRefreshInterval;
      return;
    }
    if (renderedUnavailable) {
      tsl::swapTo<BrowseGui>(previousGeneration);
      return;
    }
    SwitubeIpcBrowseHeader latest{};
    if (R_SUCCEEDED(switubeIpcGetBrowseHeader(&latest)) &&
        (latest.generation != renderedGeneration ||
         latest.state != renderedState || latest.item_count != renderedCount)) {
      tsl::swapTo<BrowseGui>(previousGeneration);
      return;
    }
    nextRefresh = now + BrowseRefreshInterval;
  }

  bool handleInput(u64 keysDown, u64, const HidTouchState&,
                   HidAnalogStickState, HidAnalogStickState) override {
    if (keysDown & HidNpadButton_B) {
      // A network request must never trap the user inside Switube. Returning
      // false lets libtesla pop this GUI immediately without waiting for IPC.
      // Re-entering Home/Library/Search starts a fresh root request.
      if (renderedState == SwitubeBrowseState_Loading) return false;

      // The service can advance the browse generation while a large collection
      // is loading or being paged.  Never send Back with the GUI's cached
      // generation: that turns a perfectly valid B press into a silent no-op.
      SwitubeIpcBrowseHeader latest{};
      const Result headerResult = switubeIpcGetBrowseHeader(&latest);
      if (R_FAILED(headerResult) ||
          latest.struct_size != sizeof(SwitubeIpcBrowseHeader) ||
          latest.abi_version != SWITUBE_IPC_BROWSE_ABI_VERSION) {
        feedbackText = resultText(headerResult);
        return false;
      }
      if (latest.depth == 0) return false;

      const uint64_t old = latest.generation;
      const Result result = switubeIpcBrowseBack(old);
      if (R_SUCCEEDED(result)) {
        previousGeneration = old;
        nextRefresh = std::chrono::steady_clock::now();
      } else {
        feedbackText = resultText(result);
        return false;
      }
      return true;
    }
    return false;
  }

 private:
  void populateItems(tsl::elm::List* list) {
    if (header.item_count == 0) {
      list->addItem(new tsl::elm::ListItem("No results", "B Back"));
      return;
    }
    const uint32_t count = std::min<uint32_t>(header.item_count, 128);
    for (uint32_t index = 0; index < count; ++index) {
      SwitubeIpcBrowseItem item{};
      if (R_FAILED(switubeIpcGetBrowseItem(index, &item)) ||
          item.generation != header.generation)
        continue;
      std::string name = browseText(item.name, sizeof(item.name));
      std::string value = browseText(item.subtitle, sizeof(item.subtitle));
      if (item.kind == SwitubeBrowseItemKind_Track) {
        if (!value.empty()) name += " - " + value;
        name = clipped(name, 42);
        value = item.duration_ms > 0 ? formatTime(item.duration_ms) : "";
      } else {
        name = clipped(name, 36);
        value = clipped(value, 18);
      }
      auto* row = actionItem(name.empty() ? "Untitled" : name, value,
                             [this, index, kind = item.kind] {
        const uint64_t old = header.generation;
        const Result result =
            kind == SwitubeBrowseItemKind_Track
                ? switubeIpcBrowsePlay(old, index)
                : switubeIpcBrowseOpen(old, index);
        if (R_FAILED(result)) {
          feedbackText = resultText(result);
        } else if (kind != SwitubeBrowseItemKind_Track) {
          previousGeneration = old;
          nextRefresh = std::chrono::steady_clock::now();
        }
      });
      list->addItem(row);
    }
  }

  uint64_t previousGeneration = 0;
  SwitubeIpcBrowseHeader header{};
  uint64_t renderedGeneration = 0;
  uint32_t renderedState = SwitubeBrowseState_Idle;
  uint32_t renderedCount = 0;
  bool renderedUnavailable = false;
  std::chrono::steady_clock::time_point nextRefresh{};
};

class SearchKeyboardElement final : public tsl::elm::Element {
 public:
  tsl::elm::Element* requestFocus(tsl::elm::Element*,
                                  tsl::FocusDirection) override {
    return this;
  }

  void layout(u16, u16, u16, u16) override {
    setBoundaries(getX(), getY(), getWidth(), getHeight());
  }

  bool onClick(u64 keys) override {
    if (keys & HidNpadButton_AnyUp) {
      if (row > 0) --row;
      clampColumn();
      triggerNavigationFeedback();
      return true;
    }
    if (keys & HidNpadButton_AnyDown) {
      if (row + 1 < static_cast<int>(Rows.size())) ++row;
      clampColumn();
      triggerNavigationFeedback();
      return true;
    }
    if (keys & HidNpadButton_AnyLeft) {
      if (column > 0) --column;
      triggerNavigationFeedback();
      return true;
    }
    if (keys & HidNpadButton_AnyRight) {
      if (column + 1 < static_cast<int>(Rows[row].size())) ++column;
      triggerNavigationFeedback();
      return true;
    }
    if (keys & HidNpadButton_X) {
      if (!query.empty()) query.pop_back();
      triggerNavigationFeedback();
      return true;
    }
    if (keys & HidNpadButton_A) {
      activate(Rows[row][column]);
      triggerEnterFeedback();
      return true;
    }
    return false;
  }

  bool handleInput(u64, u64 keysHeld, const HidTouchState&,
                   HidAnalogStickState, HidAnalogStickState) override {
    return (keysHeld & (HidNpadButton_AnyUp | HidNpadButton_AnyDown |
                        HidNpadButton_AnyLeft | HidNpadButton_AnyRight)) != 0;
  }

  void draw(tsl::gfx::Renderer* renderer) override {
    const s32 x = getX();
    const s32 y = getY();
    const s32 width = getWidth();
    const tsl::Color green = a({0xF, 0x1, 0x2, 0xF});
    renderer->drawRect(x + 10, y + 14, width - 20, 43,
                       a(tsl::style::color::ColorFrame));
    renderer->drawString(query.empty() ? "Buscar en descargas" : clipped(query, 34),
                         false, x + 20, y + 43, 21,
                         a(tsl::style::color::ColorText));
    const s32 top = y + 76;
    const s32 rowHeight = 45;
    for (size_t r = 0; r < Rows.size(); ++r) {
      const auto& keys = Rows[r];
      const s32 cellWidth = (width - 20) / static_cast<s32>(keys.size());
      for (size_t c = 0; c < keys.size(); ++c) {
        const s32 keyX = x + 10 + static_cast<s32>(c) * cellWidth;
        const s32 keyY = top + static_cast<s32>(r) * rowHeight;
        renderer->drawRect(
            keyX + 2, keyY + 2, cellWidth - 4, rowHeight - 5,
            r == static_cast<size_t>(row) && c == static_cast<size_t>(column)
                ? green
                : a(tsl::style::color::ColorFrame));
        const std::string label = keys[c] == "SHIFT" && shifted
                                      ? "shift"
                                      : keys[c];
        const auto [textWidth, textHeight] = renderer->drawString(
            label, false, 0, 0, 16,
            tsl::style::color::ColorTransparent);
        (void)textHeight;
        renderer->drawString(label, false,
                             keyX + (cellWidth - textWidth) / 2, keyY + 27,
                             16, a(tsl::style::color::ColorText));
      }
    }
  }

 private:
  inline static const std::vector<std::vector<std::string>> Rows = {
      {"1", "2", "3", "4", "5", "6", "7", "8", "9", "0"},
      {"q", "w", "e", "r", "t", "y", "u", "i", "o", "p"},
      {"a", "s", "d", "f", "g", "h", "j", "k", "l"},
      {"z", "x", "c", "v", "b", "n", "m", "-"},
      {"SHIFT", "SPACE", "DEL", "SEARCH"}};

  void clampColumn() {
    column = std::clamp(column, 0, static_cast<int>(Rows[row].size()) - 1);
  }

  void activate(const std::string& key) {
    if (key == "SHIFT") {
      shifted = !shifted;
    } else if (key == "SPACE") {
      if (query.size() < SWITUBE_IPC_BROWSE_QUERY_SIZE - 1) query += ' ';
    } else if (key == "DEL") {
      if (!query.empty()) query.pop_back();
    } else if (key == "SEARCH") {
      if (query.empty()) return;
      const uint64_t old = currentBrowseGeneration();
      const Result result = switubeIpcBrowseSearch(query.c_str());
      if (R_SUCCEEDED(result))
        tsl::swapTo<BrowseGui>(old);
      else
        feedbackText = resultText(result);
    } else if (query.size() < SWITUBE_IPC_BROWSE_QUERY_SIZE - 1) {
      char character = key[0];
      if (shifted)
        character = static_cast<char>(
            std::toupper(static_cast<unsigned char>(character)));
      query += character;
      shifted = false;
    }
  }

  std::string query;
  int row = 1;
  int column = 0;
  bool shifted = false;
};

class SearchGui final : public tsl::Gui {
 public:
  tsl::elm::Element* createUI() override {
    // Foreground applications can reclaim controller input after Tesla's
    // initial hand-off. Search must own HID for its entire lifetime so typing
    // never repeats A/D-pad input in the application underneath.
    claimExclusiveInput(true);
    superviseAutomaticService(true);
    auto* frame = new tsl::elm::OverlayFrame(
        "Search", "A type   X backspace   B cancel");
    frame->setContent(new SearchKeyboardElement());
    return frame;
  }

  void update() override {
    claimExclusiveInput();
    superviseAutomaticService();
  }
};

void openBrowser(Result (*request)()) {
  superviseAutomaticService(true);
  if (connectionState != ConnectionState::Connected ||
      currentStatus.service_state != SwitubeServiceState_Ready ||
      !(capabilities & SwitubeIpcCapability_Browsing)) {
    feedbackText = "YouTube Music is still starting";
    return;
  }
  const uint64_t old = currentBrowseGeneration();
  const Result result = request();
  if (R_SUCCEEDED(result))
    tsl::changeTo<BrowseGui>(old);
  else
    feedbackText = resultText(result);
}

class ServiceGui final : public tsl::Gui {
 public:
  tsl::elm::Element* createUI() override {
    claimExclusiveInput(true);
    superviseAutomaticService(true);
    auto* frame = new tsl::elm::OverlayFrame("Switube", "Service & settings");
    auto* list = new tsl::elm::List();
    list->addItem(new tsl::elm::CategoryHeader("Background playback"));
    serviceItem = new tsl::elm::ListItem("Service", serviceText);
    feedbackItem = new tsl::elm::ListItem("Details", clipped(feedbackText, 26));
    list->addItem(serviceItem);
    list->addItem(feedbackItem);
    retryItem = actionItem("Retry automatic startup", "A", [this] {
      beginAutomaticStartup();
      maintainAutomaticStartup();
      sync();
    });
    stopItem = actionItem("Stop background playback", "A", [this] {
      stopService();
      sync();
    });
    list->addItem(retryItem);
    list->addItem(stopItem);
    list->addItem(actionItem("Refresh connection", "A", [this] {
      refreshServiceState();
      sync();
    }));
    list->addItem(new tsl::elm::CategoryHeader("Audio"));
    qualityItem = actionItem("Streaming quality", qualityText(), [this] {
      if (connectionState != ConnectionState::Connected ||
          !(capabilities & SwitubeIpcCapability_AudioSettings)) {
        feedbackText = "Update the overlay and background service together";
        sync();
        return;
      }
      const bool high =
          !(currentStatus.flags & SwitubeStatusFlag_HighQuality);
      const Result result = switubeIpcSetAudioQuality(high);
      if (R_SUCCEEDED(result)) {
        if (high)
          currentStatus.flags |= SwitubeStatusFlag_HighQuality;
        else
          currentStatus.flags &= ~SwitubeStatusFlag_HighQuality;
        feedbackText = high ? "High quality saved; applies next service start"
                            : "Normal quality saved; applies next service start";
      } else {
        feedbackText = resultText(result);
      }
      sync();
    });
    list->addItem(qualityItem);
    list->addItem(new tsl::elm::ListItem(
        "Normal 160 kbps / High 320 kbps", "Restart to apply"));
    list->addItem(new tsl::elm::CategoryHeader(
        "Sign in once with the standalone Switube app"));
    frame->setContent(list);
    sync();
    nextRefresh = std::chrono::steady_clock::now() + RefreshInterval;
    return frame;
  }

  void update() override {
    claimExclusiveInput();
    superviseAutomaticService();
    const auto now = std::chrono::steady_clock::now();
    if (now < nextRefresh) return;
    sync();
    nextRefresh = now + RefreshInterval;
  }

 private:
  void sync() {
    if (!serviceItem) return;
    serviceItem->setValue(serviceText);
    feedbackItem->setValue(clipped(feedbackText, 26));
    retryItem->isLocked = runningPid != 0;
    stopItem->isLocked = runningPid == 0;
    if (qualityItem) qualityItem->setValue(qualityText());
  }

  std::string qualityText() const {
    return (currentStatus.flags & SwitubeStatusFlag_HighQuality)
               ? "High · 320 kbps"
               : "Normal · 160 kbps";
  }

  std::chrono::steady_clock::time_point nextRefresh{};
  tsl::elm::ListItem* serviceItem = nullptr;
  tsl::elm::ListItem* feedbackItem = nullptr;
  tsl::elm::ListItem* retryItem = nullptr;
  tsl::elm::ListItem* stopItem = nullptr;
  tsl::elm::ListItem* qualityItem = nullptr;
};

class NowPlayingGui final : public tsl::Gui {
 public:
  tsl::elm::Element* createUI() override {
    claimExclusiveInput(true);
    superviseAutomaticService(true);
    auto* frame = new tsl::elm::OverlayFrame("Switube", "En reproduccion");
    auto* list = new tsl::elm::List();
    player = new PlayerElement();
    list->addItem(player, PlayerElement::Height);
    list->addItem(new tsl::elm::CategoryHeader("Sesion de reproduccion"));
    list->addItem(actionItem("Lista actual", queueText(currentStatus), [] {
      openBrowser(switubeIpcBrowseCurrentPlaylist);
    }));
    list->addItem(actionItem("Cola de reproduccion", "Siguientes canciones", [] {
      openBrowser(switubeIpcBrowseQueue);
    }));
    frame->setContent(list);
    player->sync();
    nextRefresh = std::chrono::steady_clock::now() + RefreshInterval;
    return frame;
  }

  void update() override {
    claimExclusiveInput();
    superviseAutomaticService();
    const auto now = std::chrono::steady_clock::now();
    if (now < nextRefresh) return;
    if (player) player->sync();
    nextRefresh = now + RefreshInterval;
  }

 private:
  PlayerElement* player = nullptr;
  std::chrono::steady_clock::time_point nextRefresh{};
};

class MainGui final : public tsl::Gui {
 public:
  tsl::elm::Element* createUI() override {
    claimExclusiveInput(true);
    superviseAutomaticService(true);
    auto* frame = new tsl::elm::OverlayFrame("Switube", OverlayVersion);
    auto* list = new tsl::elm::List();

    list->addItem(new tsl::elm::CategoryHeader("Musica Descargada"));
    list->addItem(actionItem("Canciones descargadas", "Musica en SD", [] {
      openBrowser(switubeIpcBrowseLibrary);
    }));
    list->addItem(actionItem("Buscar en descargas", "Filtrar por nombre o artista", [] {
      if (connectionState == ConnectionState::Connected &&
          currentStatus.service_state == SwitubeServiceState_Ready &&
          (capabilities & SwitubeIpcCapability_Search)) {
        claimExclusiveInput(true);
        tsl::changeTo<SearchGui>();
      } else {
        feedbackText = "Iniciando servicio...";
      }
    }));

    list->addItem(new tsl::elm::CategoryHeader("En reproduccion"));
    player = new PlayerElement();
    list->addItem(player, PlayerElement::Height);

    list->addItem(actionItem("Cola de reproduccion", "Ver lista actual", [] {
      tsl::changeTo<NowPlayingGui>();
    }));
    settingsItem = actionItem("Ajustes del servicio", serviceText, [] {
      tsl::changeTo<ServiceGui>();
    });
    list->addItem(settingsItem);
    frame->setContent(list);
    player->sync();
    nextRefresh = std::chrono::steady_clock::now() + RefreshInterval;
    return frame;
  }

  void update() override {
    claimExclusiveInput();
    superviseAutomaticService();
    const auto now = std::chrono::steady_clock::now();
    if (now < nextRefresh) return;
    if (player) player->sync();
    if (settingsItem) settingsItem->setValue(serviceText);
    nextRefresh = now + RefreshInterval;
  }

 private:
  PlayerElement* player = nullptr;
  tsl::elm::ListItem* settingsItem = nullptr;
  std::chrono::steady_clock::time_point nextRefresh{};
};

class SwitubeOverlay final : public tsl::Overlay {
 public:
  void initServices() override {
    refreshServiceState(false);
    if (connectionState != ConnectionState::Connected &&
        connectionState != ConnectionState::Incompatible) {
      beginAutomaticStartup();
      maintainAutomaticStartup();
    }
  }

  void exitServices() override {
    switube::artwork::shutdown();
    disconnectIpc();
  }

  void onHide() override {
    // The playback engine is a separate Atmosphere process and continues
    // normally. Release every overlay-owned worker, BSD session, and IPC
    // handle before Ultrahand returns to the game or HOME menu.
    switube::artwork::shutdown();
    disconnectIpc();
  }

  void onShow() override {
    refreshServiceState(false);
    if (connectionState != ConnectionState::Connected &&
        connectionState != ConnectionState::Incompatible) {
      beginAutomaticStartup();
      maintainAutomaticStartup();
    }
  }

  std::unique_ptr<tsl::Gui> loadInitialGui() override {
    return initially<MainGui>();
  }
};

}  // namespace

int main(int argc, char** argv) {
  return tsl::loop<SwitubeOverlay>(argc, argv);
}



