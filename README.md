# On-Device Experimentation Service

A small on-device experimentation system for macOS and Linux:

- `expd`, a C++ daemon that assigns this device to experiment variants and publishes the assignments.
- `libexp`, a C client library (with a Swift wrapper) that apps link to read their variants.

Work in progress.
