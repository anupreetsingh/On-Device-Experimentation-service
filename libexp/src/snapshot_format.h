// The snapshot file: expd writes it, every app maps it read-only. Both sides include this
// header, so the layout is defined once. Integers are in host byte order because the file
// never leaves the device.
//
//   Header   24 bytes: "EXPS", version, entry count, checksum, generation
//   Entry    64 bytes each, sorted by experiment name so readers can binary-search
#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace snapshot {

constexpr char kMagic[4] = {'E', 'X', 'P', 'S'};
constexpr uint32_t kVersion = 1;
constexpr size_t kExperimentSize = 48;  // name + NUL
constexpr size_t kVariantSize = 16;     // name + NUL

struct Header {
  char magic[4];
  uint32_t version;
  uint32_t count;       // entries after the header
  uint32_t checksum;    // FNV-1a over all the entries
  uint64_t generation;  // up by one on every publish
};

struct Entry {
  char experiment[kExperimentSize];
  char variant[kVariantSize];
};

static_assert(sizeof(Header) == 24, "snapshot header layout changed");
static_assert(sizeof(Entry) == 64, "snapshot entry layout changed");

inline uint32_t fnv1a(const void* data, size_t len) {
  const auto* p = static_cast<const unsigned char*>(data);
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < len; ++i) {
    h ^= p[i];
    h *= 16777619u;
  }
  return h;
}

// Checks a whole file once, when it's mapped, so lookups never have to. Returns the entries,
// or nullptr if the file is truncated, another version, corrupt, or not sorted.
inline const Entry* validate(const void* base, size_t size, uint32_t* count, uint64_t* generation) {
  if (size < sizeof(Header)) return nullptr;
  Header header;
  std::memcpy(&header, base, sizeof header);
  if (std::memcmp(header.magic, kMagic, sizeof kMagic) != 0 || header.version != kVersion) {
    return nullptr;
  }
  if (header.count > (size - sizeof(Header)) / sizeof(Entry) ||
      size != sizeof(Header) + size_t(header.count) * sizeof(Entry)) {
    return nullptr;
  }
  const auto* entries =
      reinterpret_cast<const Entry*>(static_cast<const char*>(base) + sizeof(Header));
  if (fnv1a(entries, size_t(header.count) * sizeof(Entry)) != header.checksum) return nullptr;
  for (uint32_t i = 0; i < header.count; ++i) {
    const Entry& e = entries[i];
    if (e.experiment[0] == '\0' || std::memchr(e.experiment, '\0', kExperimentSize) == nullptr ||
        e.variant[0] == '\0' || std::memchr(e.variant, '\0', kVariantSize) == nullptr) {
      return nullptr;
    }
    if (i > 0 && std::strcmp(entries[i - 1].experiment, e.experiment) >= 0) return nullptr;
  }
  *count = header.count;
  *generation = header.generation;
  return entries;
}

// Binary search by experiment name: plain memory reads, no syscalls.
inline const Entry* find(const Entry* entries, uint32_t count, const char* experiment) {
  uint32_t lo = 0;
  uint32_t hi = count;
  while (lo < hi) {
    uint32_t mid = lo + (hi - lo) / 2;
    int c = std::strcmp(entries[mid].experiment, experiment);
    if (c == 0) return &entries[mid];
    if (c < 0) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return nullptr;
}

}  // namespace snapshot
