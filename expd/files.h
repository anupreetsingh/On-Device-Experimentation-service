#pragma once

#include <sys/types.h>

#include <string>
#include <string_view>

namespace expd {

// Replaces `path` so readers see the old contents or the new, never a mix: writes
// `path`.tmp in the same directory, flushes it to disk, then rename()s it over `path`.
bool write_file_atomically(const std::string& path, std::string_view data, mode_t mode,
                           std::string* error);

// Reads a whole file. False if it can't be opened or read.
bool read_file(const std::string& path, std::string* out);

}  // namespace expd
