#pragma once

#include <string>
#include <vector>

namespace expd {

// The listening sockets launchd (macOS) or systemd (Linux) created and handed to expd when
// a client first connected, made non-blocking. Empty if expd wasn't socket-activated.
std::vector<int> activated_listeners();

// Creates a non-blocking listening Unix domain socket at `path` (mode 0600), for when expd is
// run by hand. Fails if another expd is already answering there. Returns -1 on failure.
int listen_unix(const std::string& path, std::string* error);

}  // namespace expd
