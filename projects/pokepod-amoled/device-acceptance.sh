#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
EXPECTED_VARIANT=
EVIDENCE_ROOT=
PORT=
ARTIFACT=
DRY_RUN=0
STATUS_FIXTURE=

usage() {
  printf '%s\n' \
    'usage: device-acceptance.sh --expected-variant v1|v2 --evidence-root DIR' \
    '       [--port /dev/cu.usbmodem...] [--artifact BIN] [--status-fixture JSON]' \
    '       [--dry-run]'
}

while [ "$#" -gt 0 ]; do
  case "$1" in
    --expected-variant) EXPECTED_VARIANT=${2-}; shift 2 ;;
    --evidence-root) EVIDENCE_ROOT=${2-}; shift 2 ;;
    --port) PORT=${2-}; shift 2 ;;
    --artifact) ARTIFACT=${2-}; shift 2 ;;
    --status-fixture) STATUS_FIXTURE=${2-}; shift 2 ;;
    --dry-run) DRY_RUN=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) printf 'FAIL unknown_argument=%s\n' "$1" >&2; usage >&2; exit 64 ;;
  esac
done

case "$EXPECTED_VARIANT" in
  v1) VARIANT_NAME='V1 SH8601/FT3168' ;;
  v2) VARIANT_NAME='V2 CO5300/CST820' ;;
  *) printf 'FAIL expected_variant_required value=%s\n' "$EXPECTED_VARIANT" >&2; exit 64 ;;
esac
[ -n "$EVIDENCE_ROOT" ] || { printf 'FAIL evidence_root_required\n' >&2; exit 64; }
[ -z "$PORT" ] || [ -e "$PORT" ] || { printf 'FAIL explicit_port_missing port=%s\n' "$PORT" >&2; exit 66; }
[ -z "$ARTIFACT" ] || [ -f "$ARTIFACT" ] || { printf 'FAIL artifact_missing path=%s\n' "$ARTIFACT" >&2; exit 66; }
[ -z "$STATUS_FIXTURE" ] || [ -f "$STATUS_FIXTURE" ] || { printf 'FAIL status_fixture_missing path=%s\n' "$STATUS_FIXTURE" >&2; exit 66; }

RUN_ROOT="$EVIDENCE_ROOT/$EXPECTED_VARIANT/$(date -u +%Y%m%dT%H%M%SZ)-$$"
mkdir -p "$RUN_ROOT"
STATUS="$RUN_ROOT/preflight-status.json"
IDENTITY="$RUN_ROOT/preflight-identity.json"
MANIFEST="$RUN_ROOT/evidence.json"
RESULT="$RUN_ROOT/result.txt"

if [ -n "$STATUS_FIXTURE" ]; then
  cp "$STATUS_FIXTURE" "$STATUS"
  cp "$STATUS_FIXTURE" "$IDENTITY"
elif [ "$DRY_RUN" -eq 0 ]; then
  [ -n "$PORT" ] || { printf 'FAIL explicit_port_required\n' | tee "$RESULT"; exit 64; }
  if ! "$SCRIPT_DIR/cdc-status.py" "$PORT" >"$STATUS"; then
    printf 'FAIL diagnostics_unavailable evidence=%s\n' "$RUN_ROOT" | tee "$RESULT"
    exit 40
  fi
  if ! "$SCRIPT_DIR/cdc-status.py" "$PORT" --command identity >"$IDENTITY"; then
    printf 'FAIL identity_unavailable evidence=%s\n' "$RUN_ROOT" | tee "$RESULT"
    exit 41
  fi
else
  printf '{"status":"unverified","variant":"%s","deviceId":""}\n' \
    "$VARIANT_NAME" >"$STATUS"
  printf '{"status":"unverified","deviceId":""}\n' >"$IDENTITY"
fi

SOURCE_COMMIT=${POKEPOD_SOURCE_COMMIT:-}
if [ -z "$SOURCE_COMMIT" ]; then
  SOURCE_COMMIT=$(git -C "$SCRIPT_DIR/../.." rev-parse HEAD 2>/dev/null || true)
fi
case "$SOURCE_COMMIT" in
  *[!0-9a-f]*|'')
    printf 'FAIL source_commit_unavailable\n' >&2
    exit 65
    ;;
esac
[ "${#SOURCE_COMMIT}" -eq 40 ] || {
  printf 'FAIL source_commit_invalid value=%s\n' "$SOURCE_COMMIT" >&2
  exit 65
}
ARTIFACT_SHA256=
if [ -n "$ARTIFACT" ]; then
  ARTIFACT_SHA256=$(shasum -a 256 "$ARTIFACT" | awk '{print $1}')
fi

/usr/bin/python3 - "$STATUS" "$IDENTITY" "$MANIFEST" "$EXPECTED_VARIANT" "$VARIANT_NAME" \
  "$SOURCE_COMMIT" "$ARTIFACT_SHA256" "$PORT" "$DRY_RUN" <<'PY'
import datetime as dt
import json
import pathlib
import sys

status_path, identity_path, manifest_path = map(pathlib.Path, sys.argv[1:4])
variant_key, variant_name, commit, artifact_sha, port, dry_run = sys.argv[4:]
status = json.loads(status_path.read_text(encoding="utf-8"))
identity = json.loads(identity_path.read_text(encoding="utf-8"))
if status.get("variant") != variant_name:
    raise SystemExit(
        f"variant mismatch expected={variant_name!r} actual={status.get('variant')!r}"
    )

component_keys = (
    "ioExpander", "display", "touch", "sdReady", "rtc", "imu", "pmu",
    "audio", "usb", "ui_frame_buffer", "ui_animation_buffer",
)
components = {key: status.get(key) for key in component_keys}
preflight_ok = all(value is True for value in components.values())
device_id = identity.get("deviceId", "")
if dry_run == "0" and (not isinstance(device_id, str) or not device_id):
    raise SystemExit("device identity missing")

scenario_names = (
    "local_recording_100", "recording_58_5s_wav_crc_playback_transcription",
    "link_usb_start_stop_abort_disconnect_second_session",
    "link_wifi_start_stop_abort_disconnect_second_session",
    "ble_weak_signal_overflow_recovery", "sd_full_slow_remove_fragment_reinsert",
    "startup_recovery_512", "power_quiescence_current_wake",
    "display_touch_wake",
)
scenarios = [
    {
        "id": name,
        "status": "unverified",
        "verdict": None,
        "rawEvidence": [],
        "note": "behavior evidence must be captured on the explicit device",
    }
    for name in scenario_names
]
now = dt.datetime.now(dt.timezone.utc).replace(microsecond=0).isoformat()
manifest = {
    "schema": "pokepod.device-acceptance.evidence.v2",
    "sourceCommit": commit,
    "artifactSha256": artifact_sha or None,
    "expectedVariant": variant_key,
    "observedVariant": status.get("variant"),
    "deviceIdentity": device_id or None,
    "explicitPort": port or None,
    "startedAt": now,
    "finishedAt": now,
    "preflight": {
        "status": "pass" if preflight_ok else "unverified",
        "components": components,
        "claimLimit": "component presence only; no behavior PASS",
        "rawEvidence": [status_path.name, identity_path.name],
    },
    "scenarios": scenarios,
    "overall": "unverified",
    "writesFirmware": False,
    "autoSelectsSerial": False,
}
manifest_path.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
                         encoding="utf-8")
PY

printf 'UNVERIFIED device_behavior variant=%s evidence=%s\n' \
  "$EXPECTED_VARIANT" "$RUN_ROOT" | tee "$RESULT"
