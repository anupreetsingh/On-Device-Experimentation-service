// Where expd and its clients meet: the state directory, the files in it, and the
// notification names. Shared by expd and libexp so both sides agree.
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
constexpr const char* kSocketFile = "expd.sock";

inline std::string snapshot_path() { return state_dir() + "/" + kSnapshotFile; }
inline std::string socket_path() { return state_dir() + "/" + kSocketFile; }

// Darwin notifications (notify_post) expd sends after publishing. Apps remap at their next
// safe point after the first; the second means an experiment was turned off, so remap now.
constexpr const char* kSnapshotNotification = "com.example.expd.snapshot";
constexpr const char* kKillSwitchNotification = "com.example.expd.killswitch";

}  // namespace locations
