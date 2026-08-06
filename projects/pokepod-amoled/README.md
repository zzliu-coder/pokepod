# PokePod AMOLED

PokePod AMOLED turns the Waveshare ESP32-S3-Touch-AMOLED-1.8 into a small
PokeCapsule companion:

- USB microphone for macOS Dictation
- direct USB Option-Z dictation trigger from the BOOT button or touch screen
- diagnostic USB serial commands
- local 48 kHz, stereo, 16-bit WAV recording to microSD
- display, touch, RTC, IMU, battery/PMU and SD status dashboard

The firmware electronically detects the board revision at startup:

- I2C `0x15`: V2, CO5300 display and CST820-compatible touch
- I2C `0x38`: V1, SH8601 display and FT3168 touch

The same firmware image supports both revisions. Detection results are printed
as JSON on the diagnostic USB serial port and shown on the AMOLED.

## Build

Run:

```sh
./firmware/build.sh
```

The build is pinned to the Waveshare source commit recorded in the script and
uses Arduino-ESP32 3.3.8 plus the locally installed Arduino GFX 1.6.5 compatibility
library. Artifacts are written under `work/pokepod-build/output`.

Run the complete software gate with `./verify.sh`. It executes the firmware
host tests, a clean firmware compile, all Mac tests, a release Mac build and
artifact checks. After flashing, `./device-acceptance.sh` queries the device's
own I2S/USB counters and records three seconds from the USB microphone. That
test does not require a person to speak or inspect the screen.

`./end-to-end-acceptance.sh` asks the device to emit Option-Z and verifies that
macOS Dictation opens the device's USB microphone. PokeCapsule keyboard privacy
permissions and a physical button press are not part of this gate.

## Device controls

- BOOT short press: send Option-Z to the Mac
- `MAC DICTATION`: send Option-Z from the touch screen
- `CAPSULE RECORD`: start or stop a WAV recording on microSD
- USB serial commands: `status`, `dictate`, `record`, `stop`

The diagnostic serial interface also enables the standard 1200-baud reboot
path, so firmware updates after the first diagnostic build can enter the ROM
loader from the Mac without holding BOOT.

Recordings are staged under `/PokePod/recordings/<id>` and committed with a
rename only after their WAV header and metadata have been finalized. They stay
separate from the PokeCapsule device library until the Mac converts/imports them.

## Mac setup

1. Set the macOS Dictation shortcut to Option-Z.
2. Select **TinyUSB UAC1** (manufacturer **PokeCapsule**, 48 kHz) as the Dictation/input microphone. macOS uses the USB audio interface name here; the parent USB/HID device is named **PokePod V1 Voice** or **PokePod V2 Voice** after hardware detection.

The device sends Option-Z directly as a standard USB keyboard. PokeCapsule does
not monitor or synthesize keyboard input, so Accessibility and Input Monitoring
permissions are not required for dictation.

## Recovery boundary

Flashing must only happen after two complete 16 MB reads of the original flash
match byte-for-byte and the security state has been recorded. The merged image is
written at flash offset `0x0`. The original image can be restored at the same
offset while the device is in ROM BOOT mode.
