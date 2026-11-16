#include "scheduler.h"
#include "luau.h"

#include <mutex>
#include <queue>
#include <string>
#include <cstdio>
#include <cstdlib>

namespace {
    std::mutex g_mu;
    std::queue<std::string> g_queue;
}

void enqueue_script(const std::string& src) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_queue.push(src);
}

static bool compile_and_run(lua_State* L, const std::string& src) {
    size_t bclen = 0;
    char* bc = g_lua.luau_compile(src.data(), src.size(), nullptr, &bclen);
    if (!bc || !bclen) {
        std::fprintf(stderr, "[payload] compile failed\n");
        std::free(bc);
        return false;
    }

    int load_rc = g_lua.luau_load(L, "=sober-exec", bc, bclen, 0);
    std::free(bc);

    if (load_rc != 0) {
        size_t n = 0;
        const char* err = g_lua.lua_tolstring(L, -1, &n);
        std::fprintf(stderr, "[payload] load error: %.*s\n", (int)n, err ? err : "?");
        g_lua.lua_settop(L, -2);
        return false;
    }

    // Elevate: identity 8 = elevated executor context in Roblox internal model.
    if (g_lua.r_setidentity) g_lua.r_setidentity(L, 8);

    int pc = g_lua.lua_pcall(L, 0, 0, 0);
    if (pc != 0) {
        size_t n = 0;
        const char* err = g_lua.lua_tolstring(L, -1, &n);
        std::fprintf(stderr, "[payload] runtime error: %.*s\n", (int)n, err ? err : "?");
        g_lua.lua_settop(L, -2);
        return false;
    }
    return true;
}

void drain_pending(void) {
    if (!g_lua.r_getstate) return;
    lua_State* parent = g_lua.r_getstate();
    if (!parent) return;

    std::string src;
    for (;;) {
        {
            std::lock_guard<std::mutex> lk(g_mu);
            if (g_queue.empty()) return;
            src = std::move(g_queue.front());
            g_queue.pop();
        }

        // Fresh coroutine so our frame doesn't collide with whatever the
        // client is currently running on `parent`.
        lua_State* co = g_lua.lua_newthread(parent);
        compile_and_run(co, src);
        g_lua.lua_settop(parent, -2); // pop the thread from `parent`
    }
}
