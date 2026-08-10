#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
FLASH_MODE=release
MODE_EXPLICIT=0
FIRMWARE_BIN=

while [ "$#" -gt 0 ]; do
  case "$1" in
    --fast)
      FLASH_MODE=fast
      MODE_EXPLICIT=1
      shift
      ;;
    --release)
      FLASH_MODE=release
      MODE_EXPLICIT=1
      shift
      ;;
    --firmware)
      [ "$#" -ge 2 ] || { printf 'FAIL firmware_argument_missing\n' >&2; exit 64; }
      FIRMWARE_BIN=$2
      shift 2
      ;;
    --help)
      cat <<'EOF'
Usage: ./flash.sh [--release|--fast] [--firmware PATH]

  --release   Flash the separately built release artifact (default).
  --fast      Explicitly flash the fast iteration artifact.
  --firmware  Use a manifest-backed artifact at an explicit path.
EOF
      exit 0
      ;;
    -*)
      printf 'FAIL unsupported_argument value=%s\n' "$1" >&2
      exit 64
      ;;
    *)
      [ -z "$FIRMWARE_BIN" ] || { printf 'FAIL duplicate_firmware_path\n' >&2; exit 64; }
      FIRMWARE_BIN=$1
      shift
      ;;
  esac
done

if [ -z "$FIRMWARE_BIN" ]; then
  FIRMWARE_BIN="$SCRIPT_DIR/work/pokepod-build/output/$FLASH_MODE/PokePodAmoled.ino.bin"
  MODE_EXPLICIT=1
fi
MANIFEST_PATH=$(dirname -- "$FIRMWARE_BIN")/artifact.json
ESPTOOL_BIN=${ESPTOOL_BIN:-$(find "$HOME/Library/Arduino15/packages/esp32/tools/esptool_py" \
  -type f -name esptool -perm +111 -print 2>/dev/null | sort | tail -1)}

if [ ! -s "$FIRMWARE_BIN" ]; then
  printf 'FAIL firmware_binary_missing path=%s\n' "$FIRMWARE_BIN" >&2
  exit 71
fi
if [ -z "$ESPTOOL_BIN" ] || [ ! -x "$ESPTOOL_BIN" ]; then
  printf 'FAIL esptool_missing\n' >&2
  exit 72
fi
if [ ! -s "$MANIFEST_PATH" ]; then
  printf 'FAIL artifact_manifest_missing path=%s\n' "$MANIFEST_PATH" >&2
  exit 75
fi
EXPECTED_MODE=
if [ "$MODE_EXPLICIT" -eq 1 ]; then
  EXPECTED_MODE=$FLASH_MODE
fi
python3 - "$MANIFEST_PATH" "$FIRMWARE_BIN" "$EXPECTED_MODE" <<'PY'
import hashlib
import json
import pathlib
import sys

manifest_path = pathlib.Path(sys.argv[1])
binary_path = pathlib.Path(sys.argv[2])
expected_lane = sys.argv[3]
try:
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
except (OSError, json.JSONDecodeError) as exc:
    raise SystemExit(f"FAIL artifact_manifest_invalid error={exc}")
if manifest.get("schemaVersion") != 1 or manifest.get("kind") != "hardmac.artifact":
    raise SystemExit("FAIL artifact_manifest_kind")
lane = manifest.get("lane")
if lane not in {"fast", "release"}:
    raise SystemExit("FAIL artifact_manifest_lane")
if expected_lane and lane != expected_lane:
    raise SystemExit(f"FAIL artifact_lane_mismatch expected={expected_lane} actual={lane}")
binary = manifest.get("binary")
if not isinstance(binary, dict) or binary.get("file") != binary_path.name:
    raise SystemExit("FAIL artifact_manifest_binary_name")
payload = binary_path.read_bytes()
if binary.get("sizeBytes") != len(payload):
    raise SystemExit("FAIL artifact_manifest_binary_size")
actual_sha = hashlib.sha256(payload).hexdigest()
if binary.get("sha256") != actual_sha:
    raise SystemExit("FAIL artifact_manifest_binary_sha256")
print(f"PASS artifact_manifest lane={lane} sha256={actual_sha}")
PY
FLASH_STARTED_AT=$(date +%s)

find_ports() {
  find /dev -maxdepth 1 -name 'cu.usbmodem*' -print 2>/dev/null | sort
}

APP_PORT=""
PORTS_BEFORE=$(find_ports)
for port in $PORTS_BEFORE; do
  if "$SCRIPT_DIR/cdc-status.py" "$port" --timeout 1 >/dev/null 2>&1; then
    APP_PORT=$port
    break
  fi
done

if [ -n "$APP_PORT" ]; then
  # Arduino-ESP32 USBCDC recognizes 1200 baud as a request to enter the ROM
  # USB Serial/JTAG downloader. This replaces the BOOT+RESET hand sequence.
  stty -f "$APP_PORT" 1200
fi

ROM_PORT=${POKEPOD_ROM_PORT:-}
DEADLINE=$(( $(date +%s) + 8 ))
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  [ -n "$ROM_PORT" ] && break
  for port in $(find_ports); do
    if [ -n "$APP_PORT" ] &&
       ! printf '%s\n' "$PORTS_BEFORE" | grep -Fxq "$port"; then
      ROM_PORT=$port
      break
    fi
  done
  if [ -z "$APP_PORT" ] && [ -z "$ROM_PORT" ]; then
    CURRENT_PORTS=$(find_ports)
    if [ "$(printf '%s\n' "$CURRENT_PORTS" | sed '/^$/d' | wc -l | tr -d ' ')" -eq 1 ]; then
      ROM_PORT=$CURRENT_PORTS
    fi
  fi
  [ -n "$ROM_PORT" ] && break
  sleep 0.25
done

if [ -z "$ROM_PORT" ]; then
  printf '%s\n' \
    'FAIL rom_port_missing; hold BOOT, tap RESET once, then release BOOT and rerun' >&2
  exit 73
fi

WRITE_STARTED_AT=$(date +%s)
FALLBACK_USED=0
if ! "$ESPTOOL_BIN" --chip esp32s3 --port "$ROM_PORT" --baud 460800 \
  --before no-reset --after no-reset write-flash 0x10000 "$FIRMWARE_BIN"
then
  FALLBACK_USED=1
  printf 'WARN whole_image_write_failed; retrying 65536-byte chunks\n' >&2
  CHUNK_DIR=$(mktemp -d "${TMPDIR:-/tmp}/pokepod-flash.XXXXXX")
  trap 'rm -rf -- "$CHUNK_DIR"' EXIT HUP INT TERM
  CHUNK_SIZE=65536
  FIRMWARE_SIZE=$(stat -f %z "$FIRMWARE_BIN")
  CHUNK_COUNT=$(( (FIRMWARE_SIZE + CHUNK_SIZE - 1) / CHUNK_SIZE ))
  CHUNK_INDEX=0
  while [ "$CHUNK_INDEX" -lt "$CHUNK_COUNT" ]; do
    CHUNK_PATH="$CHUNK_DIR/chunk-$CHUNK_INDEX.bin"
    dd if="$FIRMWARE_BIN" of="$CHUNK_PATH" bs="$CHUNK_SIZE" \
      skip="$CHUNK_INDEX" count=1 2>/dev/null
    CHUNK_OFFSET=$((0x10000 + CHUNK_INDEX * CHUNK_SIZE))
    CHUNK_OFFSET_HEX=$(printf '0x%x' "$CHUNK_OFFSET")
    CHUNK_ATTEMPT=1
    CHUNK_WRITTEN=0
    while [ "$CHUNK_ATTEMPT" -le 3 ]; do
      if "$ESPTOOL_BIN" --chip esp32s3 --port "$ROM_PORT" --baud 115200 \
        --before no-reset --after no-reset write-flash \
        "$CHUNK_OFFSET_HEX" "$CHUNK_PATH"
      then
        CHUNK_WRITTEN=1
        break
      fi
      printf 'WARN chunk_write_retry index=%s attempt=%s\n' \
        "$CHUNK_INDEX" "$CHUNK_ATTEMPT" >&2
      CHUNK_ATTEMPT=$((CHUNK_ATTEMPT + 1))
    done
    if [ "$CHUNK_WRITTEN" -ne 1 ]; then
      printf 'FAIL chunk_write index=%s\n' "$CHUNK_INDEX" >&2
      exit 76
    fi
    CHUNK_INDEX=$((CHUNK_INDEX + 1))
  done
fi
WRITE_SECONDS=$(( $(date +%s) - WRITE_STARTED_AT ))
VERIFY_STARTED_AT=$(date +%s)
VERIFY_FALLBACK_USED=0
if ! "$ESPTOOL_BIN" --chip esp32s3 --port "$ROM_PORT" --baud 460800 \
  --before no-reset --after no-reset verify-flash 0x10000 "$FIRMWARE_BIN"
then
  VERIFY_FALLBACK_USED=1
  printf 'WARN whole_image_verify_failed; retrying at 115200 baud\n' >&2
  "$ESPTOOL_BIN" --chip esp32s3 --port "$ROM_PORT" --baud 115200 \
    --before no-reset --after no-reset verify-flash 0x10000 "$FIRMWARE_BIN"
fi
VERIFY_SECONDS=$(( $(date +%s) - VERIFY_STARTED_AT ))
# ESP32-S3's native USB Serial/JTAG RTS reset only resets the cores.  A chip
# that entered the ROM downloader through USB would keep the sampled BOOT
# strap and remain in download mode.  The watchdog reset is a full system
# reset, so the strap is sampled again and the application starts without a
# manual RESET press.
"$ESPTOOL_BIN" --chip esp32s3 --port "$ROM_PORT" \
  --before no-reset --after watchdog-reset run >/dev/null

REBOOT_STARTED_AT=$(date +%s)
DEADLINE=$(( $(date +%s) + 10 ))
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  sleep 0.5
  for port in $(find_ports); do
    if "$SCRIPT_DIR/cdc-status.py" "$port" --timeout 1 >/dev/null 2>&1; then
      printf 'PASS pokepod_flash_verified port=%s sha256=%s\n' \
        "$port" "$(shasum -a 256 "$FIRMWARE_BIN" | awk '{print $1}')"
      printf 'TIMING write_seconds=%s verify_seconds=%s reboot_seconds=%s total_seconds=%s write_fallback_used=%s verify_fallback_used=%s\n' \
        "$WRITE_SECONDS" "$VERIFY_SECONDS" \
        "$(( $(date +%s) - REBOOT_STARTED_AT ))" \
        "$(( $(date +%s) - FLASH_STARTED_AT ))" "$FALLBACK_USED" \
        "$VERIFY_FALLBACK_USED"
      exit 0
    fi
  done
done

printf 'FAIL application_cdc_did_not_return\n' >&2
exit 74
