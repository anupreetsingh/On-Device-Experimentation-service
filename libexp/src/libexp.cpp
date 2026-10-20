#include "exp.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "frame.h"
#include "libexp_internal.h"
#include "locations.h"
#include "snapshot_format.h"
#include "uds_client.h"

#ifdef __APPLE__
#include <dispatch/dispatch.h>
#include <notify.h>
#elif defined(__linux__)
#include <sys/inotify.h>
#endif

namespace {

// One mapped snapshot file. Mappings are never unmapped once lookups can see them: another
// thread may still be mid-lookup in an old one, and a snapshot is a few KB that changes
// about once a day, so keeping old ones until exit costs almost nothing.
struct Mapping {
  void* base;
  size_t size;
  const snapshot::Entry* entries;
  uint32_t count;
  uint64_t generation;
  // One flag per entry: set once this process has sent the entry's exposure. A new snapshot
  // starts with fresh flags, so each experiment logs again after a remap.
  std::unique_ptr<std::atomic<bool>[]> exposed;
};

// The only shared state a lookup touches. Remaps store with release and lookups load with
// acquire, so a thread that sees a new pointer also sees the mapping it points to fully set up.
std::atomic<const Mapping*> g_current{nullptr};

std::once_flag g_init_once;
std::mutex g_remap_mu;  // serializes remaps; lookups never take it

#ifdef __APPLE__
int g_snapshot_token = -1;
int g_kill_token = -1;
#elif defined(__linux__)
int g_inotify_fd = -1;
bool g_watching = false;
#endif

// A lazily opened connection to expd. The lock makes callers take turns; lookups on the
// fast path never use one.
struct Connection {
  std::mutex mu;
  int fd = -1;

  // Sends a request and waits for the reply (up to the socket's 2 s timeout).
  bool call(const std::string& request, std::string* reply) {
    std::lock_guard<std::mutex> lock(mu);
    // A second try on a fresh connection, in case expd restarted since the last call.
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (fd < 0 && (fd = uds::connect_to(locations::socket_path())) < 0) return false;
      if (uds::request(fd, request, reply)) return true;
      close(fd);
      fd = -1;
    }
    return false;
  }

  // Sends a message that gets no reply, without blocking. If the socket buffer is full, the
  // message is dropped: an exposure is never worth stalling the app.
  bool post(const std::string& message) {
    std::string bytes;
    frame::append(&bytes, message);
    std::lock_guard<std::mutex> lock(mu);
    for (int attempt = 0; attempt < 2; ++attempt) {
      if (fd < 0 && (fd = uds::connect_to(locations::socket_path())) < 0) return false;
      ssize_t n = send(fd, bytes.data(), bytes.size(), MSG_DONTWAIT | uds::no_sigpipe_flags());
      if (n == ssize_t(bytes.size())) return true;
      if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return false;  // nothing sent
      // Part of a frame went out, or expd is gone. Close, so expd discards the partial frame
      // when it sees the connection end; on a dead connection, try once more on a new one.
      close(fd);
      fd = -1;
      if (n > 0) return false;
    }
    return false;
  }
};

Connection g_requests;   // slow-path GETs
Connection g_exposures;  // EXPOSE messages, on their own connection so they never wait behind a GET

// Exposures already sent from the slow path, which has no mapping to keep flags in.
std::mutex g_slow_exposed_mu;
std::set<std::string> g_slow_exposed;

// open + mmap + validate. The fd can be closed right after mmap(): the mapping holds its own
// reference to the file, so it stays readable even after expd renames a new one over it.
const Mapping* map_snapshot() {
  int fd = open(locations::snapshot_path().c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) return nullptr;
  struct stat st;
  if (fstat(fd, &st) != 0 || st.st_size <= 0) {
    close(fd);
    return nullptr;
  }
  size_t size = size_t(st.st_size);
  void* base = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
  close(fd);
  if (base == MAP_FAILED) return nullptr;

  // Checked once here, so lookups never re-check. A bad file means defaults, not a crash.
  uint32_t count = 0;
  uint64_t generation = 0;
  const snapshot::Entry* entries = snapshot::validate(base, size, &count, &generation);
  if (entries == nullptr) {
    munmap(base, size);
    return nullptr;
  }
  return new Mapping{base, size, entries, count, generation,
                     std::make_unique<std::atomic<bool>[]>(count)};
}

// Maps the current file and, if expd published it after the one lookups see, switches them
// to it. Any other generation counts as newer: if expd's state is wiped, it restarts at 1.
// Caller holds g_remap_mu.
bool remap_locked() {
  const Mapping* fresh = map_snapshot();
  if (fresh == nullptr) return false;
  const Mapping* current = g_current.load(std::memory_order_relaxed);
  if (current != nullptr && fresh->generation == current->generation) {
    munmap(fresh->base, fresh->size);  // no lookup has seen it, so it can go
    delete fresh;
    return false;
  }
  g_current.store(fresh, std::memory_order_release);
  return true;
}

bool remap() {
  std::lock_guard<std::mutex> lock(g_remap_mu);
  return remap_locked();
}

#ifdef __linux__
// True if expd renamed a new snapshot into place since the last call.
bool snapshot_replaced() {
  if (!g_watching) return true;  // couldn't watch the directory: just look
  alignas(inotify_event) char buf[4096];
  bool seen = false;
  ssize_t n;
  while ((n = read(g_inotify_fd, buf, sizeof buf)) > 0) {
    for (char* p = buf; p < buf + n;) {
      auto* event = reinterpret_cast<inotify_event*>(p);
      if ((event->mask & IN_Q_OVERFLOW) ||
          (event->len > 0 && std::strcmp(event->name, locations::kSnapshotFile) == 0)) {
        seen = true;
      }
      p += sizeof(inotify_event) + event->len;
    }
  }
  return seen;
}
#endif

void init() {
#ifdef __APPLE__
  // notify_check() on this token is a cheap read of shared memory, done only in exp_refresh().
  notify_register_check(locations::kSnapshotNotification, &g_snapshot_token);
  // A kill switch can't wait for a safe point: remap as soon as expd posts it.
  notify_register_dispatch(locations::kKillSwitchNotification, &g_kill_token,
                           dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^(int) {
                             remap();
                           });
#elif defined(__linux__)
  // rename() into the directory shows up as IN_MOVED_TO for the name "snapshot".
  g_inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
  g_watching = g_inotify_fd >= 0 &&
               inotify_add_watch(g_inotify_fd, locations::state_dir().c_str(),
                                 IN_MOVED_TO | IN_CLOSE_WRITE) >= 0;
#endif
  remap();
}

int copy_out(const char* variant, char* out, size_t out_len) {
  size_t len = std::strlen(variant);
  if (len + 1 > out_len) return -1;
  std::memcpy(out, variant, len + 1);
  return 0;
}

bool variant_via_socket(const char* experiment, std::string* variant) {
  std::string reply;
  if (!g_requests.call(std::string("GET ") + experiment, &reply)) return false;
  if (reply.rfind("OK ", 0) != 0) return false;  // NONE: not in the experiment
  *variant = reply.substr(3);
  return true;
}

void send_exposure(const char* experiment, const char* variant) {
  g_exposures.post(std::string("EXPOSE ") + experiment + " " + variant);
}

int lookup(const char* experiment, char* out, size_t out_len, bool log_exposure) {
  if (experiment == nullptr || out == nullptr) return -1;
  std::call_once(g_init_once, init);

  // Fast path: a binary search over the mapped snapshot, with no syscall and no lock.
  const Mapping* m = g_current.load(std::memory_order_acquire);
  if (m != nullptr) {
    const snapshot::Entry* entry = snapshot::find(m->entries, m->count, experiment);
    if (entry == nullptr) return -1;  // no such experiment, or it was turned off
    if (log_exposure) {
      // After the first time, this is one relaxed load. Only the thread that flips the flag
      // sends, so each experiment logs once per snapshot even when threads race.
      std::atomic<bool>& sent = m->exposed[size_t(entry - m->entries)];
      if (!sent.load(std::memory_order_relaxed) && !sent.exchange(true, std::memory_order_relaxed)) {
        send_exposure(entry->experiment, entry->variant);
      }
    }
    return copy_out(entry->variant, out, out_len);
  }

  // Slow path: no snapshot yet (expd hasn't published, or this app can't read it). expd
  // computes the same answer; this costs a round trip over the socket.
  std::string variant;
  if (!variant_via_socket(experiment, &variant)) return -1;
  if (log_exposure) {
    bool first;
    {
      std::lock_guard<std::mutex> lock(g_slow_exposed_mu);
      first = g_slow_exposed.insert(std::string(experiment) + " " + variant).second;
    }
    if (first) send_exposure(experiment, variant.c_str());
  }
  return copy_out(variant.c_str(), out, out_len);
}

}  // namespace

extern "C" int exp_variant(const char* experiment, char* out, size_t out_len) {
  return lookup(experiment, out, out_len, /*log_exposure=*/true);
}

extern "C" int exp_peek_variant(const char* experiment, char* out, size_t out_len) {
  return lookup(experiment, out, out_len, /*log_exposure=*/false);
}

extern "C" void exp_log_exposure(const char* experiment) {
  char variant[EXP_VARIANT_MAX];
  lookup(experiment, variant, sizeof variant, /*log_exposure=*/true);
}

extern "C" int exp_refresh(void) {
  std::call_once(g_init_once, init);
  std::lock_guard<std::mutex> lock(g_remap_mu);
  bool look = g_current.load(std::memory_order_relaxed) == nullptr;
#ifdef __APPLE__
  int posted = 0;
  if (notify_check(g_snapshot_token, &posted) == NOTIFY_STATUS_OK && posted) look = true;
#elif defined(__linux__)
  if (snapshot_replaced()) look = true;
#else
  look = true;
#endif
  return look && remap_locked() ? 1 : 0;
}

extern "C" uint64_t exp_snapshot_generation(void) {
  std::call_once(g_init_once, init);
  const Mapping* m = g_current.load(std::memory_order_acquire);
  return m != nullptr ? m->generation : 0;
}

extern "C" int exp_internal_variant_via_socket(const char* experiment, char* out, size_t out_len) {
  if (experiment == nullptr || out == nullptr) return -1;
  std::string variant;
  if (!variant_via_socket(experiment, &variant)) return -1;
  return copy_out(variant.c_str(), out, out_len);
}
