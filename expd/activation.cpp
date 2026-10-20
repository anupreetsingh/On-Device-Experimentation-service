#include "activation.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>

#ifdef __APPLE__
#include <launch.h>
#endif

namespace expd {

std::vector<int> activated_listeners() {
  std::vector<int> fds;
#ifdef __APPLE__
  // launchd created and bound the socket from the plist's Sockets > Listeners entry, held it
  // while expd wasn't running, and started expd when the first client connected.
  int* list = nullptr;
  size_t count = 0;
  if (launch_activate_socket("Listeners", &list, &count) == 0) {
    fds.assign(list, list + count);
    std::free(list);
  }
#elif defined(__linux__)
  // systemd's socket-passing protocol, which sd_listen_fds() implements: LISTEN_PID names the
  // process the sockets are for, LISTEN_FDS how many there are, starting at fd 3.
  const char* pid = std::getenv("LISTEN_PID");
  const char* count = std::getenv("LISTEN_FDS");
  if (pid != nullptr && count != nullptr && std::strtol(pid, nullptr, 10) == getpid()) {
    int n = int(std::strtol(count, nullptr, 10));
    for (int fd = 3; fd < 3 + n; ++fd) {
      fcntl(fd, F_SETFD, FD_CLOEXEC);
      fds.push_back(fd);
    }
  }
  unsetenv("LISTEN_PID");
  unsetenv("LISTEN_FDS");
  unsetenv("LISTEN_FDNAMES");
#endif
  for (int fd : fds) fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  return fds;
}

int listen_unix(const std::string& path, std::string* error) {
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  if (path.size() >= sizeof addr.sun_path) {
    *error = "socket path too long: " + path;
    return -1;
  }
  std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);

  // A socket file left by an earlier expd makes bind() fail with EADDRINUSE, so remove it,
  // unless something is still accepting connections on it.
  int probe = socket(AF_UNIX, SOCK_STREAM, 0);
  if (probe >= 0) {
    bool alive = connect(probe, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0;
    close(probe);
    if (alive) {
      *error = "something is already listening on " + path;
      return -1;
    }
  }
  unlink(path.c_str());

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    *error = std::string("socket: ") + std::strerror(errno);
    return -1;
  }
  fcntl(fd, F_SETFD, FD_CLOEXEC);
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
      listen(fd, SOMAXCONN) != 0) {
    *error = "bind/listen " + path + ": " + std::strerror(errno);
    close(fd);
    return -1;
  }
  // Only this user's processes may connect. The state directory is 0700 as well.
  chmod(path.c_str(), 0600);
  return fd;
}

}  // namespace expd
