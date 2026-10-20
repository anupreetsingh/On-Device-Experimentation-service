#pragma once

#include <string>

namespace expd {

// The device's ID for bucketing: a random UUID made on first run and saved at `path` (mode
// 0600). It isn't a hardware ID and never leaves the device. Deleting the file gives the
// device a new ID, which re-buckets it in every experiment. Returns "" on failure.
std::string load_or_create_device_id(const std::string& path, std::string* error);

}  // namespace expd
