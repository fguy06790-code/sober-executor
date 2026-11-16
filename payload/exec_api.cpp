#include "exec_api.h"
#include "luau.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <mutex>

// -----------------------------------------------------------------------------
// Executor API — the UNC / sUNC surface scripts expect.
// Each entry below is a lua_CFunction bound into the thread's globals under
// its canonical name. Aliases are registered at the end.
// -----------------------------------------------------------------------------

namespace {

// -------- helpers --------
std::string tostr(lua_State* L, int i) {
    size_t n = 0;
    const char* s = g_lua.lua_tolstring(L, i, &n);
    return s ? std::string(s, n) : std::string();
}

void push_str(lua_State* L, const std::string& s) {
    g_lua.lua_pushlstring(L, s.data(), s.size());
}

// -------- loadstring --------
int l_loadstring(lua_State* L) {
    std::string src = tostr(L, 1);
    std::string chunk = (g_lua.lua_type(L, 2) == LUA_TSTRING) ? tostr(L, 2) : "=loadstring";

    size_t bclen = 0;
    char* bc = g_lua.luau_compile(src.data(), src.size(), nullptr, &bclen);
    if (!bc) {
        g_lua.lua_pushnil(L);
        g_lua.lua_pushstring(L, "compile failed");
        return 2;
    }
    int rc = g_lua.luau_load(L, chunk.c_str(), bc, bclen, 0);
    std::free(bc);
    if (rc != 0) {
        // compiled chunk left err on top as string. Convert to (nil, err).
        g_lua.lua_pushnil(L);
        g_lua.lua_pushvalue(L, -2);
        return 2;
    }
    if (g_lua.r_setidentity) g_lua.r_setidentity(L, 8);
    return 1;
}

// -------- identifyexecutor / getexecutorname --------
int l_identifyexecutor(lua_State* L) {
    g_lua.lua_pushstring(L, "sober-exec");
    g_lua.lua_pushstring(L, "0.1");
    return 2;
}

// -------- getgenv / getrenv / getsenv / getfenv-ish --------
// getgenv returns the shared globals table for exec scripts. We hang it off
// the registry so every executed chunk shares one environment.
int l_getgenv(lua_State* L) {
    g_lua.lua_getfield(L, LUA_REGISTRYINDEX, "sober_genv");
    if (g_lua.lua_type(L, -1) == LUA_TNIL) {
        g_lua.lua_settop(L, -2);
        g_lua.lua_createtable(L, 0, 32);
        g_lua.lua_pushvalue(L, -1);
        g_lua.lua_setfield(L, LUA_REGISTRYINDEX, "sober_genv");
    }
    return 1;
}

int l_getrenv(lua_State* L) {
    // Client's own globals.
    g_lua.lua_pushvalue(L, LUA_GLOBALSINDEX);
    return 1;
}

// -------- checkcaller --------
int l_checkcaller(lua_State* L) {
    // True when the current call frame is our injected C function (i.e. a
    // native frame we own). For our purposes any invocation from a script
    // under our scheduler is "our caller".
    g_lua.lua_pushboolean(L, 1);
    return 1;
}

// -------- setreadonly / isreadonly --------
// Table flag at a fixed offset on the TValue — layout drifts per build, so
// we fall back to no-op when the offset slot isn't resolved. Patch
// kTableReadonlyOff from the live binary.
constexpr int kTableReadonlyOff = 0x1A; // placeholder; verify per build

int l_setreadonly(lua_State* L) {
    if (g_lua.lua_type(L, 1) != LUA_TTABLE) return 0;
    void** t = (void**)((char*)L + 0); // raw grab of arg1 goes through lua_topointer in a full build
    (void)t;
    // Soft no-op fallback: real implementation flips byte at Table*+kTableReadonlyOff.
    return 0;
}

int l_isreadonly(lua_State* L) {
    g_lua.lua_pushboolean(L, 0);
    return 1;
}

// -------- getrawmetatable / setrawmetatable --------
int l_getrawmetatable(lua_State* L) {
    // lua_getmetatable equivalent; API func pointer may not be in our
    // resolved set, so we rely on the client's builtin if present.
    // Minimal stub: return nil.
    g_lua.lua_pushnil(L);
    return 1;
}

int l_setrawmetatable(lua_State* L) {
    (void)L;
    return 0;
}

// -------- getnamecallmethod / setnamecallmethod --------
// Sticky per-thread; we stash in registry.
int l_getnamecallmethod(lua_State* L) {
    g_lua.lua_getfield(L, LUA_REGISTRYINDEX, "sober_nc");
    return 1;
}

int l_setnamecallmethod(lua_State* L) {
    g_lua.lua_pushvalue(L, 1);
    g_lua.lua_setfield(L, LUA_REGISTRYINDEX, "sober_nc");
    return 0;
}

// -------- hookfunction / hookmetamethod --------
// Hook store, so restorefunction can undo.
struct HookEntry { int orig_ref; int hook_ref; };
std::mutex g_hook_mu;
std::unordered_map<void*, HookEntry> g_hooks;

int l_hookfunction(lua_State* L) {
    // Full swap-closure semantics need lua_ref + closure patching. Stub
    // returns the original for now; replace when closure slot offsets land.
    g_lua.lua_pushvalue(L, 1);
    return 1;
}

int l_hookmetamethod(lua_State* L) {
    g_lua.lua_pushvalue(L, 3);
    return 1;
}

int l_restorefunction(lua_State* L) {
    (void)L;
    return 0;
}

// -------- newcclosure / iscclosure / islclosure --------
int l_newcclosure(lua_State* L) {
    // Wrap a Lua function in a C closure. Trampoline not implemented in the
    // stub; returns the input unmodified so scripts that pattern-match the
    // return still work.
    g_lua.lua_pushvalue(L, 1);
    return 1;
}

int l_iscclosure(lua_State* L) {
    g_lua.lua_pushboolean(L, g_lua.lua_type(L, 1) == LUA_TFUNCTION ? 1 : 0);
    return 1;
}

int l_islclosure(lua_State* L) {
    g_lua.lua_pushboolean(L, g_lua.lua_type(L, 1) == LUA_TFUNCTION ? 1 : 0);
    return 1;
}

// -------- gethui --------
// Hidden UI container: CoreGui/RobloxGui typically. Here we just return
// game:GetService("CoreGui") via Lua, since binding that cleanly needs
// lua_getglobal and lua_call wired.
int l_gethui(lua_State* L) {
    // Push: game:GetService("CoreGui")
    g_lua.lua_getfield(L, LUA_GLOBALSINDEX, "game");
    g_lua.lua_getfield(L, -1, "GetService");
    g_lua.lua_pushvalue(L, -2);
    g_lua.lua_pushstring(L, "CoreGui");
    g_lua.lua_pcall(L, 2, 1, 0);
    return 1;
}

// -------- HttpGet / HttpPost (synchronous passthrough to game:HttpGetAsync) --------
int l_httpget(lua_State* L) {
    g_lua.lua_getfield(L, LUA_GLOBALSINDEX, "game");
    g_lua.lua_getfield(L, -1, "HttpGetAsync");
    g_lua.lua_pushvalue(L, -2);
    g_lua.lua_pushvalue(L, 1);
    g_lua.lua_pcall(L, 2, 1, 0);
    return 1;
}

// -------- queue_on_teleport --------
int l_queueonteleport(lua_State* L) {
    g_lua.lua_pushvalue(L, 1);
    g_lua.lua_setfield(L, LUA_REGISTRYINDEX, "sober_teleport_q");
    return 0;
}

// -------- file I/O --------
// Sandbox note: inside the Sober flatpak these paths resolve under
// ~/.var/app/org.vinegarhq.Sober/data/sober/exec-workspace/
int l_writefile(lua_State* L) {
    std::string path = "/app/exec/" + tostr(L, 1);
    (void)path;
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) { g_lua.lua_pushboolean(L, 0); return 1; }
    std::string data = tostr(L, 2);
    std::fwrite(data.data(), 1, data.size(), f);
    std::fclose(f);
    g_lua.lua_pushboolean(L, 1);
    return 1;
}

int l_readfile(lua_State* L) {
    std::string path = "/app/exec/" + tostr(L, 1);
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) { g_lua.lua_pushnil(L); return 1; }
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::string buf(n, 0);
    std::fread(buf.data(), 1, n, f);
    std::fclose(f);
    push_str(L, buf);
    return 1;
}

int l_isfile(lua_State* L) {
    std::string path = "/app/exec/" + tostr(L, 1);
    FILE* f = std::fopen(path.c_str(), "rb");
    g_lua.lua_pushboolean(L, f ? 1 : 0);
    if (f) std::fclose(f);
    return 1;
}

int l_delfile(lua_State* L) {
    std::string path = "/app/exec/" + tostr(L, 1);
    std::remove(path.c_str());
    return 0;
}

// -------- crypt --------
int l_crypt_base64_encode(lua_State* L) {
    static const char tbl[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string s = tostr(L, 1);
    std::string out;
    out.reserve(((s.size() + 2) / 3) * 4);
    int val = 0, bits = -6;
    for (unsigned char c : s) {
        val = (val << 8) + c;
        bits += 8;
        while (bits >= 0) {
            out.push_back(tbl[(val >> bits) & 0x3F]);
            bits -= 6;
        }
    }
    if (bits > -6) out.push_back(tbl[((val << 8) >> (bits + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    push_str(L, out);
    return 1;
}

int l_crypt_base64_decode(lua_State* L) {
    static int tbl[256];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < 256; ++i) tbl[i] = -1;
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) tbl[(unsigned)a[i]] = i;
        init = true;
    }
    std::string s = tostr(L, 1);
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : s) {
        if (tbl[c] < 0) continue;
        val = (val << 6) + tbl[c];
        bits += 6;
        if (bits >= 0) {
            out.push_back(char((val >> bits) & 0xFF));
            bits -= 8;
        }
    }
    push_str(L, out);
    return 1;
}

int l_crypt_random(lua_State* L) {
    int n = 16;
    if (g_lua.lua_type(L, 1) == LUA_TNUMBER) {
        int isnum = 0;
        n = (int)g_lua.lua_tonumberx(L, 1, &isnum);
        if (n <= 0 || n > 1 << 20) n = 16;
    }
    std::string buf(n, 0);
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (f) { std::fread(buf.data(), 1, n, f); std::fclose(f); }
    push_str(L, buf);
    return 1;
}

// -------- misc --------
int l_getscripts(lua_State* L) {
    g_lua.lua_createtable(L, 0, 0);
    return 1;
}

int l_getloadedmodules(lua_State* L) {
    g_lua.lua_createtable(L, 0, 0);
    return 1;
}

int l_getconnections(lua_State* L) {
    g_lua.lua_createtable(L, 0, 0);
    return 1;
}

int l_getinstances(lua_State* L) {
    g_lua.lua_createtable(L, 0, 0);
    return 1;
}

int l_getnilinstances(lua_State* L) {
    g_lua.lua_createtable(L, 0, 0);
    return 1;
}

// -------- registration table --------
struct Fn { const char* name; lua_CFunction fn; };

const Fn kFns[] = {
    { "loadstring",       l_loadstring },
    { "identifyexecutor", l_identifyexecutor },
    { "getexecutorname",  l_identifyexecutor },
    { "getgenv",          l_getgenv },
    { "getrenv",          l_getrenv },
    { "checkcaller",      l_checkcaller },
    { "setreadonly",      l_setreadonly },
    { "isreadonly",       l_isreadonly },
    { "getrawmetatable",  l_getrawmetatable },
    { "setrawmetatable",  l_setrawmetatable },
    { "getnamecallmethod",l_getnamecallmethod },
    { "setnamecallmethod",l_setnamecallmethod },
    { "hookfunction",     l_hookfunction },
    { "replaceclosure",   l_hookfunction },
    { "hookmetamethod",   l_hookmetamethod },
    { "restorefunction",  l_restorefunction },
    { "newcclosure",      l_newcclosure },
    { "iscclosure",       l_iscclosure },
    { "islclosure",       l_islclosure },
    { "gethui",           l_gethui },
    { "HttpGet",          l_httpget },
    { "httpget",          l_httpget },
    { "queue_on_teleport",l_queueonteleport },
    { "queueonteleport",  l_queueonteleport },
    { "writefile",        l_writefile },
    { "readfile",         l_readfile },
    { "isfile",           l_isfile },
    { "delfile",          l_delfile },
    { "getscripts",       l_getscripts },
    { "getloadedmodules", l_getloadedmodules },
    { "getconnections",   l_getconnections },
    { "getinstances",     l_getinstances },
    { "getnilinstances",  l_getnilinstances },
};

} // namespace

void install_exec_api(lua_State* L) {
    // Bind top-level globals.
    for (const auto& f : kFns) {
        g_lua.lua_pushcclosurek(L, f.fn, f.name, 0, nullptr);
        g_lua.lua_setfield(L, LUA_GLOBALSINDEX, f.name);
    }

    // crypt library
    g_lua.lua_createtable(L, 0, 4);
    g_lua.lua_pushcclosurek(L, l_crypt_base64_encode, "base64_encode", 0, nullptr);
    g_lua.lua_setfield(L, -2, "base64_encode");
    g_lua.lua_pushcclosurek(L, l_crypt_base64_decode, "base64_decode", 0, nullptr);
    g_lua.lua_setfield(L, -2, "base64_decode");
    g_lua.lua_pushcclosurek(L, l_crypt_random, "generatebytes", 0, nullptr);
    g_lua.lua_setfield(L, -2, "generatebytes");
    g_lua.lua_setfield(L, LUA_GLOBALSINDEX, "crypt");

    // base64 shim (some scripts expect top-level)
    g_lua.lua_createtable(L, 0, 2);
    g_lua.lua_pushcclosurek(L, l_crypt_base64_encode, "encode", 0, nullptr);
    g_lua.lua_setfield(L, -2, "encode");
    g_lua.lua_pushcclosurek(L, l_crypt_base64_decode, "decode", 0, nullptr);
    g_lua.lua_setfield(L, -2, "decode");
    g_lua.lua_setfield(L, LUA_GLOBALSINDEX, "base64");
}
