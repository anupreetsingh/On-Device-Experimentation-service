// SHA-256, written out so bucketing gives the same answer on every platform and compiler
// without depending on CommonCrypto or OpenSSL.
#pragma once

#include <array>
#include <cstdint>
#include <string_view>

namespace expd {

std::array<uint8_t, 32> sha256(std::string_view data);

}  // namespace expd
