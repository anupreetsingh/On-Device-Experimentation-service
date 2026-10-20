#include "event_loop.h"

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>

#if defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/event.h>
#define EXPD_KQUEUE 1
#elif defined(__linux__)
#include <pthread.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#else
#error "expd needs kqueue or epoll"
#endif

namespace expd {
namespace {

void make_nonblocking(int fd) {
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
  fcntl(fd, F_SETFD, FD_CLOEXEC);
}

#ifdef EXPD_KQUEUE
void change(int kq, uintptr_t ident, int16_t filter, uint16_t flags) {
  struct kevent ev;
  EV_SET(&ev, ident, filter, flags, 0, 0, nullptr);
  // Deleting a filter that isn't registered fails with ENOENT, which is fine.
  kevent(kq, &ev, 1, nullptr, 0, nullptr);
}
#endif

}  // namespace

EventLoop::EventLoop() {
#ifdef EXPD_KQUEUE
  poll_fd_ = kqueue();
  if (poll_fd_ >= 0) fcntl(poll_fd_, F_SETFD, FD_CLOEXEC);
#else
  poll_fd_ = epoll_create1(EPOLL_CLOEXEC);
#endif
  if (poll_fd_ < 0) return;

  // stop() writes a byte here, which wakes the loop from another thread.
  if (pipe(wake_pipe_) != 0) {
    close(poll_fd_);
    poll_fd_ = -1;
    return;
  }
  make_nonblocking(wake_pipe_[0]);
  make_nonblocking(wake_pipe_[1]);
  watch_read(wake_pipe_[0], [this] {
    char buf[64];
    while (read(wake_pipe_[0], buf, sizeof buf) > 0) {
    }
  });
}

EventLoop::~EventLoop() {
  for (int fd : signal_fds_) close(fd);
  for (int fd : wake_pipe_) {
    if (fd >= 0) close(fd);
  }
  if (poll_fd_ >= 0) close(poll_fd_);
}

void EventLoop::dispatch(int fd, bool writable) {
  auto it = fds_.find(fd);
  if (it == fds_.end()) return;  // an earlier callback in this batch unwatched it
  // A copy, because the callback may unwatch (erase) its own fd.
  Callback callback = writable ? it->second.write : it->second.read;
  if (callback) callback();
}

void EventLoop::stop() {
  stopping_.store(true);
  char byte = 1;
  (void)!write(wake_pipe_[1], &byte, 1);
}

#ifdef EXPD_KQUEUE

void EventLoop::watch_read(int fd, Callback on_readable) {
  fds_[fd].read = std::move(on_readable);
  change(poll_fd_, uintptr_t(fd), EVFILT_READ, EV_ADD);
}

void EventLoop::watch_write(int fd, Callback on_writable) {
  fds_[fd].write = std::move(on_writable);
  change(poll_fd_, uintptr_t(fd), EVFILT_WRITE, EV_ADD);
}

void EventLoop::unwatch_write(int fd) {
  if (auto it = fds_.find(fd); it != fds_.end()) it->second.write = nullptr;
  change(poll_fd_, uintptr_t(fd), EVFILT_WRITE, EV_DELETE);
}

void EventLoop::unwatch(int fd) {
  change(poll_fd_, uintptr_t(fd), EVFILT_READ, EV_DELETE);
  change(poll_fd_, uintptr_t(fd), EVFILT_WRITE, EV_DELETE);
  fds_.erase(fd);
}

void EventLoop::watch_signal(int signo, Callback on_signal) {
  // The default action (terminate) would run before kqueue reports the signal. EVFILT_SIGNAL
  // still sees a signal that's ignored, so ignore it and handle the event instead.
  signal(signo, SIG_IGN);
  signals_[signo] = std::move(on_signal);
  change(poll_fd_, uintptr_t(signo), EVFILT_SIGNAL, EV_ADD);
}

void EventLoop::run() {
  struct kevent events[64];
  while (!stopping_.load()) {
    int n = kevent(poll_fd_, nullptr, 0, events, 64, nullptr);
    if (n < 0) {
      if (errno == EINTR) continue;
      std::perror("expd: kevent");
      return;
    }
    for (int i = 0; i < n && !stopping_.load(); ++i) {
      int ident = int(events[i].ident);
      if (events[i].filter == EVFILT_SIGNAL) {
        if (auto it = signals_.find(ident); it != signals_.end()) {
          Callback callback = it->second;
          callback();
        }
        continue;
      }
      // A read event with EV_EOF set still goes to the read callback, whose read() returns 0.
      dispatch(ident, events[i].filter == EVFILT_WRITE);
    }
  }
}

#else  // epoll

void EventLoop::update(int fd) {
  auto it = fds_.find(fd);
  if (it == fds_.end()) return;
  epoll_event ev{};
  ev.data.fd = fd;
  ev.events = (it->second.read ? EPOLLIN : 0u) | (it->second.write ? EPOLLOUT : 0u);
  if (epoll_ctl(poll_fd_, EPOLL_CTL_MOD, fd, &ev) != 0 && errno == ENOENT) {
    epoll_ctl(poll_fd_, EPOLL_CTL_ADD, fd, &ev);
  }
}

void EventLoop::watch_read(int fd, Callback on_readable) {
  fds_[fd].read = std::move(on_readable);
  update(fd);
}

void EventLoop::watch_write(int fd, Callback on_writable) {
  fds_[fd].write = std::move(on_writable);
  update(fd);
}

void EventLoop::unwatch_write(int fd) {
  if (auto it = fds_.find(fd); it != fds_.end()) it->second.write = nullptr;
  update(fd);
}

void EventLoop::unwatch(int fd) {
  epoll_ctl(poll_fd_, EPOLL_CTL_DEL, fd, nullptr);
  fds_.erase(fd);
}

void EventLoop::watch_signal(int signo, Callback on_signal) {
  // epoll only watches fds, so the signal is blocked and read from a signalfd instead.
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, signo);
  pthread_sigmask(SIG_BLOCK, &mask, nullptr);
  int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
  if (sfd < 0) {
    std::perror("expd: signalfd");
    return;
  }
  signal_fds_.push_back(sfd);
  watch_read(sfd, [sfd, on_signal = std::move(on_signal)] {
    signalfd_siginfo info;
    while (read(sfd, &info, sizeof info) == ssize_t(sizeof info)) {
    }
    on_signal();
  });
}

void EventLoop::run() {
  epoll_event events[64];
  while (!stopping_.load()) {
    int n = epoll_wait(poll_fd_, events, 64, -1);
    if (n < 0) {
      if (errno == EINTR) continue;
      std::perror("expd: epoll_wait");
      return;
    }
    for (int i = 0; i < n && !stopping_.load(); ++i) {
      int fd = events[i].data.fd;
      uint32_t ready = events[i].events;
      if (ready & (EPOLLIN | EPOLLHUP | EPOLLERR)) dispatch(fd, false);
      if (ready & EPOLLOUT) dispatch(fd, true);
    }
  }
}

#endif

}  // namespace expd
