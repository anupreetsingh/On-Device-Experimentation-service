#include "exp.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>

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

// A lazily opened connection to expd for the slow path. The lock makes callers take turns.
struct Connection {
  std::mutex mu;
  int fd = -1;

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
};

Connection g_requests;

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
  return new Mapping{base, size, entries, count, generation};
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

int variant_via_socket(const char* experiment, char* out, size_t out_len) {
  std::string reply;
  if (!g_requests.call(std::string("GET ") + experiment, &reply)) return -1;
  if (reply.rfind("OK ", 0) != 0) return -1;  // NONE: not in the experiment
  return copy_out(reply.c_str() + 3, out, out_len);
}

}  // namespace

extern "C" int exp_variant(const char* experiment, char* out, size_t out_len) {
  if (experiment == nullptr || out == nullptr) return -1;
  std::call_once(g_init_once, init);

  // Fast path: a binary search over the mapped snapshot, with no syscall and no lock.
  const Mapping* m = g_current.load(std::memory_order_acquire);
  if (m != nullptr) {
    const snapshot::Entry* entry = snapshot::find(m->entries, m->count, experiment);
    if (entry == nullptr) return -1;  // no such experiment, or it was turned off
    return copy_out(entry->variant, out, out_len);
  }

  // Slow path: no snapshot yet (expd hasn't published, or this app can't read it). expd
  // computes the same answer; this costs a round trip over the socket.
  return variant_via_socket(experiment, out, out_len);
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
  return variant_via_socket(experiment, out, out_len);
}
