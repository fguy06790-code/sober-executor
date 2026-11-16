#include "scheduler.h"

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

bool resolve_luau_api();
void start_drain_thread();
void start_ipc_server();

__attribute__((constructor))
static void payload_entry() {
    std::fprintf(stderr, "[payload] loaded, pid=%d\n", (int)getpid());

    if (!resolve_luau_api()) {
        std::fprintf(stderr, "[payload] signature set out of date — refresh kSigs in luau_api.cpp\n");
        // Continue anyway; IPC still accepts scripts and we'll retry on each drain.
    } else {
        std::fprintf(stderr, "[payload] luau api resolved\n");
    }

    start_drain_thread();
    start_ipc_server();
}
