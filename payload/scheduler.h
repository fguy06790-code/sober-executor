#pragma once

#include <string>

bool resolve_luau_api();

// Queue a source chunk to be compiled + executed on the next scheduler tick
// in the elevated thread.
void enqueue_script(const std::string& src);

// Called from the scheduler hook on the main client thread.
void drain_pending(void);
