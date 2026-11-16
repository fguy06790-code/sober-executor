#pragma once

#include <string>

namespace sober::gui {

// Blocking: connects to the payload's unix socket and sends one script.
// Returns true on send success. Does NOT read a response; the payload
// writes errors to its own stderr (visible in Sober's log).
bool send_script(const std::string& src, std::string& error_out);

// True if the payload socket exists and connect() succeeds.
bool is_attached();

// Returns the resolved socket path the loader + payload agree on.
std::string socket_path();

} // namespace sober::gui
