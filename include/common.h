#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <optional>

namespace sober {

constexpr const char* kSocketPath = "/tmp/.sober-exec.sock";
constexpr const char* kPayloadName = "libsoberpay.so";
constexpr const char* kTargetBinary = "RobloxPlayerBeta";
constexpr const char* kSoberProcess = "sober";

struct MemRegion {
    uintptr_t start;
    uintptr_t end;
    std::string perms;
    std::string path;
    size_t size() const { return end - start; }
};

struct ProcInfo {
    pid_t pid;
    std::string exe_path;
    uintptr_t base;
    std::vector<MemRegion> regions;
};

std::optional<pid_t> find_roblox_pid();
std::vector<MemRegion> read_maps(pid_t pid);
std::optional<MemRegion> find_region(const std::vector<MemRegion>& regions, const std::string& substr);
bool read_mem(pid_t pid, uintptr_t addr, void* out, size_t n);

uintptr_t scan_pattern(pid_t pid, const MemRegion& r, const std::string& pattern);
uintptr_t resolve_symbol(pid_t pid, const std::string& module, const std::string& symbol);

bool inject_library(pid_t pid, const std::string& so_path);

} // namespace sober
