#pragma once

// Luau C API surface used by the payload. Function pointers are resolved
// at runtime by scanning / symbol lookup inside RobloxPlayerBeta; these
// are the signatures only.
//
// Reference: https://github.com/luau-lang/luau — the client embeds Luau
// with the same ABI, stripped of symbols in release builds.

#include <cstddef>
#include <cstdint>

struct lua_State;

using lua_CFunction = int (*)(lua_State*);

constexpr int LUA_TNIL        = 0;
constexpr int LUA_TBOOLEAN    = 1;
constexpr int LUA_TLIGHTUSERDATA = 2;
constexpr int LUA_TNUMBER     = 3;
constexpr int LUA_TVECTOR     = 4;
constexpr int LUA_TSTRING     = 5;
constexpr int LUA_TTABLE      = 6;
constexpr int LUA_TFUNCTION   = 7;
constexpr int LUA_TUSERDATA   = 8;
constexpr int LUA_TTHREAD     = 9;

constexpr int LUA_GLOBALSINDEX  = -10002;
constexpr int LUA_REGISTRYINDEX = -10000;

struct LuauFns {
    // Core
    lua_State* (*lua_newthread)(lua_State* L);
    void       (*lua_close)(lua_State* L);
    int        (*lua_pcall)(lua_State* L, int nargs, int nres, int errf);
    int        (*lua_resume)(lua_State* L, lua_State* from, int nargs);
    void       (*lua_settop)(lua_State* L, int idx);
    int        (*lua_gettop)(lua_State* L);
    void       (*lua_pushvalue)(lua_State* L, int idx);
    void       (*lua_pushnil)(lua_State* L);
    void       (*lua_pushnumber)(lua_State* L, double n);
    void       (*lua_pushinteger)(lua_State* L, int n);
    void       (*lua_pushstring)(lua_State* L, const char* s);
    void       (*lua_pushlstring)(lua_State* L, const char* s, size_t n);
    void       (*lua_pushcclosurek)(lua_State* L, lua_CFunction fn, const char* debugname, int nup, void* cont);
    void       (*lua_pushboolean)(lua_State* L, int b);
    const char*(*lua_tolstring)(lua_State* L, int idx, size_t* len);
    double     (*lua_tonumberx)(lua_State* L, int idx, int* isnum);
    int        (*lua_toboolean)(lua_State* L, int idx);
    int        (*lua_type)(lua_State* L, int idx);
    void       (*lua_getfield)(lua_State* L, int idx, const char* k);
    void       (*lua_setfield)(lua_State* L, int idx, const char* k);
    void       (*lua_rawget)(lua_State* L, int idx);
    void       (*lua_rawset)(lua_State* L, int idx);
    void       (*lua_createtable)(lua_State* L, int narr, int nrec);

    // Luau bytecode
    int        (*luau_load)(lua_State* L, const char* chunkname,
                            const char* data, size_t size, int env);
    // Luau compile helper: produces bytecode from source.
    // char* luau_compile(const char* src, size_t srcLen, void* opts, size_t* outLen);
    char*      (*luau_compile)(const char* src, size_t srcLen, void* opts, size_t* outLen);

    // Roblox-specific: set the identity on a thread (identity 7 ≈ CoreScript,
    // identity 8 ≈ elevated). Signature varies by build; two arg form used here.
    // void (*r_setidentity)(lua_State* L, int identity);
    void       (*r_setidentity)(lua_State* L, int identity);

    // Global ScriptContext / DataModel getter. Returns a *lua_State for the
    // elevated actor thread we can reuse. Patched in by scanner.
    lua_State* (*r_getstate)();
};

extern LuauFns g_lua;

// Convenience wrappers.
inline void lua_pop(lua_State* L, int n) { g_lua.lua_settop(L, -n - 1); }
inline void lua_pushcfunction(lua_State* L, lua_CFunction f, const char* dbg) {
    g_lua.lua_pushcclosurek(L, f, dbg, 0, nullptr);
}
