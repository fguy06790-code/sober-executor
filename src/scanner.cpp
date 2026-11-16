#include "common.h"

#include <elf.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <cstring>
#include <cstdio>
#include <vector>

namespace sober {

struct SigByte {
    uint8_t v;
    bool wild;
};

static std::vector<SigByte> parse_sig(const std::string& p) {
    std::vector<SigByte> out;
    for (size_t i = 0; i < p.size();) {
        while (i < p.size() && p[i] == ' ') ++i;
        if (i >= p.size()) break;
        if (p[i] == '?') {
            out.push_back({0, true});
            ++i;
            if (i < p.size() && p[i] == '?') ++i;
        } else {
            auto hex = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                return -1;
            };
            int hi = hex(p[i]);
            int lo = (i + 1 < p.size()) ? hex(p[i + 1]) : -1;
            if (hi < 0 || lo < 0) { ++i; continue; }
            out.push_back({(uint8_t)((hi << 4) | lo), false});
            i += 2;
        }
    }
    return out;
}

uintptr_t scan_pattern(pid_t pid, const MemRegion& r, const std::string& pattern) {
    auto sig = parse_sig(pattern);
    if (sig.empty()) return 0;

    std::vector<uint8_t> buf(r.size());
    if (!read_mem(pid, r.start, buf.data(), buf.size())) return 0;

    const size_t m = sig.size();
    for (size_t i = 0; i + m <= buf.size(); ++i) {
        bool match = true;
        for (size_t j = 0; j < m; ++j) {
            if (sig[j].wild) continue;
            if (buf[i + j] != sig[j].v) { match = false; break; }
        }
        if (match) return r.start + i;
    }
    return 0;
}

uintptr_t resolve_symbol(pid_t pid, const std::string& module_substr, const std::string& sym) {
    auto regs = read_maps(pid);
    auto m = find_region(regs, module_substr);
    if (!m) return 0;

    int fd = open(m->path.c_str(), O_RDONLY);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) < 0) { close(fd); return 0; }
    void* map = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return 0;

    const uint8_t* base = (const uint8_t*)map;
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)base;
    if (std::memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0) {
        munmap(map, st.st_size);
        return 0;
    }

    const Elf64_Shdr* sh = (const Elf64_Shdr*)(base + eh->e_shoff);
    const char* shstr = (const char*)(base + sh[eh->e_shstrndx].sh_offset);

    uintptr_t result = 0;
    for (uint16_t i = 0; i < eh->e_shnum; ++i) {
        const Elf64_Shdr& s = sh[i];
        if (s.sh_type != SHT_SYMTAB && s.sh_type != SHT_DYNSYM) continue;

        const Elf64_Sym* syms = (const Elf64_Sym*)(base + s.sh_offset);
        size_t n = s.sh_size / s.sh_entsize;
        const char* strs = (const char*)(base + sh[s.sh_link].sh_offset);

        for (size_t j = 0; j < n; ++j) {
            const Elf64_Sym& e = syms[j];
            if (!e.st_name || !e.st_value) continue;
            const char* name = strs + e.st_name;
            if (sym == name) {
                result = m->start + e.st_value;
                break;
            }
        }
        if (result) break;
        (void)shstr;
    }

    munmap(map, st.st_size);
    return result;
}

} // namespace sober
