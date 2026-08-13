# PokePod firmware

This repository contains the PokePod firmware audit-remediation history for the
Waveshare ESP32-S3-Touch-AMOLED-1.8 board.

The repository was reconstructed from the source-audit archive for external
baseline commit `2278bb11af4c97f83e71e6e3529b440e80810419`. The imported root
commit `2ed04f5` has been byte-for-byte compared with that archive: all 360
source-package files and SHA-256 digests match. The original audit report has
SHA-256 `91d413656d9505bf816ec675f975ffa5fddc05e2e98ed73f65d391a72b746eaf`.

The firmware project is under [`projects/pokepod-amoled`](projects/pokepod-amoled).
The `main` branch preserves the imported audit baseline; remediation work is on
`agent/audit-remediation` so each finding remains reviewable as a separate
commit.

The repository CI runs source, asset, toolchain, ASan/UBSan, source-audit
round-trip and a clean forced Fast build. It does not run a Release build,
access a serial port, or flash hardware.
