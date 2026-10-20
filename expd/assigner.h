#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "assignment.h"

namespace expd {

// The daemon's view of experiments: the current config and this device's variant in each.
// Only the event-loop thread touches it, so it needs no lock.
class Assigner {
 public:
  Assigner(std::string state_dir, std::string device_id);

  // Loads the saved config (none means no experiments) and publishes a snapshot for it.
  bool load(std::string* error);

  // This device's variant in `experiment`, or nullptr if there's no such experiment.
  const std::string* variant(std::string_view experiment) const;

  size_t experiment_count() const { return assignments_.size(); }
  uint64_t generation() const { return generation_; }

 private:
  // Assigns, publishes a new snapshot, and only then makes `experiments` current.
  bool apply(std::vector<Experiment> experiments, std::string* error);

  std::string state_dir_;
  std::string device_id_;
  std::vector<Experiment> experiments_;
  std::vector<Assignment> assignments_;  // sorted by experiment
  uint64_t generation_ = 0;
};

}  // namespace expd
