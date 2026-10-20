// libexp entry points for this repo's benchmark and tests, not for apps.
#pragma once

#include <cstddef>

extern "C" {

// The slow path on its own: asks expd over the socket even when a snapshot is mapped.
int exp_internal_variant_via_socket(const char* experiment, char* out, size_t out_len);

}  // extern "C"
