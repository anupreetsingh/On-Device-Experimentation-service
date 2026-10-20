// exp_bench: times libexp's two read paths against a running expd.
//
//   exp_bench [experiment]
//
// Fast path: exp_variant() on the mapped snapshot. Slow path: the same lookup as a GET over
// expd's socket. Each runs a warm-up first, so mapping the file and the first-touch page
// faults aren't counted, then reports the mean over many calls.
#include <chrono>
#include <cstdio>

#include "exp.h"
#include "libexp_internal.h"

namespace {

template <typename F>
double mean_ns(int iterations, F&& f) {
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < iterations; ++i) f();
  auto elapsed = std::chrono::steady_clock::now() - start;
  return std::chrono::duration<double, std::nano>(elapsed).count() / iterations;
}

}  // namespace

int main(int argc, char** argv) {
  const char* experiment = argc > 1 ? argv[1] : "new_search_ranking";
  char out[EXP_VARIANT_MAX];

  exp_refresh();
  if (exp_snapshot_generation() == 0) {
    std::fprintf(stderr, "exp_bench: no snapshot mapped; is expd running with a config?\n");
    return 1;
  }
  if (exp_variant(experiment, out, sizeof out) != 0) {
    std::fprintf(stderr, "exp_bench: '%s' isn't in the snapshot\n", experiment);
    return 1;
  }
  if (exp_internal_variant_via_socket(experiment, out, sizeof out) != 0) {
    std::fprintf(stderr, "exp_bench: expd isn't answering on its socket\n");
    return 1;
  }

  // volatile: keeps the compiler from dropping lookups whose result is never used.
  volatile char sink = 0;
  auto fast = [&] {
    exp_variant(experiment, out, sizeof out);
    sink = out[0];
  };
  auto slow = [&] {
    exp_internal_variant_via_socket(experiment, out, sizeof out);
    sink = out[0];
  };

  mean_ns(100'000, fast);
  double fast_ns = mean_ns(10'000'000, fast);
  mean_ns(1'000, slow);
  double slow_ns = mean_ns(20'000, slow);
  (void)sink;

  std::printf("experiment:    %s = %s (snapshot generation %llu)\n", experiment, out,
              (unsigned long long)exp_snapshot_generation());
  std::printf("mmap snapshot: %8.1f ns per lookup\n", fast_ns);
  std::printf("socket (GET):  %8.1f ns per lookup (%.1f us)\n", slow_ns, slow_ns / 1000);
  std::printf("ratio:         %8.0fx\n", slow_ns / fast_ns);
  return 0;
}
