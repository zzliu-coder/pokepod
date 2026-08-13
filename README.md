# PokePod firmware

This repository contains the native PokeCapsule/PokePod history and the
audit-remediation integration for the Waveshare
ESP32-S3-Touch-AMOLED-1.8 firmware.

The real pre-remediation baseline is commit
`2278bb11af4c97f83e71e6e3529b440e80810419` on
`feature/pokepod-amoled-1.8`. It is an ancestor in this repository's native Git
history. External audit archives and reports are review inputs; they do not
replace that history.

The firmware project is under [`projects/pokepod-amoled`](projects/pokepod-amoled).
The integrated remediation history is on
`codex/pokepod-audit-remediation-integration`. The reviewed code cut before the
documentation-only reconciliation is
`590b549cb49b9a02064846673b51d26c585ee8a7`; the linear history keeps each
remediation reviewable as a separate commit.

The repository CI runs source, asset, toolchain, ASan/UBSan, source-audit
round-trip and a clean forced Fast build. It does not run a Release build,
access a serial port, or flash hardware.
