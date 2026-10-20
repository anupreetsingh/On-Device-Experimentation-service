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
  if (!apply(std::move(experiments), error)) return false;
  announce_snapshot(false);
  return true;
}

bool Assigner::push(std::string_view config_text, std::string* error) {
  std::vector<Experiment> experiments;
  if (!parse_config(config_text, &experiments, error)) return false;

  // Removing an experiment is the kill switch: apps remap right away instead of waiting for
  // their next safe point, so the feature turns off now.
  bool removed_any = std::any_of(experiments_.begin(), experiments_.end(), [&](const Experiment& old) {
    return std::none_of(experiments.begin(), experiments.end(),
                        [&](const Experiment& e) { return e.name == old.name; });
  });

  // Save the config before publishing, so a restart serves what was pushed.
  std::string path = state_dir_ + "/config";
  std::string previous;
  bool had_previous = read_file(path, &previous);
  if (!write_file_atomically(path, config_text, 0600, error)) return false;
  if (!apply(std::move(experiments), error)) {
    std::string ignored;
    if (had_previous) write_file_atomically(path, previous, 0600, &ignored);
    return false;
  }
  announce_snapshot(removed_any);
  return true;
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
