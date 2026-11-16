#include "ipc.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace sober::gui {

std::string socket_path() {
    const char* x = std::getenv("XDG_RUNTIME_DIR");
    if (x && *x) return std::string(x) + "/sober-exec.sock";
    return "/tmp/sober-exec.sock";
}

static int connect_once() {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_un a{};
    a.sun_family = AF_UNIX;
    std::string p = socket_path();
    std::strncpy(a.sun_path, p.c_str(), sizeof(a.sun_path) - 1);
    if (connect(fd, (sockaddr*)&a, sizeof(a)) < 0) { close(fd); return -1; }
    return fd;
}

bool is_attached() {
    int fd = connect_once();
    if (fd < 0) return false;
    close(fd);
    return true;
}

bool send_script(const std::string& src, std::string& error_out) {
    int fd = connect_once();
    if (fd < 0) { error_out = std::strerror(errno); return false; }
    uint32_t n = (uint32_t)src.size();
    if (write(fd, &n, 4) != 4) { error_out = "short header write"; close(fd); return false; }
    size_t w = 0;
    while (w < src.size()) {
        ssize_t r = write(fd, src.data() + w, src.size() - w);
        if (r <= 0) { error_out = std::strerror(errno); close(fd); return false; }
        w += r;
    }
    close(fd);
    return true;
}

} // namespace sober::gui
