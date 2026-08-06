#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RUN_ROOT=${1:-"$SCRIPT_DIR/work/end-to-end-acceptance/$(date +%Y%m%d-%H%M%S)"}
mkdir -p "$RUN_ROOT"

json_field() {
  /usr/bin/python3 - "$1" "$2" <<'PY'
import json
import sys
with open(sys.argv[1], encoding="utf-8") as source:
    value = json.load(source).get(sys.argv[2])
if isinstance(value, bool):
    print("true" if value else "false")
elif value is not None:
    print(value)
PY
}

"$SCRIPT_DIR/device-acceptance.sh" "$RUN_ROOT/device"
"$SCRIPT_DIR/mac-dictation-diagnostics.sh" >"$RUN_ROOT/mac.txt"

if ! "$SCRIPT_DIR/cdc-status.py" >"$RUN_ROOT/status-before.json"; then
  printf 'FAIL diagnostics_unavailable_before_dictation evidence=%s\n' "$RUN_ROOT" | tee "$RUN_ROOT/result.txt"
  exit 50
fi
if [ "$(json_field "$RUN_ROOT/status-before.json" mic_streaming)" = "true" ]; then
  printf 'FAIL microphone_busy_before_dictation evidence=%s\n' "$RUN_ROOT" | tee "$RUN_ROOT/result.txt"
  exit 51
fi
BEFORE_OPEN=$(json_field "$RUN_ROOT/status-before.json" mic_open_count)

if ! "$SCRIPT_DIR/cdc-status.py" --command dictate --event dictation_trigger \
  >"$RUN_ROOT/dictation-trigger.json"; then
  printf 'FAIL dictation_hid_trigger_unavailable evidence=%s\n' "$RUN_ROOT" | tee "$RUN_ROOT/result.txt"
  exit 52
fi
if [ "$(json_field "$RUN_ROOT/dictation-trigger.json" shortcut)" != "OPTION_Z" ]; then
  printf 'FAIL unexpected_hid_shortcut evidence=%s\n' "$RUN_ROOT" | tee "$RUN_ROOT/result.txt"
  exit 53
fi

DEADLINE=$(( $(date +%s) + 10 ))
OPENED=false
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  if "$SCRIPT_DIR/cdc-status.py" --timeout 0.8 >"$RUN_ROOT/status-dictating.json" 2>/dev/null; then
    AFTER_OPEN=$(json_field "$RUN_ROOT/status-dictating.json" mic_open_count)
    if [ "${AFTER_OPEN:-0}" -gt "${BEFORE_OPEN:-0}" ]; then
      OPENED=true
      break
    fi
  fi
done
if [ "$OPENED" != "true" ]; then
  printf 'FAIL macos_dictation_did_not_open_microphone evidence=%s\n' "$RUN_ROOT" | tee "$RUN_ROOT/result.txt"
  exit 54
fi

if [ "$(json_field "$RUN_ROOT/status-dictating.json" mic_streaming)" = "true" ]; then
  "$SCRIPT_DIR/cdc-status.py" --command dictate --event dictation_trigger \
    >"$RUN_ROOT/dictation-stop.json"
fi

printf 'PASS end_to_end_acceptance shortcut=option_z permissions=not_required evidence=%s\n' \
  "$RUN_ROOT" | tee "$RUN_ROOT/result.txt"
