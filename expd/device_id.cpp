#include "device_id.h"

#include <fcntl.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "files.h"

namespace expd {
namespace {

bool looks_like_uuid(const std::string& s) {
  if (s.size() != 36) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    bool dash = i == 8 || i == 13 || i == 18 || i == 23;
    if (dash ? s[i] != '-' : !std::isxdigit(static_cast<unsigned char>(s[i]))) return false;
  }
  return true;
}

bool random_bytes(uint8_t* out, size_t len) {
  int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
  if (fd < 0) return false;
  size_t got = 0;
  while (got < len) {
    ssize_t n = read(fd, out + got, len - got);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    got += size_t(n);
  }
  close(fd);
  return got == len;
}

}  // namespace

std::string load_or_create_device_id(const std::string& path, std::string* error) {
  std::string saved;
  if (read_file(path, &saved)) {
    while (!saved.empty() && (saved.back() == '\n' || saved.back() == '\r')) saved.pop_back();
    if (looks_like_uuid(saved)) return saved;
  }

  // First run, or the file is damaged: make a random (version 4) UUID.
  uint8_t b[16];
  if (!random_bytes(b, sizeof b)) {
    *error = std::string("read /dev/urandom: ") + std::strerror(errno);
    return "";
  }
  b[6] = uint8_t((b[6] & 0x0f) | 0x40);  // version 4
  b[8] = uint8_t((b[8] & 0x3f) | 0x80);  // RFC 4122 variant
  char id[37];
  std::snprintf(id, sizeof id,
                "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", b[0], b[1],
                b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14],
                b[15]);
  if (!write_file_atomically(path, std::string(id) + "\n", 0600, error)) return "";
  return id;
}

}  // namespace expd
