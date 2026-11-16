// sober-cli: send a script over the unix socket to the running payload.
//
// usage:
//   sober-cli exec <path>           # read file, send contents
//   sober-cli exec -                # read stdin
//   sober-cli exec -s "<code>"      # send literal string
//   sober-cli ping                  # verify socket is live
//
// socket resolution matches payload/ipc_server.cpp: $XDG_RUNTIME_DIR/sober-exec.sock
// else /tmp/sober-exec.sock.

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

static std::string sock_path() {
    const char* x = std::getenv("XDG_RUNTIME_DIR");
    if (x && *x) return std::string(x) + "/sober-exec.sock";
    return "/tmp/sober-exec.sock";
}

static int connect_sock() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::string p = sock_path();
    std::strncpy(a.sun_path, p.c_str(), sizeof(a.sun_path) - 1);
    if (connect(fd, (sockaddr*)&a, sizeof(a)) < 0) {
        std::fprintf(stderr, "connect(%s): %s\n", p.c_str(), std::strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int send_script(const std::string& src) {
    int fd = connect_sock();
    if (fd < 0) return 1;
    uint32_t n = (uint32_t)src.size();
    if (write(fd, &n, 4) != 4) { close(fd); return 2; }
    size_t w = 0;
    while (w < src.size()) {
        ssize_t r = write(fd, src.data() + w, src.size() - w);
        if (r <= 0) { close(fd); return 2; }
        w += r;
    }
    close(fd);
    return 0;
}

static std::string read_file(const std::string& p) {
    if (p == "-") {
        std::stringstream s;
        s << std::cin.rdbuf();
        return s.str();
    }
    std::ifstream f(p);
    if (!f) { std::fprintf(stderr, "cannot open %s\n", p.c_str()); std::exit(1); }
    std::stringstream s;
    s << f.rdbuf();
    return s.str();
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: sober-cli exec <path|-> | exec -s <code> | ping\n");
        return 1;
    }
    std::string cmd = argv[1];

    if (cmd == "ping") {
        int fd = connect_sock();
        if (fd < 0) return 1;
        close(fd);
        std::puts("ok");
        return 0;
    }

    if (cmd == "exec") {
        if (argc < 3) { std::fprintf(stderr, "missing argument\n"); return 1; }
        std::string src;
        if (!std::strcmp(argv[2], "-s")) {
            if (argc < 4) { std::fprintf(stderr, "-s requires code\n"); return 1; }
            src = argv[3];
        } else {
            src = read_file(argv[2]);
        }
        return send_script(src);
    }

    std::fprintf(stderr, "unknown command: %s\n", cmd.c_str());
    return 1;
}
