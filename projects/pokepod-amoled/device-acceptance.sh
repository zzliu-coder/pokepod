#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RUN_ROOT=${1:-"$SCRIPT_DIR/work/device-acceptance/$(date +%Y%m%d-%H%M%S)"}
mkdir -p "$RUN_ROOT"

BEFORE="$RUN_ROOT/status-before.json"
AFTER="$RUN_ROOT/status-after.json"
RESULT="$RUN_ROOT/result.txt"

if ! "$SCRIPT_DIR/cdc-status.py" >"$BEFORE"; then
  printf 'FAIL diagnostics_unavailable evidence=%s\n' "$RUN_ROOT" | tee "$RESULT"
  exit 40
fi

set +e
"$SCRIPT_DIR/usb-audio-smoke.sh" "$RUN_ROOT/audio"
AUDIO_RESULT=$?
set -e

if ! "$SCRIPT_DIR/cdc-status.py" >"$AFTER"; then
  printf 'FAIL diagnostics_lost_after_audio evidence=%s\n' "$RUN_ROOT" | tee "$RESULT"
  exit 41
fi

if ! /usr/bin/python3 - "$BEFORE" "$AFTER" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    before = json.load(source)
with open(sys.argv[2], encoding="utf-8") as source:
    after = json.load(source)

required = {
    "variant": "V1 SH8601/FT3168",
    "ioExpander": True,
    "display": True,
    "touch": True,
    "rtc": True,
    "imu": True,
    "pmu": True,
    "audio": True,
    "usb": True,
    "ui_frame_buffer": True,
    "ui_animation_buffer": True,
}
for key, expected in required.items():
    if after.get(key) != expected:
        raise SystemExit(f"status mismatch {key}={after.get(key)!r}")

for key in ("audio_read_bytes", "uac_attempted_bytes", "uac_accepted_bytes",
            "uac_usb_bytes_sent", "uac_usb_packets_sent"):
    if int(after.get(key, 0)) <= int(before.get(key, 0)):
        raise SystemExit(f"counter did not advance: {key}")

packets = int(after.get("uac_usb_packets_sent", 0)) - int(before.get("uac_usb_packets_sent", 0))
zero_packets = int(after.get("uac_usb_zero_packets", 0)) - int(before.get("uac_usb_zero_packets", 0))
if zero_packets < 0 or zero_packets * 20 > packets:
    raise SystemExit(f"too many zero-length USB packets: {zero_packets}/{packets}")

if int(after.get("mic_open_count", 0)) < 1:
    raise SystemExit("macOS never enabled the microphone interface")
if int(after.get("audio_peak", 0)) < 1:
    raise SystemExit("I2S returned frames with no non-zero microphone samples")
PY
then
  printf 'FAIL firmware_counters evidence=%s\n' "$RUN_ROOT" | tee "$RESULT"
  exit 42
fi

if [ "$AUDIO_RESULT" -ne 0 ]; then
  printf 'FAIL host_audio_capture code=%s evidence=%s\n' "$AUDIO_RESULT" "$RUN_ROOT" | tee "$RESULT"
  exit 43
fi

printf 'PASS device_acceptance evidence=%s\n' "$RUN_ROOT" | tee "$RESULT"
