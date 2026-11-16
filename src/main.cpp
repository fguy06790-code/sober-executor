#include "common.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

using namespace sober;

static void usage() {
    std::fprintf(stderr,
        "sober-exec: linux roblox executor loader (targets sober)\n"
        "usage:\n"
        "  sober-exec inject [--so <path>]   # locate sober, inject payload\n"
        "  sober-exec status                 # show detected process + maps summary\n"
        "  sober-exec pid                    # print the detected pid only\n");
}

static int cmd_status() {
    auto pid = find_roblox_pid();
    if (!pid) { std::printf("no roblox process detected\n"); return 1; }
    std::printf("pid: %d\n", *pid);
    auto regs = read_maps(*pid);
    auto main = find_region(regs, kTargetBinary);
    if (main) {
        std::printf("binary: %s\n", main->path.c_str());
        std::printf("base:   0x%lx\n", main->start);
    } else {
        std::printf("binary: (not found in maps)\n");
    }
    std::printf("regions: %zu\n", regs.size());
    return 0;
}

static int cmd_pid() {
    auto pid = find_roblox_pid();
    if (!pid) return 1;
    std::printf("%d\n", *pid);
    return 0;
}

static std::string abspath(const std::string& p) {
    if (p.empty()) return p;
    if (p[0] == '/') return p;
    char buf[4096];
    if (!getcwd(buf, sizeof(buf))) return p;
    return std::string(buf) + "/" + p;
}

static int cmd_inject(int argc, char** argv) {
    std::string so = "./build/libsoberpay.so";
    for (int i = 0; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--so") && i + 1 < argc) so = argv[++i];
    }
    so = abspath(so);

    auto pid = find_roblox_pid();
    if (!pid) { std::fprintf(stderr, "no roblox process detected\n"); return 1; }
    std::fprintf(stderr, "target pid: %d\n", *pid);
    std::fprintf(stderr, "payload:    %s\n", so.c_str());

    if (!inject_library(*pid, so)) {
        std::fprintf(stderr, "injection failed\n");
        return 2;
    }
    std::fprintf(stderr, "ok. socket: %s\n", kSocketPath);
    return 0;
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }
    std::string cmd = argv[1];
    if (cmd == "status") return cmd_status();
    if (cmd == "pid")    return cmd_pid();
    if (cmd == "inject") return cmd_inject(argc - 2, argv + 2);
    usage();
    return 1;
}
