#include "exposure_log.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <ctime>

#ifdef __linux__
#include <sys/prctl.h>
#endif

namespace expd {

RingBuffer::RingBuffer(size_t capacity) : slots_(std::max<size_t>(capacity, 1)) {}

RingBuffer::Push RingBuffer::push(const Exposure& exposure) {
  std::lock_guard<std::mutex> lock(mu_);
  if (count_ == slots_.size()) {
    // Full: the tail has caught up with the head. Overwrite the oldest and move the head on.
    slots_[head_] = exposure;
    head_ = (head_ + 1) % slots_.size();
    ++dropped_;
    return Push::kDropped;
  }
  slots_[(head_ + count_) % slots_.size()] = exposure;
  ++count_;
  return count_ * 4 >= slots_.size() * 3 ? Push::kHighWater : Push::kOk;
}

void RingBuffer::drain(std::vector<Exposure>* batch) {
  std::lock_guard<std::mutex> lock(mu_);
  // Two copies at most: from head to the end of the array, then the part that wrapped.
  size_t first = std::min(count_, slots_.size() - head_);
  batch->insert(batch->end(), slots_.begin() + long(head_), slots_.begin() + long(head_ + first));
  batch->insert(batch->end(), slots_.begin(), slots_.begin() + long(count_ - first));
  head_ = 0;
  count_ = 0;
}

size_t RingBuffer::size() const {
  std::lock_guard<std::mutex> lock(mu_);
  return count_;
}

uint64_t RingBuffer::dropped() const {
  std::lock_guard<std::mutex> lock(mu_);
  return dropped_;
}

ExposureLog::ExposureLog(Options options) : options_(std::move(options)), ring_(options_.capacity) {}

ExposureLog::~ExposureLog() { stop(); }

bool ExposureLog::start(std::string* error) {
  fd_ = open(options_.path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd_ < 0) {
    *error = "open " + options_.path + ": " + std::strerror(errno);
    return false;
  }
  running_ = true;
  int64_t interval_ns = int64_t(options_.interval.count()) * 1'000'000;
  int64_t leeway_ns = int64_t(options_.leeway.count()) * 1'000'000;

#ifdef __APPLE__
  queue_ = dispatch_queue_create(
      "com.example.expd.exposures",
      dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
  timer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue_);
  dispatch_set_context(timer_, this);
  dispatch_source_set_event_handler_f(timer_, [](void* self) { static_cast<ExposureLog*>(self)->flush(); });
  // The leeway is what makes the timer coalesced: the kernel may fire it anywhere in
  // [interval, interval + leeway], alongside other timers that are already waking the CPU.
  dispatch_source_set_timer(timer_, dispatch_time(DISPATCH_TIME_NOW, interval_ns), uint64_t(interval_ns),
                            uint64_t(leeway_ns));
  dispatch_resume(timer_);
#else
  stopping_ = false;
  writer_ = std::thread([this, leeway_ns] {
    // Timer slack is Linux's leeway: the kernel may delay this thread's timed waits by up to
    // this much so they line up with other wakeups. It applies to the calling thread only.
    prctl(PR_SET_TIMERSLACK, (unsigned long)leeway_ns, 0, 0, 0);
    std::unique_lock<std::mutex> lock(wake_mu_);
    while (!stopping_) {
      wake_.wait_for(lock, options_.interval, [this] { return stopping_ || flush_pending_.load(); });
      lock.unlock();
      flush();
      lock.lock();
    }
    lock.unlock();
    flush();  // whatever arrived while the last flush was running
  });
#endif
  return true;
}

void ExposureLog::record(std::string_view experiment, std::string_view variant) {
  Exposure exposure{};
  exposure.time = int64_t(std::time(nullptr));
  std::memcpy(exposure.experiment, experiment.data(), std::min(experiment.size(), sizeof exposure.experiment - 1));
  std::memcpy(exposure.variant, variant.data(), std::min(variant.size(), sizeof exposure.variant - 1));
  recorded_.fetch_add(1, std::memory_order_relaxed);
  if (ring_.push(exposure) == RingBuffer::Push::kHighWater) request_early_flush();
}

void ExposureLog::request_early_flush() {
  // One early flush in flight at a time; the flush clears the flag when it starts.
  if (!running_.load() || flush_pending_.exchange(true)) return;
#ifdef __APPLE__
  dispatch_async_f(queue_, this, [](void* self) { static_cast<ExposureLog*>(self)->flush(); });
#else
  { std::lock_guard<std::mutex> lock(wake_mu_); }
  wake_.notify_one();
#endif
}

void ExposureLog::flush() {
  flush_pending_.store(false);
  flushes_.fetch_add(1, std::memory_order_relaxed);
  batch_.clear();
  ring_.drain(&batch_);  // holds the ring's lock only for the copy
  if (batch_.empty()) return;

  // The disk write happens with the lock released, so the event loop never waits on it.
  std::string text;
  text.reserve(batch_.size() * 48);
  for (const Exposure& e : batch_) {
    text += std::to_string(e.time);
    text += '\t';
    text += e.experiment;
    text += '\t';
    text += e.variant;
    text += '\n';
  }
  size_t done = 0;
  while (done < text.size()) {
    ssize_t n = write(fd_, text.data() + done, text.size() - done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return;  // disk full or similar: these exposures are lost, the next batch tries again
    done += size_t(n);
  }
  written_.fetch_add(batch_.size(), std::memory_order_relaxed);
}

void ExposureLog::stop() {
  if (!running_.exchange(false)) return;
#ifdef __APPLE__
  dispatch_source_cancel(timer_);
  // Serial queue: this runs after any flush already in progress, then nothing else does.
  dispatch_sync_f(queue_, this, [](void* self) { static_cast<ExposureLog*>(self)->flush(); });
  dispatch_release(timer_);
  dispatch_release(queue_);
  timer_ = nullptr;
  queue_ = nullptr;
#else
  {
    std::lock_guard<std::mutex> lock(wake_mu_);
    stopping_ = true;
  }
  wake_.notify_one();
  writer_.join();
#endif
  close(fd_);
  fd_ = -1;
}

ExposureLog::Stats ExposureLog::stats() const {
  return {recorded_.load(std::memory_order_relaxed), ring_.dropped(),
          flushes_.load(std::memory_order_relaxed), written_.load(std::memory_order_relaxed)};
}

}  // namespace expd
