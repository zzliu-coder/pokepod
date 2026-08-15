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

The device settings UI keeps a fixed five-row layout: Wi-Fi configuration is
opened from the left side of the wireless row while the right side remains the
Wi-Fi toggle; Bluetooth pairing and its master switch use the same split-row
interaction. The fifth row is an explicit shutdown action with confirmation and
the existing cooperative safe-shutdown gate. USB CDC OTA is implemented for a
running firmware image; the fixture performs identity, size and SHA-256 checks,
streams the inactive OTA slot, and leaves BOOT/RESET as the recovery path.
The repository also includes a reference USB fixture-controller firmware,
fail-closed host control profile, automatic ROM rescue, full app0 readback and
recording/provisioning exercise capture. A plain USB cable supports normal OTA;
automatic recovery requires the protected open-drain BOOT/RESET controller.

The current runtime hardening releases the complete BLE allocation before
SoftAP provisioning, removes NVS writes from the timed SD qualification probe,
and keeps persistent diagnostics out of the wireless capture start window.
Status and terminal logs expose capture read counts, I2S timeouts, ring
high-water and drops so device failures can be diagnosed from collected facts.
