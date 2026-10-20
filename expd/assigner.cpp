#include "assigner.h"

#include <algorithm>

#include "files.h"
#include "snapshot_writer.h"

namespace expd {

Assigner::Assigner(std::string state_dir, std::string device_id)
    : state_dir_(std::move(state_dir)), device_id_(std::move(device_id)) {}

bool Assigner::load(std::string* error) {
  // Continue the generation count, so apps holding an older snapshot see this one as newer.
  generation_ = current_generation(state_dir_);
  std::string text;
  if (!read_file(state_dir_ + "/config", &text)) text.clear();
  std::vector<Experiment> experiments;
  if (!parse_config(text, &experiments, error)) return false;
  return apply(std::move(experiments), error);
}

const std::string* Assigner::variant(std::string_view experiment) const {
  auto it = std::lower_bound(
      assignments_.begin(), assignments_.end(), experiment,
      [](const Assignment& a, std::string_view name) { return a.experiment < name; });
  if (it == assignments_.end() || it->experiment != experiment) return nullptr;
  return &it->variant;
}

bool Assigner::apply(std::vector<Experiment> experiments, std::string* error) {
  std::vector<Assignment> assignments = assign(experiments, device_id_);
  if (!publish_snapshot(state_dir_, assignments, generation_ + 1, error)) return false;
  ++generation_;
  experiments_ = std::move(experiments);
  assignments_ = std::move(assignments);
  return true;
}

}  // namespace expd
