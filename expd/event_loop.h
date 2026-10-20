#pragma once

#include <atomic>
#include <functional>
#include <unordered_map>
#include <vector>

namespace expd {

// One thread waits on every fd and signal expd cares about, and runs the callback for each
// one that's ready: kqueue on macOS, epoll on Linux. Level-triggered, so a callback that
// leaves bytes unread gets called again on the next wait. Every method except stop() must be
// called on the loop's thread.
class EventLoop {
 public:
  using Callback = std::function<void()>;

  EventLoop();
  ~EventLoop();
  EventLoop(const EventLoop&) = delete;
  EventLoop& operator=(const EventLoop&) = delete;

  bool ok() const { return poll_fd_ >= 0; }

  void watch_read(int fd, Callback on_readable);
  void watch_write(int fd, Callback on_writable);  // until unwatch_write()
  void unwatch_write(int fd);
  void unwatch(int fd);  // call before close(fd)

  // Runs `on_signal` on the loop thread when `signo` arrives, instead of in a signal handler,
  // so it can do anything the loop can. On Linux this blocks the signal for the calling
  // thread, so call it before starting other threads (they inherit the mask).
  void watch_signal(int signo, Callback on_signal);

  void run();   // returns after stop()
  void stop();  // safe from any thread

 private:
  struct Handlers {
    Callback read;
    Callback write;
  };
  void dispatch(int fd, bool writable);
  void update(int fd);  // epoll only: sync the fd's interest with its handlers

  int poll_fd_ = -1;  // the kqueue or epoll instance
  int wake_pipe_[2] = {-1, -1};
  std::atomic<bool> stopping_{false};
  std::unordered_map<int, Handlers> fds_;
  std::unordered_map<int, Callback> signals_;  // kqueue: by signal number
  std::vector<int> signal_fds_;                // epoll: one signalfd per signal
};

}  // namespace expd
