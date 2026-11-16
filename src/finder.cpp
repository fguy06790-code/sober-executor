#include "common.h"

#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

namespace sober {

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    if (!f) return {};
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string read_cmdline(pid_t pid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/cmdline", pid);
    std::string s = read_file(path);
    for (char& c : s) if (c == '\0') c = ' ';
    return s;
}

static std::string read_comm(pid_t pid) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/comm", pid);
    std::string s = read_file(path);
    if (!s.empty() && s.back() == '\n') s.pop_back();
    return s;
}

static std::string read_exe(pid_t pid) {
    char path[64], target[4096];
    std::snprintf(path, sizeof(path), "/proc/%d/exe", pid);
    ssize_t n = readlink(path, target, sizeof(target) - 1);
    if (n <= 0) return {};
    target[n] = 0;
    return std::string(target);
}

std::optional<pid_t> find_roblox_pid() {
    DIR* d = opendir("/proc");
    if (!d) return std::nullopt;

    pid_t best = -1;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_type != DT_DIR) continue;
        pid_t pid = 0;
        for (const char* p = e->d_name; *p; ++p) {
            if (*p < '0' || *p > '9') { pid = 0; break; }
            pid = pid * 10 + (*p - '0');
        }
        if (pid <= 0) continue;

        std::string comm = read_comm(pid);
        std::string exe = read_exe(pid);
        std::string cmd = read_cmdline(pid);

        bool hit = false;
        if (comm.find("RobloxPlayer") != std::string::npos) hit = true;
        if (exe.find("RobloxPlayer") != std::string::npos) hit = true;
        if (cmd.find("RobloxPlayerBeta") != std::string::npos) hit = true;

        if (hit) {
            best = pid;
            break;
        }
    }
    closedir(d);

    if (best > 0) return best;
    return std::nullopt;
}

std::vector<MemRegion> read_maps(pid_t pid) {
    std::vector<MemRegion> out;
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/maps", pid);
    std::ifstream f(path);
    if (!f) return out;

    std::string line;
    while (std::getline(f, line)) {
        uintptr_t start = 0, end = 0;
        char perms[8] = {0};
        unsigned long off = 0;
        int dev_a = 0, dev_b = 0;
        unsigned long inode = 0;
        int path_off = 0;
        int got = std::sscanf(line.c_str(),
                              "%lx-%lx %7s %lx %x:%x %lu %n",
                              &start, &end, perms, &off, &dev_a, &dev_b, &inode, &path_off);
        if (got < 7) continue;

        MemRegion r;
        r.start = start;
        r.end = end;
        r.perms = perms;
        if (path_off > 0 && (size_t)path_off < line.size()) {
            std::string p = line.substr(path_off);
            while (!p.empty() && (p.front() == ' ' || p.front() == '\t')) p.erase(p.begin());
            r.path = p;
        }
        out.push_back(r);
    }
    return out;
}

std::optional<MemRegion> find_region(const std::vector<MemRegion>& regions, const std::string& substr) {
    for (const auto& r : regions) {
        if (r.path.find(substr) != std::string::npos) return r;
    }
    return std::nullopt;
}

bool read_mem(pid_t pid, uintptr_t addr, void* out, size_t n) {
    char path[64];
    std::snprintf(path, sizeof(path), "/proc/%d/mem", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    bool ok = pread(fd, out, n, (off_t)addr) == (ssize_t)n;
    close(fd);
    return ok;
}

} // namespace sober
