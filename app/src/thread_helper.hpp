#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <pthread.h>
#include "NXConsoleLog.h"

namespace switube {

namespace detail {
inline std::atomic<bool> g_shuttingDown{false};
inline std::atomic<int> g_activeWorkers{0};
inline std::mutex g_workerExitMutex;
inline std::condition_variable g_workerExitCv;

inline void onWorkerFinished() {
  if (g_activeWorkers.fetch_sub(1) == 1) {
    std::lock_guard<std::mutex> lock(g_workerExitMutex);
    g_workerExitCv.notify_all();
  }
}
} // namespace detail

inline bool isAppShuttingDown() {
  return detail::g_shuttingDown.load();
}

inline void requestAppShutdown() {
  detail::g_shuttingDown.store(true);
}

inline void waitForWorkers(uint32_t timeoutMs = 2000) {
  std::unique_lock<std::mutex> lock(detail::g_workerExitMutex);
  detail::g_workerExitCv.wait_for(
      lock, std::chrono::milliseconds(timeoutMs),
      []() { return detail::g_activeWorkers.load() <= 0; });
}

inline void launchWorkerWithStack(std::function<void()> work, size_t stackSize = 512 * 1024) {
  if (detail::g_shuttingDown.load()) return;

  struct Context {
    std::function<void()> work;
  };
  auto* ctx = new Context{std::move(work)};
  detail::g_activeWorkers.fetch_add(1);

  pthread_t pt;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, stackSize);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);

  int err = pthread_create(&pt, &attr, [](void* arg) -> void* {
    auto* c = static_cast<Context*>(arg);
    try {
      c->work();
    } catch (const std::exception& e) {
      nxlog::pushf("Thread exception: %s", e.what());
    } catch (...) {
      nxlog::push("Thread unknown exception");
    }
    delete c;
    detail::onWorkerFinished();
    return nullptr;
  }, ctx);

  pthread_attr_destroy(&attr);
  if (err != 0) {
    nxlog::pushf("pthread_create failed: %d, executing inline", err);
    try {
      ctx->work();
    } catch (...) {}
    delete ctx;
    detail::onWorkerFinished();
  }
}

} // namespace switube
