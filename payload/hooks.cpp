#include "scheduler.h"
#include "exec_api.h"
#include "luau.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <cstdio>

static std::atomic<bool> g_api_installed{false};
static std::atomic<bool> g_drain_thread_started{false};

// We don't yet patch the client's task scheduler step. Instead a polling
// drain thread runs at 60Hz, acquires the elevated thread and drains the
// queue. This is simpler and avoids chasing per-build offsets on the
// scheduler v-table.
//
// When you need frame-accurate hooks (e.g. for render-tied effects),
// replace the sleep loop below with a MinHook-equivalent on
// `RBX::TaskScheduler::stepJobs` after locating it via signature.

static void drain_loop() {
    using namespace std::chrono_literals;
    while (true) {
        // Install API on the elevated thread's globals once it's available.
        if (!g_api_installed.load() && g_lua.r_getstate) {
            lua_State* L = g_lua.r_getstate();
            if (L) {
                install_exec_api(L);
                g_api_installed.store(true);
                std::fprintf(stderr, "[payload] exec api installed\n");
            }
        }
        drain_pending();
        std::this_thread::sleep_for(16ms);
    }
}

void start_drain_thread() {
    bool expected = false;
    if (!g_drain_thread_started.compare_exchange_strong(expected, true)) return;
    std::thread(drain_loop).detach();
}
