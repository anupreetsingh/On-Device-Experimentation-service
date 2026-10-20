/* libexp: read this device's experiment variants.
 *
 * expd publishes a snapshot file with this device's variant for every experiment. libexp
 * maps it into the app's address space on first use, so a lookup is a binary search over
 * memory: no syscall, no IPC, no lock. Before expd has published anything, libexp asks expd
 * over its socket instead.
 *
 * Every function is safe to call from any thread, including the main thread. */
#ifndef EXP_H
#define EXP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest variant name plus its NUL. A buffer this size always fits. */
#define EXP_VARIANT_MAX 16

/* Copies this device's variant for `experiment` into `out` and returns 0. Returns -1, leaving
 * `out` untouched, if the device isn't in the experiment, expd can't be reached, or `out` is
 * too small. On -1 the app uses its default. */
int exp_variant(const char *experiment, char *out, size_t out_len);

/* Picks up a new snapshot if expd has published one. Call it at a safe point, like app launch
 * or return to foreground, so a variant never changes mid-session. Returns 1 if a newer
 * snapshot is now in use, else 0. */
int exp_refresh(void);

/* The generation of the snapshot in use, or 0 if none is mapped. */
uint64_t exp_snapshot_generation(void);

#ifdef __cplusplus
}
#endif

#endif /* EXP_H */
