#include "common.h"

#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <sys/uio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cerrno>

namespace sober {

static bool attach(pid_t pid) {
    if (ptrace(PTRACE_ATTACH, pid, 0, 0) < 0) {
        std::perror("ptrace attach");
        return false;
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) return false;
    return WIFSTOPPED(status);
}

static void detach(pid_t pid) {
    ptrace(PTRACE_DETACH, pid, 0, 0);
}

static bool read_regs(pid_t pid, user_regs_struct& r) {
    return ptrace(PTRACE_GETREGS, pid, 0, &r) == 0;
}

static bool write_regs(pid_t pid, const user_regs_struct& r) {
    return ptrace(PTRACE_SETREGS, pid, 0, &r) == 0;
}

static bool peek(pid_t pid, uintptr_t a, void* dst, size_t n) {
    for (size_t i = 0; i < n; i += sizeof(long)) {
        errno = 0;
        long w = ptrace(PTRACE_PEEKDATA, pid, a + i, 0);
        if (errno) return false;
        size_t cp = (n - i < sizeof(long)) ? n - i : sizeof(long);
        std::memcpy((uint8_t*)dst + i, &w, cp);
    }
    return true;
}

static bool poke(pid_t pid, uintptr_t a, const void* src, size_t n) {
    for (size_t i = 0; i < n; i += sizeof(long)) {
        long w = 0;
        size_t cp = (n - i < sizeof(long)) ? n - i : sizeof(long);
        if (cp < sizeof(long)) {
            errno = 0;
            w = ptrace(PTRACE_PEEKDATA, pid, a + i, 0);
            if (errno) return false;
        }
        std::memcpy(&w, (const uint8_t*)src + i, cp);
        if (ptrace(PTRACE_POKEDATA, pid, a + i, w) < 0) return false;
    }
    return true;
}

// Resolve the address of a libc symbol inside the remote process by walking
// its /proc/pid/maps for libc, and reading the local copy's offset.
static uintptr_t remote_libc_sym(pid_t pid, const char* sym) {
    auto regs = read_maps(pid);
    std::string path;
    uintptr_t remote_base = 0;
    for (const auto& r : regs) {
        if (r.path.find("libc.so") != std::string::npos ||
            r.path.find("libc-") != std::string::npos) {
            if (r.perms.find('x') != std::string::npos && !remote_base) {
                remote_base = r.start;
                path = r.path;
                break;
            }
        }
    }
    if (!remote_base) return 0;

    void* h = dlopen(path.c_str(), RTLD_LAZY | RTLD_NOLOAD);
    if (!h) h = dlopen(path.c_str(), RTLD_LAZY);
    if (!h) return 0;
    void* local_sym = dlsym(h, sym);
    if (!local_sym) return 0;

    // Find the local base of the same libc from /proc/self/maps.
    uintptr_t local_base = 0;
    FILE* f = std::fopen("/proc/self/maps", "r");
    if (!f) return 0;
    char line[4096];
    while (std::fgets(line, sizeof(line), f)) {
        if (std::strstr(line, path.c_str())) {
            uintptr_t s, e;
            char perms[8];
            if (std::sscanf(line, "%lx-%lx %7s", &s, &e, perms) == 3) {
                if (std::strchr(perms, 'x')) { local_base = s; break; }
            }
        }
    }
    std::fclose(f);
    if (!local_base) return 0;

    return remote_base + ((uintptr_t)local_sym - local_base);
}

// Call a remote function with up to 6 args. Returns rax.
static uintptr_t remote_call(pid_t pid, uintptr_t fn, uintptr_t a1, uintptr_t a2,
                             uintptr_t a3 = 0, uintptr_t a4 = 0) {
    user_regs_struct saved, r;
    if (!read_regs(pid, saved)) return 0;
    r = saved;

    r.rdi = a1;
    r.rsi = a2;
    r.rdx = a3;
    r.rcx = a4;
    r.rax = fn;
    r.rip = fn;

    // Push a sentinel return address so the callee hits an INT3 we plant at rip-1.
    // Easier: place an INT3 after the call via PTRACE_SINGLESTEP tracking.
    uintptr_t stack = (saved.rsp - 0x100) & ~0xFULL;
    uintptr_t ret_addr = stack;
    r.rsp = stack - 8;

    // Write an INT3 (0xCC) at ret_addr, push ret_addr onto stack.
    long orig;
    errno = 0;
    orig = ptrace(PTRACE_PEEKDATA, pid, ret_addr, 0);
    long trap = (orig & ~0xFFL) | 0xCC;
    ptrace(PTRACE_POKEDATA, pid, ret_addr, trap);
    ptrace(PTRACE_POKEDATA, pid, r.rsp, ret_addr);

    write_regs(pid, r);
    ptrace(PTRACE_CONT, pid, 0, 0);

    int status;
    waitpid(pid, &status, 0);

    user_regs_struct after;
    read_regs(pid, after);

    // Restore
    ptrace(PTRACE_POKEDATA, pid, ret_addr, orig);
    write_regs(pid, saved);

    return after.rax;
}

bool inject_library(pid_t pid, const std::string& so_path) {
    if (!attach(pid)) return false;

    uintptr_t mmap_fn  = remote_libc_sym(pid, "mmap");
    uintptr_t dlopen_f = remote_libc_sym(pid, "__libc_dlopen_mode");
    if (!dlopen_f) dlopen_f = remote_libc_sym(pid, "dlopen");

    if (!mmap_fn || !dlopen_f) {
        std::fprintf(stderr, "symbol resolution failed (mmap=%p dlopen=%p)\n",
                     (void*)mmap_fn, (void*)dlopen_f);
        detach(pid);
        return false;
    }

    // Allocate a page in the remote for the path string.
    uintptr_t page = remote_call(pid, mmap_fn, 0, 0x1000);
    if (!page || page == (uintptr_t)-1) {
        detach(pid);
        return false;
    }

    std::string path = so_path;
    poke(pid, page, path.c_str(), path.size() + 1);

    // RTLD_NOW | RTLD_GLOBAL = 0x102 (for __libc_dlopen_mode: 2 is RTLD_NOW)
    uintptr_t handle = remote_call(pid, dlopen_f, page, 2);
    detach(pid);

    if (!handle) {
        std::fprintf(stderr, "remote dlopen returned null\n");
        return false;
    }
    std::fprintf(stderr, "payload loaded at handle 0x%lx\n", handle);
    return true;
}

} // namespace sober
