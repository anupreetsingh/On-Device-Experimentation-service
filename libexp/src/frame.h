// Messages on expd's socket are frames: a 4-byte big-endian length, then that many bytes of
// payload. A stream socket has no message boundaries, so the reader needs the length to know
// where one message ends.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace frame {

constexpr uint32_t kMaxPayload = 64 * 1024;

inline void append(std::string* out, std::string_view payload) {
  uint32_t n = static_cast<uint32_t>(payload.size());
  const char length[4] = {char(n >> 24), char(n >> 16), char(n >> 8), char(n)};
  out->append(length, sizeof length);
  out->append(payload);
}

// Looks for one whole frame at the start of `buf`. Returns 1 and sets *payload and *frame_len
// if there is one, 0 if more bytes are needed, and -1 if the length is over kMaxPayload (the
// peer is broken or hostile).
inline int parse(std::string_view buf, std::string_view* payload, size_t* frame_len) {
  if (buf.size() < 4) return 0;
  const auto* b = reinterpret_cast<const unsigned char*>(buf.data());
  uint32_t n = uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | uint32_t(b[3]);
  if (n > kMaxPayload) return -1;
  if (buf.size() < 4 + size_t(n)) return 0;
  *payload = buf.substr(4, n);
  *frame_len = 4 + size_t(n);
  return 1;
}

}  // namespace frame
