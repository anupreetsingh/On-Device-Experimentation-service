#include "uds_client.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

#include "frame.h"

namespace uds {

int no_sigpipe_flags() {
#ifdef MSG_NOSIGNAL
  return MSG_NOSIGNAL;
#else
  return 0;
#endif
}

int connect_to(const std::string& path) {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof addr.sun_path) {
    errno = ENAMETOOLONG;
    return -1;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
  // macOS: writing to a socket expd has closed returns EPIPE instead of killing the app.
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
  timeval timeout{2, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof timeout);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);

  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
  }
  return fd;
}

bool send_frame(int fd, std::string_view payload) {
  std::string bytes;
  frame::append(&bytes, payload);
  size_t sent = 0;
  while (sent < bytes.size()) {
    ssize_t n = send(fd, bytes.data() + sent, bytes.size() - sent, no_sigpipe_flags());
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;
    sent += size_t(n);
  }
  return true;
}

bool read_frame(int fd, std::string* payload) {
  std::string buf;
  char chunk[4096];
  for (;;) {
    std::string_view found;
    size_t frame_len = 0;
    int r = frame::parse(buf, &found, &frame_len);
    if (r < 0) return false;
    if (r > 0) {
      payload->assign(found);
      return frame_len == buf.size();  // expd sends one reply per request, never more
    }
    ssize_t n = recv(fd, chunk, sizeof chunk, 0);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return false;  // closed, error, or timed out
    buf.append(chunk, size_t(n));
  }
}

bool request(int fd, std::string_view payload, std::string* reply) {
  return send_frame(fd, payload) && read_frame(fd, reply);
}

}  // namespace uds
