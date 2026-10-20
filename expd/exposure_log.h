#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#else
#include <condition_variable>
#include <thread>
#endif

namespace expd {

// One exposure: this device ran the code that differs between variants.
struct Exposure {
  int64_t time;  // seconds since the epoch, when expd received it
  char experiment[48];
  char variant[16];
};

// A fixed-size circular buffer guarded by one mutex. The event loop pushes and the writer
// drains, each holding the lock only to copy a few bytes. It never grows and never makes the
// event loop wait: when full, the new exposure overwrites the oldest, and the drop is counted
// so the analysis knows data is missing.
class RingBuffer {
 public:
  enum class Push { kOk, kHighWater, kDropped };

  explicit RingBuffer(size_t capacity);

  // kHighWater once the buffer is 75% full, so the caller can ask for an early flush;
  // kDropped if it was full and the oldest exposure was overwritten.
  Push push(const Exposure& exposure);

  // Appends everything buffered to *batch, oldest first, and empties the buffer.
  void drain(std::vector<Exposure>* batch);

  size_t size() const;
  uint64_t dropped() const;

 private:
  mutable std::mutex mu_;
  std::vector<Exposure> slots_;
  size_t head_ = 0;   // the oldest exposure
  size_t count_ = 0;  // tail is (head_ + count_) % capacity
  uint64_t dropped_ = 0;
};

// Exposures from clients, appended to a log file by a writer that wakes on a coalesced timer
// (every `interval`, give or take `leeway`) or early when the buffer passes 75%. The disk
// write happens on the writer, never on the event loop.
class ExposureLog {
 public:
  struct Options {
    std::string path;
    size_t capacity = 4096;
    std::chrono::milliseconds interval{60'000};
    std::chrono::milliseconds leeway{10'000};
  };
  struct Stats {
    uint64_t recorded;
    uint64_t dropped;
    uint64_t flushes;  // writer wakeups
    uint64_t written;
  };

  explicit ExposureLog(Options options);
  ~ExposureLog();
  ExposureLog(const ExposureLog&) = delete;
  ExposureLog& operator=(const ExposureLog&) = delete;

  // Opens the log file and starts the writer.
  bool start(std::string* error);

  // Event-loop thread. Copies the exposure into the buffer and returns.
  void record(std::string_view experiment, std::string_view variant);

  // Flushes what's left and stops the writer. Called on SIGTERM; SIGKILL loses up to one
  // interval of exposures.
  void stop();

  Stats stats() const;

 private:
  void flush();  // writer only
  void request_early_flush();

  Options options_;
  RingBuffer ring_;
  int fd_ = -1;
  std::atomic<bool> running_{false};
  std::vector<Exposure> batch_;  // writer only
  std::atomic<bool> flush_pending_{false};
  std::atomic<uint64_t> recorded_{0};
  std::atomic<uint64_t> flushes_{0};
  std::atomic<uint64_t> written_{0};

#ifdef __APPLE__
  dispatch_queue_t queue_ = nullptr;
  dispatch_source_t timer_ = nullptr;
#else
  std::thread writer_;
  std::mutex wake_mu_;
  std::condition_variable wake_;
  bool stopping_ = false;  // guarded by wake_mu_
#endif
};

}  // namespace expd
