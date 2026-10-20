#include "server.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>

#include "frame.h"
#include "uds_client.h"

#ifdef __linux__
#include <sys/un.h>
#endif

namespace expd {
namespace {

// A client that stops reading its replies gets disconnected once this much is waiting.
constexpr size_t kMaxPendingOut = 1 << 20;

uid_t peer_uid(int fd) {
#ifdef __linux__
  ucred cred{};
  socklen_t len = sizeof cred;
  if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0) return cred.uid;
#else
  uid_t uid;
  gid_t gid;
  if (getpeereid(fd, &uid, &gid) == 0) return uid;
#endif
  return uid_t(-1);
}

}  // namespace

Server::Server(EventLoop& loop, Assigner& assigner) : loop_(loop), assigner_(assigner) {}

Server::~Server() {
  for (auto& [fd, client] : clients_) {
    loop_.unwatch(fd);
    close(fd);
  }
  for (int fd : listeners_) {
    loop_.unwatch(fd);
    close(fd);
  }
}

void Server::listen_on(int listen_fd) {
  listeners_.push_back(listen_fd);
  loop_.watch_read(listen_fd, [this, listen_fd] { accept_from(listen_fd); });
}

void Server::accept_from(int listen_fd) {
  for (;;) {
    int fd = accept(listen_fd, nullptr, nullptr);
    if (fd < 0) {
      if (errno == EINTR) continue;
      return;  // EAGAIN: no one else is waiting
    }
    // Non-blocking, so one slow client can never stall the loop for everyone else.
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    Client client;
    client.uid = peer_uid(fd);
    clients_.emplace(fd, std::move(client));
    loop_.watch_read(fd, [this, fd] { read_from(fd); });
  }
}

void Server::read_from(int fd) {
  auto it = clients_.find(fd);
  if (it == clients_.end()) return;
  Client& client = it->second;

  // One read per wakeup. If more is waiting, level triggering calls us again.
  char buf[16384];
  ssize_t n = read(fd, buf, sizeof buf);
  if (n == 0) {  // the client closed the connection or exited
    disconnect(fd);
    return;
  }
  if (n < 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) disconnect(fd);
    return;
  }
  client.in.append(buf, size_t(n));

  // Handle every whole frame. A partial one waits in `in` for the rest of its bytes, without
  // holding up any other client.
  size_t used = 0;
  for (;;) {
    std::string_view payload;
    size_t frame_len = 0;
    int r = frame::parse(std::string_view(client.in).substr(used), &payload, &frame_len);
    if (r < 0) {
      disconnect(fd);
      return;
    }
    if (r == 0) break;
    handle(client, payload);
    used += frame_len;
  }
  client.in.erase(0, used);
  if (!client.out.empty()) write_to(fd);
}

void Server::write_to(int fd) {
  auto it = clients_.find(fd);
  if (it == clients_.end()) return;
  Client& client = it->second;

  while (!client.out.empty()) {
    ssize_t n = send(fd, client.out.data(), client.out.size(), uds::no_sigpipe_flags());
    if (n > 0) {
      client.out.erase(0, size_t(n));
    } else if (n < 0 && errno == EINTR) {
      continue;
    } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      break;
    } else {
      disconnect(fd);
      return;
    }
  }

  if (client.out.empty()) {
    if (client.watching_write) {
      loop_.unwatch_write(fd);
      client.watching_write = false;
    }
  } else if (client.out.size() > kMaxPendingOut) {
    disconnect(fd);
  } else if (!client.watching_write) {
    // The client's receive buffer is full. Finish when its socket is writable again.
    loop_.watch_write(fd, [this, fd] { write_to(fd); });
    client.watching_write = true;
  }
}

void Server::handle(Client& client, std::string_view request) {
  ++requests_;
  size_t space = request.find_first_of(" \n");
  std::string_view command = request.substr(0, space);
  std::string_view arg = space == std::string_view::npos ? std::string_view() : request.substr(space + 1);
  std::string reply;

  if (command == "GET") {
    const std::string* variant = assigner_.variant(arg);
    reply = variant != nullptr ? "OK " + *variant : "NONE";
  } else if (command == "STATS") {
    reply = "OK experiments=" + std::to_string(assigner_.experiment_count()) +
            " generation=" + std::to_string(assigner_.generation()) +
            " clients=" + std::to_string(clients_.size()) +
            " requests=" + std::to_string(requests_);
  } else {
    reply = "ERR unknown command";
  }
  frame::append(&client.out, reply);
}

void Server::disconnect(int fd) {
  loop_.unwatch(fd);
  close(fd);
  clients_.erase(fd);
}

}  // namespace expd
