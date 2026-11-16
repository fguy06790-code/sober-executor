#include "scheduler.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <atomic>

static std::atomic<bool> g_ipc_started{false};

static const char* socket_path() {
    // Inside the Sober flatpak sandbox, /tmp maps to a per-app location.
    // Put the socket somewhere the loader can reach via `flatpak-spawn --host`
    // bind-mounts or an explicit --bind, or use $XDG_RUNTIME_DIR which is
    // visible from the host.
    static std::string p;
    if (!p.empty()) return p.c_str();
    const char* xrd = std::getenv("XDG_RUNTIME_DIR");
    if (xrd && *xrd) p = std::string(xrd) + "/sober-exec.sock";
    else             p = "/tmp/sober-exec.sock";
    return p.c_str();
}

static std::string recv_all(int fd) {
    // Protocol: 4-byte little-endian length, then payload.
    uint32_t n = 0;
    if (read(fd, &n, 4) != 4) return {};
    if (n == 0 || n > (1u << 24)) return {};
    std::string buf(n, 0);
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, buf.data() + got, n - got);
        if (r <= 0) return {};
        got += r;
    }
    return buf;
}

static void server_loop() {
    const char* path = socket_path();
    unlink(path);

    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) { std::perror("[payload] socket"); return; }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (bind(srv, (sockaddr*)&addr, sizeof(addr)) < 0) {
        std::perror("[payload] bind");
        close(srv);
        return;
    }
    chmod(path, 0666);

    if (listen(srv, 4) < 0) {
        std::perror("[payload] listen");
        close(srv);
        return;
    }

    std::fprintf(stderr, "[payload] ipc listening on %s\n", path);

    while (true) {
        int cl = accept(srv, nullptr, nullptr);
        if (cl < 0) {
            if (errno == EINTR) continue;
            break;
        }
        std::string src = recv_all(cl);
        close(cl);
        if (!src.empty()) {
            enqueue_script(src);
        }
    }
    close(srv);
}

void start_ipc_server() {
    bool expected = false;
    if (!g_ipc_started.compare_exchange_strong(expected, true)) return;
    std::thread(server_loop).detach();
}
