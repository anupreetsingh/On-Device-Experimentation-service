#include "snapshot_writer.h"

#include <algorithm>
#include <cstring>

#include "files.h"
#include "locations.h"
#include "snapshot_format.h"

#ifdef __APPLE__
#include <notify.h>
#endif

namespace expd {

static_assert(kMaxExperimentName < snapshot::kExperimentSize, "experiment names must fit");
static_assert(kMaxVariantName < snapshot::kVariantSize, "variant names must fit");

std::string encode_snapshot(std::vector<Assignment> assignments, uint64_t generation) {
  std::sort(assignments.begin(), assignments.end(),
            [](const Assignment& a, const Assignment& b) { return a.experiment < b.experiment; });

  std::string bytes(sizeof(snapshot::Header) + assignments.size() * sizeof(snapshot::Entry), '\0');
  char* entries = bytes.data() + sizeof(snapshot::Header);
  for (size_t i = 0; i < assignments.size(); ++i) {
    snapshot::Entry entry{};
    std::memcpy(entry.experiment, assignments[i].experiment.data(),
                std::min(assignments[i].experiment.size(), snapshot::kExperimentSize - 1));
    std::memcpy(entry.variant, assignments[i].variant.data(),
                std::min(assignments[i].variant.size(), snapshot::kVariantSize - 1));
    std::memcpy(entries + i * sizeof entry, &entry, sizeof entry);
  }

  snapshot::Header header{};
  std::memcpy(header.magic, snapshot::kMagic, sizeof header.magic);
  header.version = snapshot::kVersion;
  header.count = static_cast<uint32_t>(assignments.size());
  header.checksum = snapshot::fnv1a(entries, assignments.size() * sizeof(snapshot::Entry));
  header.generation = generation;
  std::memcpy(bytes.data(), &header, sizeof header);
  return bytes;
}

bool publish_snapshot(const std::string& dir, const std::vector<Assignment>& assignments,
                      uint64_t generation, std::string* error) {
  // 0644: every app on the device maps this file read-only.
  return write_file_atomically(dir + "/" + locations::kSnapshotFile,
                               encode_snapshot(assignments, generation), 0644, error);
}

uint64_t current_generation(const std::string& dir) {
  std::string bytes;
  if (!read_file(dir + "/" + locations::kSnapshotFile, &bytes)) return 0;
  uint32_t count;
  uint64_t generation;
  if (snapshot::validate(bytes.data(), bytes.size(), &count, &generation) == nullptr) return 0;
  return generation;
}

void announce_snapshot(bool kill_switch) {
#ifdef __APPLE__
  notify_post(locations::kSnapshotNotification);
  if (kill_switch) notify_post(locations::kKillSwitchNotification);
#else
  (void)kill_switch;
#endif
}

}  // namespace expd
