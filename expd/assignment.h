// Which variant this device gets in each experiment. Every device holds the same config and
// computes its own variant from a salted hash of its ID, so no server keeps per-device state
// and the answer survives reboots.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace expd {

constexpr uint32_t kBuckets = 10000;
constexpr size_t kMaxExperimentName = 47;
constexpr size_t kMaxVariantName = 15;
constexpr size_t kMaxSalt = 32;

struct Arm {
  std::string name;  // like "treatment"
  uint32_t weight;   // buckets out of 10000, like 5000 for 50%
};

struct Experiment {
  std::string name;       // like "new_search_ranking"
  std::string salt;       // like "k3f9"; changing it reshuffles every device
  std::vector<Arm> arms;  // arm i covers the arms[i].weight buckets after arm i-1's
};

struct Assignment {
  std::string experiment;
  std::string variant;
};

// bucket = SHA-256(salt, experiment, device_id) % 10000, with a NUL between the inputs so
// ("ab", "c") and ("a", "bc") hash differently. The same inputs give the same bucket on every
// platform and compiler, which std::hash doesn't promise.
uint32_t bucket_for(std::string_view salt, std::string_view experiment, std::string_view device_id);

// The arm whose bucket range holds `bucket`, or nullptr if the arms don't cover it.
const Arm* arm_for(const Experiment& experiment, uint32_t bucket);

// This device's variant in every experiment, sorted by experiment name.
std::vector<Assignment> assign(const std::vector<Experiment>& experiments, std::string_view device_id);

// Letters, digits, '_', '-' and '.', 1 to max_len characters.
bool valid_name(std::string_view name, size_t max_len);

// Parses a config: one experiment per line, `#` starts a comment.
//
//   # experiment        salt  arm:weight ...
//   new_search_ranking  k3f9  control:5000 treatment:5000
//
// Weights must add up to 10000. To ramp an arm up later without moving the devices already
// in it, list it last: its range grows downward from 10000, so it keeps every bucket it had.
bool parse_config(std::string_view text, std::vector<Experiment>* out, std::string* error);

}  // namespace expd
