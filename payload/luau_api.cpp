#include "luau.h"

#include <dlfcn.h>
#include <link.h>
#include <elf.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cstdlib>

LuauFns g_lua = {};

// ---------------------------------------------------------------------------
// Pattern slots.
// Luau is almost always stripped in release RobloxPlayerBeta builds. These
// are prologue signatures that need refresh on each client update — pull
// them out with IDA/BinaryNinja on the live client binary and drop them in.
// Format: hex bytes, "??" = wildcard.
// ---------------------------------------------------------------------------
struct Sig { const char* name; const char* pat; };
static const Sig kSigs[] = {
    // Luau core
    { "lua_newthread",      "55 48 89 E5 41 57 41 56 41 55 41 54 53 48 83 EC ?? 49 89 FE" },
    { "lua_pcall",          "55 48 89 E5 41 57 41 56 41 55 41 54 53 50 44 89 C3 49 89 FF" },
    { "lua_resume",         "55 48 89 E5 41 57 41 56 41 55 41 54 53 48 83 EC ?? 48 89 FB" },
    { "lua_settop",         "55 48 89 E5 53 50 48 89 FB 48 8B 47 ?? 48 8D 48 ??" },
    { "lua_gettop",         "55 48 89 E5 48 8B 47 ?? 48 2B 47 ?? 48 C1 F8 ?? 5D C3" },
    { "lua_pushvalue",      "55 48 89 E5 53 50 48 89 FB E8 ?? ?? ?? ?? 48 8B 7B" },
    { "lua_pushnil",        "55 48 89 E5 48 8B 47 ?? C7 40 ?? ?? ?? ?? ?? 48 83 47" },
    { "lua_pushnumber",     "55 48 89 E5 48 8B 47 ?? F2 0F 11 00 C7 40 ?? 03 00 00 00" },
    { "lua_pushinteger",    "55 48 89 E5 F2 0F 2A C6 48 8B 47 ?? F2 0F 11 00" },
    { "lua_pushstring",     "55 48 89 E5 53 50 48 89 F3 48 85 F6 74 ?? 48 89 FB" },
    { "lua_pushlstring",    "55 48 89 E5 41 57 41 56 41 55 41 54 53 48 83 EC ?? 49 89 D7" },
    { "lua_pushcclosurek",  "55 48 89 E5 41 57 41 56 41 55 41 54 53 48 83 EC ?? 45 89 C7" },
    { "lua_pushboolean",    "55 48 89 E5 48 8B 47 ?? 89 70 ?? C7 40 ?? 01 00 00 00" },
    { "lua_tolstring",      "55 48 89 E5 41 56 53 50 49 89 D6 48 89 FB E8 ?? ?? ?? ??" },
    { "lua_tonumberx",      "55 48 89 E5 53 50 48 89 D3 E8 ?? ?? ?? ?? 48 85 C0" },
    { "lua_toboolean",      "55 48 89 E5 E8 ?? ?? ?? ?? 83 F8 ?? 0F 94 C0 ?? C3" },
    { "lua_type",           "55 48 89 E5 E8 ?? ?? ?? ?? 48 83 F8 ?? 75 ?? B8 FF FF FF FF" },
    { "lua_getfield",       "55 48 89 E5 41 56 53 50 49 89 D6 41 89 F5 48 89 FB" },
    { "lua_setfield",       "55 48 89 E5 41 57 41 56 41 55 41 54 53 50 49 89 D7" },
    { "lua_rawget",         "55 48 89 E5 53 48 89 FB 48 8B 47 ?? 48 83 E8 ??" },
    { "lua_rawset",         "55 48 89 E5 41 56 53 50 41 89 F6 48 89 FB E8" },
    { "lua_createtable",    "55 48 89 E5 41 56 53 50 41 89 D6 89 F3 48 89 FD" },
    // Luau bytecode
    { "luau_load",          "55 48 89 E5 41 57 41 56 41 55 41 54 53 48 81 EC ?? ?? 00 00" },
    { "luau_compile",       "55 48 89 E5 41 57 41 56 41 55 41 54 53 48 83 EC ?? 49 89 CE" },
    // Roblox-specific
    { "r_setidentity",      "55 48 89 E5 53 48 89 FB 48 8B 4F ?? 48 8B 09" },
    { "r_getstate",         "55 48 89 E5 48 8B 05 ?? ?? ?? ?? 48 8B 00 48 8B 40" },
};

// ---------------------------------------------------------------------------
// Self-contained pattern scanner against RobloxPlayerBeta's .text.
// ---------------------------------------------------------------------------
struct TextRange { uintptr_t start; size_t size; std::string path; };

static TextRange find_text_range() {
    TextRange tr{};
    FILE* f = std::fopen("/proc/self/maps", "r");
    if (!f) return tr;
    char line[4096];
    while (std::fgets(line, sizeof(line), f)) {
        if (!std::strstr(line, "RobloxPlayerBeta")) continue;
        uintptr_t s, e;
        char perms[8] = {0};
        unsigned long off = 0;
        int po = 0;
        if (std::sscanf(line, "%lx-%lx %7s %lx %*x:%*x %*u %n",
                        &s, &e, perms, &off, &po) < 4) continue;
        if (!std::strchr(perms, 'x')) continue;
        tr.start = s;
        tr.size = e - s;
        if (po > 0) {
            std::string p = line + po;
            while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(p.begin());
            while (!p.empty() && (p.back() == '\n' || p.back() == '\r')) p.pop_back();
            tr.path = p;
        }
        break;
    }
    std::fclose(f);
    return tr;
}

static std::vector<std::pair<uint8_t, bool>> parse_sig(const char* p) {
    std::vector<std::pair<uint8_t, bool>> out;
    while (*p) {
        while (*p == ' ') ++p;
        if (!*p) break;
        if (*p == '?') {
            out.emplace_back(0, true);
            ++p;
            if (*p == '?') ++p;
        } else {
            auto hx = [](char c) { return (c>='0'&&c<='9')?c-'0':(c>='a'&&c<='f')?c-'a'+10:(c>='A'&&c<='F')?c-'A'+10:-1; };
            int hi = hx(p[0]), lo = hx(p[1]);
            if (hi < 0 || lo < 0) { ++p; continue; }
            out.emplace_back((uint8_t)((hi<<4)|lo), false);
            p += 2;
        }
    }
    return out;
}

static uintptr_t scan(const uint8_t* mem, size_t n, const char* pat) {
    auto s = parse_sig(pat);
    if (s.empty()) return 0;
    for (size_t i = 0; i + s.size() <= n; ++i) {
        bool ok = true;
        for (size_t j = 0; j < s.size(); ++j) {
            if (s[j].second) continue;
            if (mem[i+j] != s[j].first) { ok = false; break; }
        }
        if (ok) return i;
    }
    return 0;
}

// Pull symbol table out of the on-disk ELF (works even when runtime symbols
// are stripped if the file itself keeps .symtab — uncommon in Roblox, so
// patterns remain the primary source of truth).
static uintptr_t symtab_lookup(const std::string& path, uintptr_t base, const char* name) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return 0; }
    void* map = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return 0;

    const uint8_t* b = (const uint8_t*)map;
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)b;
    if (std::memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) { munmap(map, st.st_size); return 0; }

    uintptr_t r = 0;
    const Elf64_Shdr* sh = (const Elf64_Shdr*)(b + eh->e_shoff);
    for (uint16_t i = 0; i < eh->e_shnum; ++i) {
        if (sh[i].sh_type != SHT_SYMTAB && sh[i].sh_type != SHT_DYNSYM) continue;
        const Elf64_Sym* ss = (const Elf64_Sym*)(b + sh[i].sh_offset);
        size_t n = sh[i].sh_size / sh[i].sh_entsize;
        const char* strs = (const char*)(b + sh[sh[i].sh_link].sh_offset);
        for (size_t j = 0; j < n; ++j) {
            if (!ss[j].st_name || !ss[j].st_value) continue;
            if (!std::strcmp(strs + ss[j].st_name, name)) { r = base + ss[j].st_value; break; }
        }
        if (r) break;
    }
    munmap(map, st.st_size);
    return r;
}

static uintptr_t resolve_one(const uint8_t* text, size_t n, uintptr_t base,
                             const std::string& path, const Sig& s) {
    // Try symtab first (cheap; nearly always empty in Roblox but free to try).
    if (uintptr_t a = symtab_lookup(path, base, s.name)) return a;
    uintptr_t off = scan(text, n, s.pat);
    if (!off) return 0;
    return base + off;
}

bool resolve_luau_api() {
    TextRange tr = find_text_range();
    if (!tr.start) {
        std::fprintf(stderr, "[payload] RobloxPlayerBeta .text not found\n");
        return false;
    }
    const uint8_t* text = (const uint8_t*)tr.start;

    auto R = [&](const char* name) -> uintptr_t {
        for (const auto& s : kSigs) {
            if (!std::strcmp(s.name, name))
                return resolve_one(text, tr.size, tr.start, tr.path, s);
        }
        return 0;
    };

    #define BIND(member) g_lua.member = reinterpret_cast<decltype(g_lua.member)>(R(#member))
    BIND(lua_newthread);
    BIND(lua_pcall);
    BIND(lua_resume);
    BIND(lua_settop);
    BIND(lua_gettop);
    BIND(lua_pushvalue);
    BIND(lua_pushnil);
    BIND(lua_pushnumber);
    BIND(lua_pushinteger);
    BIND(lua_pushstring);
    BIND(lua_pushlstring);
    BIND(lua_pushcclosurek);
    BIND(lua_pushboolean);
    BIND(lua_tolstring);
    BIND(lua_tonumberx);
    BIND(lua_toboolean);
    BIND(lua_type);
    BIND(lua_getfield);
    BIND(lua_setfield);
    BIND(lua_rawget);
    BIND(lua_rawset);
    BIND(lua_createtable);
    BIND(luau_load);
    BIND(luau_compile);
    BIND(r_setidentity);
    BIND(r_getstate);
    #undef BIND

    int missing = 0;
    #define CHK(m) if (!g_lua.m) { std::fprintf(stderr, "[payload] unresolved: %s\n", #m); ++missing; }
    CHK(lua_pcall); CHK(luau_load); CHK(luau_compile);
    CHK(lua_settop); CHK(lua_gettop); CHK(lua_tolstring);
    CHK(r_getstate);
    #undef CHK

    return missing == 0;
}
