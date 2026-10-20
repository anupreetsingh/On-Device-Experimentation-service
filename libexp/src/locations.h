// Where expd and its clients meet: the state directory and the files in it. Shared by expd
// and libexp so both sides agree.
#pragma once

#include <cstdlib>
#include <string>

namespace locations {

// $EXP_STATE_DIR if set (tests, side-by-side installs), else $HOME/.expd.
inline std::string state_dir() {
  if (const char* dir = std::getenv("EXP_STATE_DIR"); dir != nullptr && *dir != '\0') return dir;
  const char* home = std::getenv("HOME");
  return std::string(home != nullptr && *home != '\0' ? home : "/tmp") + "/.expd";
}

constexpr const char* kSnapshotFile = "snapshot";

inline std::string snapshot_path() { return state_dir() + "/" + kSnapshotFile; }

}  // namespace locations
