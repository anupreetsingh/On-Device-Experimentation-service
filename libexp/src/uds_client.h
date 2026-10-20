// The client side of expd's Unix domain socket, shared by libexp's slow path and the tools.
#pragma once

#include <string>
#include <string_view>

namespace uds {

// Connects to the socket at `path`. The socket gets 2 s send and receive timeouts and never
// raises SIGPIPE, so a dead expd can't hang or kill the app. Returns -1 with errno set.
int connect_to(const std::string& path);

// Sends one frame holding `payload`, blocking until it's all sent.
bool send_frame(int fd, std::string_view payload);

// Reads exactly one frame.
bool read_frame(int fd, std::string* payload);

// send_frame, then read_frame for the reply.
bool request(int fd, std::string_view payload, std::string* reply);

// Flags that keep send() from raising SIGPIPE where the platform has MSG_NOSIGNAL.
int no_sigpipe_flags();

}  // namespace uds
