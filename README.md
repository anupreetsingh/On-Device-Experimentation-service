# On-Device Experimentation Service

A small on-device experimentation system for macOS and Linux. A daemon, `expd`, assigns the device to a variant in each experiment and publishes the assignments. Apps link `libexp`, a C library with a Swift wrapper, and read their variant in one call without leaving their own process.

```swift
if Experiments.variant(for: "new_search_ranking") == "treatment" {
    useNewRanking()
}
```

## How it works

A config push flows through the daemon to a file every app maps, and exposures flow back the other way.

1. **Config push.** `expctl push` sends a config over expd's Unix domain socket. Each line is one experiment, like `new_search_ranking k3f9 control:5000 treatment:5000`. Only expd's own user (or root) may push. expd identifies the client with `getpeereid()` on macOS and `SO_PEERCRED` on Linux.
2. **Assign.** For each experiment, expd computes `bucket = SHA-256(salt, experiment, device_id) % 10000`, and the bucket picks the arm. The device ID is a random UUID created on first run. The hash is deterministic, so the device keeps its variant across reboots with no server state, and ramping the last arm up keeps every device already in it. ([assignment.cpp](expd/assignment.cpp))
3. **Publish.** expd writes every assignment to `snapshot.tmp`, syncs it (`F_FULLFSYNC` on macOS), and `rename()`s it over `snapshot`. On macOS it then posts a Darwin notification. ([snapshot_writer.cpp](expd/snapshot_writer.cpp), [snapshot_format.h](libexp/src/snapshot_format.h))
4. **Read.** An app calls `variant(for:)`. libexp `mmap()`s the snapshot on first use, checks it once (magic, version, size, checksum, sort order), and from then on answers with a binary search over the mapped entries: no syscall, no IPC, no lock. The mapping sits behind one `std::atomic` pointer. ([libexp.cpp](libexp/src/libexp.cpp))
5. **Remap at a safe point.** `exp_refresh()`, called at launch or on return to foreground, remaps if expd has published since. It finds out through `notify_check()` on macOS and inotify on Linux. Until then the app keeps reading its old mapping, so a variant never changes mid-session. The exception is a kill switch, meaning an experiment removed from the config: that posts a second notification, and libexp remaps immediately.
6. **Slow path.** If there's no snapshot yet, libexp sends `GET` to expd over the socket instead.
7. **Exposure.** The first read of each experiment with a given snapshot sends `EXPOSE` to expd. The message is non-blocking and gets no reply, and each mapped entry has an atomic flag so the exposure goes out exactly once.
8. **Buffer and flush.** expd's event loop copies the exposure into a fixed-size, mutex-guarded ring buffer and goes back to serving clients. A writer drains the buffer to `exposures.log` on a coalesced 60 s timer (10 s leeway), or early when the buffer is 75% full, and once more on SIGTERM. ([exposure_log.cpp](expd/exposure_log.cpp))

expd itself is one thread running a level-triggered kqueue (macOS) or epoll (Linux) loop over non-blocking sockets ([event_loop.cpp](expd/event_loop.cpp), [server.cpp](expd/server.cpp)). launchd or systemd create its socket and start expd on the first connection ([activation.cpp](expd/activation.cpp)).

## Layout

| Path | What |
|---|---|
| `expd/` | The daemon: bucketing, snapshot writer, event loop, server, exposure log |
| `libexp/include/exp.h` | The C API apps call |
| `libexp/src/` | libexp, plus the snapshot format, framing and paths shared with expd |
| `swift/` | The Swift wrapper (`Experiments`) and `exp-demo` |
| `tools/` | `expctl` (push, get, expose, stats) and `exp_bench` |
| `deploy/`, `scripts/` | launchd plist, systemd units, install scripts |
| `tests/` | GoogleTest suite |

## Build and test

```sh
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure

# The same tests under ThreadSanitizer
cmake -S . -B build-tsan -DEXP_TSAN=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-tsan -j && ctest --test-dir build-tsan

# Swift wrapper and demo
swift build
```

The tests pin bucket values computed independently with Python's `hashlib`, check the traffic split and independence across experiments, and reject damaged snapshots. They also check that a mapped snapshot survives a publish, that half a frame doesn't block other clients, and that exposures are sent once. Two tests race readers against remaps and the event loop against the writer, for ThreadSanitizer to check. CI runs everything on macOS and Linux, with and without TSan.

## Try it

```sh
export EXP_STATE_DIR=~/.expd            # the default; expd and apps must agree
./build/expd &                          # run by hand, it creates the socket itself
./build/expctl push examples/experiments.conf
./build/expctl get new_search_ranking   # OK control
.build/debug/exp-demo new_search_ranking photo_grid_layout
./build/exp_bench                       # times both read paths
./build/expctl stats
kill %1                                 # SIGTERM: flushes exposures, then exits
```

To have launchd start expd on demand instead, run `scripts/install-launchd.sh`; on Linux, `scripts/install-systemd.sh`. The state directory holds `device_id`, `config`, `snapshot`, `exposures.log` and `expd.sock`.

## Protocol

Each message is a frame: a 4-byte big-endian length, then that many bytes of text.

| Request | Reply |
|---|---|
| `GET <experiment>` | `OK <variant>` or `NONE` |
| `PUSH\n<config>` | `OK <experiment count>` or `ERR <reason>` |
| `EXPOSE <experiment> <variant>` | none |
| `STATS` | `OK experiments=… generation=… requests=… exposures=… dropped=… flushes=… written=…` |

## Measurements

On an Apple Silicon Mac, with 3 experiments in the snapshot (`exp_bench`, mean of 10M and 20k calls after warm-up):

| Path | Time per lookup |
|---|---|
| mmap'd snapshot | about 10 ns |
| GET over the Unix domain socket | about 5 µs |

expd's RSS after a push and some exposures: about 1.8 MB.

To compare writer wakeups at different flush intervals, set `EXPD_FLUSH_INTERVAL_MS` and read `flushes=` from `expctl stats`, or watch `top -stats pid,command,idlew` on macOS.

## Design notes

- **Unix domain sockets, not XPC.** UDS runs on both macOS and Linux. An Apple-only version would use XPC instead, which brings typed messages, launchd lifecycle, priority donation, and entitlement checks on the client's audit token.
- **One event-loop thread.** Each request is a lookup costing microseconds, so one thread is enough, needs no locks on the assignment table, and keeps memory small. The only extra thread does the disk writes, which can block.
- **rename() instead of in-place writes.** Readers never see a half-written file and never need a cross-process lock. expd never truncates a file someone has mapped, which would make them crash with SIGBUS on the next read past the new end.
- **Old mappings are kept until exit.** Another thread may still be mid-lookup in one. Snapshots are a few KB and change rarely, so this costs little. Epoch-based reclamation would be the next step if they changed often.
- **A full ring buffer overwrites its oldest exposure** and counts the drop, because the event loop must never wait on the disk. SIGKILL loses up to one flush interval of exposures.

## Not covered

- Configs arrive by `expctl push`. There's no fetch from a server.
- Exposures stay in a local log. Nothing uploads or aggregates them.
- The Linux paths (epoll, signalfd, inotify, systemd) are exercised in CI. Day-to-day development and the launchd testing were on macOS.
