#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RUN_ROOT=${1:-"$SCRIPT_DIR/work/device-acceptance/$(date +%Y%m%d-%H%M%S)"}
mkdir -p "$RUN_ROOT"

STATUS="$RUN_ROOT/status.json"
RESULT="$RUN_ROOT/result.txt"

if ! "$SCRIPT_DIR/cdc-status.py" >"$STATUS"; then
  printf 'FAIL diagnostics_unavailable evidence=%s\n' "$RUN_ROOT" | tee "$RESULT"
  exit 40
fi

if ! /usr/bin/python3 - "$STATUS" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    status = json.load(source)

required = {
    "variant": "V1 SH8601/FT3168",
    "ioExpander": True,
    "display": True,
    "touch": True,
    "sdReady": True,
    "rtc": True,
    "imu": True,
    "pmu": True,
    "audio": True,
    "usb": True,
    "ui_frame_buffer": True,
    "ui_animation_buffer": True,
}
for key, expected in required.items():
    if status.get(key) != expected:
        raise SystemExit(f"status mismatch {key}={status.get(key)!r}")
PY
then
  printf 'FAIL firmware_status evidence=%s\n' "$RUN_ROOT" | tee "$RESULT"
  exit 42
fi

printf 'PASS device_acceptance evidence=%s\n' "$RUN_ROOT" | tee "$RESULT"
