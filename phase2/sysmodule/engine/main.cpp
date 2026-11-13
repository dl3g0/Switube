#include "background_backend.hpp"
#include "ipc_self_test.hpp"
#include "switube_service.hpp"

#include "NXConsoleLog.h"

#include <curl/curl.h>
#include <switch.h>

#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <memory>
#include <new>

namespace {

constexpr SocketInitConfig SocketConfig = {
    .tcp_tx_buf_size = 0x800,
    .tcp_rx_buf_size = 0x1000,
    .tcp_tx_buf_max_size = 0x2EE0,
    .tcp_rx_buf_max_size = 0x4000,
    .udp_tx_buf_size = 0x800,
    .udp_rx_buf_size = 0x1000,
    .sb_efficiency = 2,
    .num_bsd_sessions = 3,
    .bsd_service_type = BsdServiceType_User,
};

Result socketInitResult = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);

}  // namespace

extern "C" {

uint32_t __nx_applet_type = AppletType_None;
uint32_t __nx_fs_num_sessions = 4;
static bool fsMounted = false;

void __libnx_initheap(void) {
  static char heap[1536 * 1024];
  extern char* fake_heap_start;
  extern char* fake_heap_end;
  fake_heap_start = heap;
  fake_heap_end = heap + sizeof(heap);
}

void __appInit(void) {
  if (R_FAILED(smInitialize())) return;
  if (R_SUCCEEDED(fsInitialize())) {
    if (R_SUCCEEDED(fsdevMountSdmc())) {
      fsMounted = true;
    } else {
      fsExit();
    }
  }
  socketInitResult = socketInitialize(&SocketConfig);
}

void __appExit(void) {
  curl_global_cleanup();
  if (R_SUCCEEDED(socketInitResult)) socketExit();
  if (fsMounted) {
    fsdevUnmountDevice("sdmc");
    fsExit();
  }
  smExit();
}

}  // extern "C"

int main(int, char**) {
  mkdir("/config", 0755);
  mkdir("/config/Switube", 0755);
  nxlog::setLogFile("/config/Switube/background.log", 64 * 1024);
  nxlog::setQueueEnabled(false);
  nxlog::push("Switube Phase 2 background engine starting (4 MiB pool)");

  curl_global_init(CURL_GLOBAL_DEFAULT);

  // Allocate ServiceCore on heap: stack is only 16 KiB!
  auto core = std::unique_ptr<switube::phase2::ServiceCore>(
      new (std::nothrow) switube::phase2::ServiceCore());
  if (!core) {
    nxlog::push("Fatal: Failed to allocate ServiceCore on heap");
    return 0;
  }

  const Result serviceResult = switube::phase2::switch_service::initialize(*core);
  if (R_FAILED(serviceResult)) {
    nxlog::pushf("Fatal: IPC service registration failed: 0x%08X", serviceResult);
    return 0;
  }

  auto backend = std::unique_ptr<switube::phase2::BackgroundBackend>(
      new (std::nothrow) switube::phase2::BackgroundBackend(*core));

  bool backendStarted = false;
  if (backend && R_SUCCEEDED(socketInitResult)) {
    backendStarted = backend->start();
    if (!backendStarted) {
      nxlog::push("Warning: BackgroundBackend failed to start initially");
    }
  } else {
    core->configureCommands(false, SwitubeIpcCapability_Status, 0);
    core->publishServiceState(SwitubeServiceState_Error, 0);
    nxlog::pushf("Socket unavailable at launch (0x%08X), service standing by", socketInitResult);
  }

  const Result testResult = switube::phase2::ipc_self_test::start();
  if (R_FAILED(testResult)) {
    nxlog::pushf("Warning: IPC self-test: 0x%08X", testResult);
  }

  nxlog::push("Switube Phase 2 background service online and waiting for IPC");
  switube::phase2::switch_service::loop();

  if (backend && backendStarted) {
    backend->stop();
  }
  switube::phase2::switch_service::exit();
  return 0;
}
