#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "assignment.h"

namespace expd {

// The bytes of a snapshot file (snapshot_format.h) holding `assignments`.
std::string encode_snapshot(std::vector<Assignment> assignments, uint64_t generation);

// Publishes a new snapshot in `dir`: writes snapshot.tmp, syncs it, and rename()s it over
// `snapshot`. Apps that already mapped the old file keep reading it, because rename() only
// moves the name; the old file's data lives until its last mapping goes away.
bool publish_snapshot(const std::string& dir, const std::vector<Assignment>& assignments,
                      uint64_t generation, std::string* error);

// The generation of the valid snapshot in `dir`, or 0 if there isn't one.
uint64_t current_generation(const std::string& dir);

}  // namespace expd
