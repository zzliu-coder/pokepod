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

The repository CI has two complementary firmware views. The exact-head audit
workflow binds source and Fast evidence to the PR head; the separate
`.github/workflows/repository-integration.yml` workflow checks the PR
merge-result firmware source gate and runs independent Mac, Android and shared
protocol lanes. The protocol lane uses the dependency-free
`projects/pokecapsule-protocol/validate-fixtures.py` validator and rejects
missing local `$ref` targets, invalid examples and invalid processing fixtures.
The Android lane runs debug checks plus Release lint/assemble using a temporary
runner-only signing key; this validates the variant build graph and does not
produce a distributable signed release. Firmware Fast is an exact-head
artifact/evidence build; firmware Release is not run. Neither firmware build
nor any Mac/Android host result accesses a serial port or flashes hardware, and
a green host/CI result remains `unverified` for device behavior.

The repository contains a CODEOWNERS routing file and a security policy. GitHub
branch protection, required checks, signing status and downloaded-artifact
verification are external repository facts; source files can describe the
required policy but cannot prove that those settings are enabled.

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

USB firmware OTA binds the request to the artifact's `sourceRevision`,
`firmwareVersion`, and `appElfSha256`. The device requires all three fields;
it validates the source/version marker and reads the candidate OTA slot's ESP
application descriptor before `esp_ota_end` and boot-slot selection. A missing,
zero, or mismatched candidate ELF digest aborts without changing the boot
partition. The fixture additionally waits for the application to return and
checks the running partition and all three fields. Direct `cdc-status.py`
firmware requests require the same adjacent artifact and ELF files; optional
identity arguments can only confirm those validated values.

The current runtime hardening releases the complete BLE allocation before
SoftAP provisioning, removes NVS writes from the timed SD qualification probe,
and keeps persistent diagnostics out of the wireless capture start window.
Status and terminal logs expose capture read counts, I2S timeouts, ring
high-water and drops so device failures can be diagnosed from collected facts.
