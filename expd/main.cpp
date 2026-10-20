// expd, the experiment-assignment daemon. launchd (or systemd) starts it on the first
// connection to its socket. Run by hand, it creates the socket itself.
#include <sys/stat.h>

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "activation.h"
#include "assigner.h"
#include "device_id.h"
#include "event_loop.h"
#include "exposure_log.h"
#include "locations.h"
#include "server.h"

int main() {
  // A client that hangs up mid-reply must not kill expd.
  std::signal(SIGPIPE, SIG_IGN);

  std::string dir = locations::state_dir();
  if (mkdir(dir.c_str(), 0700) != 0 && errno != EEXIST) {
    std::fprintf(stderr, "expd: mkdir %s: %s\n", dir.c_str(), std::strerror(errno));
    return 1;
  }

  std::string error;
  std::string device_id = expd::load_or_create_device_id(dir + "/device_id", &error);
  if (device_id.empty()) {
    std::fprintf(stderr, "expd: device id: %s\n", error.c_str());
    return 1;
  }

  expd::EventLoop loop;
  if (!loop.ok()) {
    std::perror("expd: event loop");
    return 1;
  }

  expd::Assigner assigner(dir, device_id);
  if (!assigner.load(&error)) {
    // Keep serving: every lookup gets NONE, and apps use their defaults.
    std::fprintf(stderr, "expd: no usable config: %s\n", error.c_str());
  }

  std::vector<int> listeners = expd::activated_listeners();
  if (listeners.empty()) {
    std::string path = locations::socket_path();
    int fd = expd::listen_unix(path, &error);
    if (fd < 0) {
      std::fprintf(stderr, "expd: %s\n", error.c_str());
      return 1;
    }
    listeners.push_back(fd);
    std::fprintf(stderr, "expd: not socket-activated; listening on %s\n", path.c_str());
  }

  // launchd stops expd with SIGTERM, then SIGKILL after ExitTimeOut. Handle it as an event on
  // the loop, not inside a signal handler. Before any thread starts: on Linux the writer
  // thread must inherit the blocked-signal mask.
  loop.watch_signal(SIGTERM, [&loop] { loop.stop(); });
  loop.watch_signal(SIGINT, [&loop] { loop.stop(); });

  expd::ExposureLog::Options options;
  options.path = dir + "/exposures.log";
  if (const char* ms = std::getenv("EXPD_FLUSH_INTERVAL_MS")) {  // for demos and measurements
    options.interval = std::chrono::milliseconds(std::strtol(ms, nullptr, 10));
    options.leeway = options.interval / 6;
  }
  expd::ExposureLog exposures(options);
  if (!exposures.start(&error)) {
    std::fprintf(stderr, "expd: exposure log: %s\n", error.c_str());
    return 1;
  }

  expd::Server server(loop, assigner, &exposures);
  for (int fd : listeners) server.listen_on(fd);

  std::fprintf(stderr, "expd: serving %zu experiments (snapshot generation %llu)\n",
               assigner.experiment_count(), (unsigned long long)assigner.generation());
  loop.run();

  // Flush the exposures still in memory before exiting.
  exposures.stop();
  expd::ExposureLog::Stats stats = exposures.stats();
  std::fprintf(stderr, "expd: stopping; %llu exposures written, %llu dropped, %llu writer wakeups\n",
               (unsigned long long)stats.written, (unsigned long long)stats.dropped,
               (unsigned long long)stats.flushes);
  return 0;
}
