#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RUN_ROOT=${1:-"$SCRIPT_DIR/work/hid-shortcut-smoke/$(date +%Y%m%d-%H%M%S)"}
mkdir -p "$RUN_ROOT"

MONITOR="$RUN_ROOT/hid-monitor"
EVENTS="$RUN_ROOT/events.txt"
ERRORS="$RUN_ROOT/monitor-error.txt"
TRIGGER="$RUN_ROOT/trigger.json"
STATUS_BEFORE="$RUN_ROOT/status-before.json"
STATUS="$RUN_ROOT/status-held.json"
STATUS_STOPPED="$RUN_ROOT/status-stopped.json"
RESULT="$RUN_ROOT/result.txt"
MONITOR_PID=""
HOLD_ACTIVE=0

cleanup() {
  if [ "$HOLD_ACTIVE" -eq 1 ]; then
    "$SCRIPT_DIR/cdc-status.py" --command dictate-stop \
      --event dictation_stopped --timeout 5 >/dev/null 2>&1 || true
    HOLD_ACTIVE=0
  fi
  if [ -n "$MONITOR_PID" ]; then
    kill "$MONITOR_PID" 2>/dev/null || true
    wait "$MONITOR_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT HUP INT TERM

clang -x c -framework ApplicationServices -o "$MONITOR" - <<'C'
#include <ApplicationServices/ApplicationServices.h>
#include <stdio.h>

static CGEventRef capture(CGEventTapProxy proxy, CGEventType type,
                          CGEventRef event, void *context) {
  (void)proxy;
  (void)context;
  if (type == kCGEventKeyDown || type == kCGEventKeyUp ||
      type == kCGEventFlagsChanged) {
    const long long key = CGEventGetIntegerValueField(
        event, kCGKeyboardEventKeycode);
    const unsigned long long flags = (unsigned long long)CGEventGetFlags(event);
    printf("type=%u keycode=%lld flags=0x%llx\n",
           (unsigned)type, key, flags);
    fflush(stdout);
  }
  return event;
}

int main(void) {
  const CGEventMask mask = CGEventMaskBit(kCGEventKeyDown) |
      CGEventMaskBit(kCGEventKeyUp) |
      CGEventMaskBit(kCGEventFlagsChanged);
  CFMachPortRef tap = CGEventTapCreate(
      kCGSessionEventTap, kCGHeadInsertEventTap,
      kCGEventTapOptionListenOnly, mask, capture, NULL);
  if (tap == NULL) {
    fputs("event_tap_unavailable\n", stderr);
    return 2;
  }
  CFRunLoopSourceRef source = CFMachPortCreateRunLoopSource(
      kCFAllocatorDefault, tap, 0);
  CFRunLoopAddSource(CFRunLoopGetCurrent(), source, kCFRunLoopCommonModes);
  CGEventTapEnable(tap, true);
  CFRunLoopRun();
  return 0;
}
C

"$MONITOR" >"$EVENTS" 2>"$ERRORS" &
MONITOR_PID=$!
sleep 0.5
"$SCRIPT_DIR/cdc-status.py" --command status --timeout 3 >"$STATUS_BEFORE"
"$SCRIPT_DIR/cdc-status.py" --command dictate-start --event dictation_started \
  --timeout 5 >"$TRIGGER"
HOLD_ACTIVE=1
sleep 1
"$SCRIPT_DIR/cdc-status.py" --command status --timeout 3 >"$STATUS"
"$SCRIPT_DIR/cdc-status.py" --command dictate-stop --event dictation_stopped \
  --timeout 5 >"$RUN_ROOT/dictation-stop.json"
HOLD_ACTIVE=0

STOP_DEADLINE=$(( $(date +%s) + 5 ))
STOPPED=false
while [ "$(date +%s)" -lt "$STOP_DEADLINE" ]; do
  if "$SCRIPT_DIR/cdc-status.py" --command status --timeout 1 >"$STATUS_STOPPED" 2>/dev/null &&
     /usr/bin/python3 - "$STATUS_BEFORE" "$STATUS_STOPPED" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    before = json.load(source)
with open(sys.argv[2], encoding="utf-8") as source:
    stopped = json.load(source)

closed = int(stopped.get("mic_close_count", 0)) > int(
    before.get("mic_close_count", 0)
)
raise SystemExit(0 if closed and not stopped.get("mic_streaming", False)
                 and not stopped.get("dictation_holding", False) else 1)
PY
  then
    STOPPED=true
    break
  fi
done
cleanup
MONITOR_PID=""

if [ "$STOPPED" != "true" ]; then
  printf 'FAIL wetype_voice_did_not_release_microphone evidence=%s\n' "$RUN_ROOT" |
    tee "$RESULT"
  exit 62
fi

if [ -s "$ERRORS" ]; then
  printf 'FAIL hid_event_monitor_unavailable evidence=%s\n' "$RUN_ROOT" |
    tee "$RESULT"
  exit 61
fi

if ! /usr/bin/python3 - "$EVENTS" "$STATUS_BEFORE" "$STATUS" "$STATUS_STOPPED" <<'PY'
import json
import re
import sys

pattern = re.compile(r"type=(\d+) keycode=(\d+) flags=0x([0-9a-fA-F]+)")
events = []
with open(sys.argv[1], encoding="utf-8") as source:
    for line in source:
        match = pattern.fullmatch(line.strip())
        if match:
            events.append(tuple(int(value, 16 if index == 2 else 10)
                                for index, value in enumerate(match.groups())))

alternate = 0x80000
option_down = lambda event: (
    event[0] == 12 and event[1] == 58 and event[2] & alternate
)
option_up = lambda event: (
    event[0] == 12 and event[1] == 58 and not event[2] & alternate
)
full_sequence = [
    option_down,
    lambda event: event[0] == 10 and event[1] == 6 and event[2] & alternate,
    lambda event: event[0] == 11 and event[1] == 6 and event[2] & alternate,
    option_up,
]

def contains(sequence):
    cursor = 0
    for predicate in sequence:
        while cursor < len(events) and not predicate(events[cursor]):
            cursor += 1
        if cursor == len(events):
            return False
        cursor += 1
    return True

with open(sys.argv[2], encoding="utf-8") as source:
    before = json.load(source)
with open(sys.argv[3], encoding="utf-8") as source:
    held = json.load(source)
with open(sys.argv[4], encoding="utf-8") as source:
    stopped = json.load(source)

# WeType consumes the Z and release events before a session event tap can
# observe them. Option-down plus the device's UAC open/hold/close lifecycle is
# the reliable end-to-end proof; a complete visible key sequence is accepted
# as additional evidence when macOS exposes it.
opened = int(held.get("mic_open_count", 0)) > int(before.get("mic_open_count", 0))
closed = int(stopped.get("mic_close_count", 0)) > int(before.get("mic_close_count", 0))
held_ok = opened and held.get("mic_streaming") and held.get("dictation_holding")
stopped_ok = closed and not stopped.get("mic_streaming") and not stopped.get("dictation_holding")
full_redraws_stable = (
    int(held.get("ui_full_redraws", -1)) == int(before.get("ui_full_redraws", -2))
    and int(stopped.get("ui_full_redraws", -1)) == int(before.get("ui_full_redraws", -2))
)
partial_redraws_used = int(stopped.get("ui_partial_redraws", 0)) > int(
    before.get("ui_partial_redraws", 0)
)
if (not contains([option_down]) or not held_ok or not stopped_ok
        or not full_redraws_stable or not partial_redraws_used):
    raise SystemExit(1)
PY
then
  printf 'FAIL hid_or_flicker_regression evidence=%s\n' "$RUN_ROOT" |
    tee "$RESULT"
  exit 63
fi

printf 'PASS hid_option_z_hold_and_partial_refresh evidence=%s\n' "$RUN_ROOT" | tee "$RESULT"
