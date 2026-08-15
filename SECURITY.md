# Security policy

## Supported versions

Security fixes apply to the current clean Git candidate on the default branch.
Older firmware binaries, local build directories, and unreviewed branches are
not release channels.  The repository does not claim Secure Boot, flash
encryption, encrypted NVS, or cryptographically signed OTA as implemented
features.

## Reporting a vulnerability

Please use GitHub Security Advisories (a private advisory for this repository)
when the
report may expose a credential, pairing material, device identity, or an
exploitable protocol path.  Include the affected commit, product surface,
reproduction steps, and the smallest safe evidence set.  Remove credentials,
private keys, Wi-Fi passwords, Tencent secrets, serial transcripts containing
identifiers, and capsule contents before attaching files.

If private advisories are unavailable, open a minimal public issue that only
states the affected component and asks for a private reporting channel.  Do
not publish a working exploit or secret in the issue.

## Scope and response boundary

The scope includes the firmware Link/USB/BLE protocol, the Mac and Android
clients, the shared PokeCapsule file protocol, and the computer-side fixture
software.  The scope excludes unconnected fixture mechanics, third-party
cloud services, and hardware faults that cannot be reproduced from software
evidence.

Reports are triaged against the exact source commit and then reproduced with
host tests before any device operation is considered.  The repository CI and
source package do not access a serial port, flash a device, or collect a
device dump while receiving a report；不通过串口或刷写设备收集安全报告。
