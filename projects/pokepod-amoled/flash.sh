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
HARDMAC_SKILL_DIR=${HARDMAC_SKILL_DIR:-"${CODEX_HOME:-$HOME/.codex}/skills/hardmac"}
TRANSFER_SCRIPT=${HARDMAC_ESP32_TRANSFER:-"$HARDMAC_SKILL_DIR/scripts/esp32_region_transfer.py"}
ARTIFACT_VALIDATOR="$SCRIPT_DIR/tools/validate-flash-artifact.py"

if [ ! -s "$FIRMWARE_BIN" ]; then
  printf 'FAIL firmware_binary_missing path=%s\n' "$FIRMWARE_BIN" >&2
  exit 71
fi
if [ -z "$ESPTOOL_BIN" ] || [ ! -x "$ESPTOOL_BIN" ]; then
  printf 'FAIL esptool_missing\n' >&2
  exit 72
fi
if [ ! -s "$TRANSFER_SCRIPT" ]; then
  printf 'FAIL hardmac_transfer_missing path=%s\n' "$TRANSFER_SCRIPT" >&2
  exit 77
fi
TRANSFER_CONTRACT=$(python3 "$TRANSFER_SCRIPT" --version 2>/dev/null || true)
if [ "$TRANSFER_CONTRACT" != "hardmac.esp32-region-transfer.v1" ]; then
  printf 'FAIL hardmac_transfer_contract expected=%s actual=%s\n' \
    'hardmac.esp32-region-transfer.v1' "$TRANSFER_CONTRACT" >&2
  exit 77
fi
if [ ! -s "$MANIFEST_PATH" ]; then
  printf 'FAIL artifact_manifest_missing path=%s\n' "$MANIFEST_PATH" >&2
  exit 75
fi
if [ ! -s "$ARTIFACT_VALIDATOR" ]; then
  printf 'FAIL artifact_validator_missing path=%s\n' "$ARTIFACT_VALIDATOR" >&2
  exit 75
fi
EXPECTED_MODE=
if [ "$MODE_EXPLICIT" -eq 1 ]; then
  EXPECTED_MODE=$FLASH_MODE
fi
if [ -n "$EXPECTED_MODE" ]; then
  python3 "$ARTIFACT_VALIDATOR" --manifest "$MANIFEST_PATH" \
    --binary "$FIRMWARE_BIN" --expected-lane "$EXPECTED_MODE"
else
  python3 "$ARTIFACT_VALIDATOR" --manifest "$MANIFEST_PATH" \
    --binary "$FIRMWARE_BIN"
fi
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

RUN_ROOT="$SCRIPT_DIR/work/hardmac-runs"
mkdir -p "$RUN_ROOT"
RUN_DIR=$(mktemp -d "$RUN_ROOT/$(date +%Y%m%d-%H%M%S)-flash.XXXXXX")
IDENTITY_LOG="$RUN_DIR/chip-id.log"
IDENTITY_OK=0
IDENTITY_ATTEMPT=1
while [ "$IDENTITY_ATTEMPT" -le 3 ]; do
  if "$ESPTOOL_BIN" --chip esp32s3 --port "$ROM_PORT" --baud 115200 \
    --before usb-reset --after no-reset --no-stub chip-id \
    >"$IDENTITY_LOG" 2>&1
  then
    IDENTITY_OK=1
    break
  fi
  printf 'WARN chip_identity_retry attempt=%s\n' "$IDENTITY_ATTEMPT" >&2
  IDENTITY_ATTEMPT=$((IDENTITY_ATTEMPT + 1))
done
cat "$IDENTITY_LOG"
if [ "$IDENTITY_OK" -ne 1 ]; then
  printf 'FAIL chip_identity_unavailable run_dir=%s\n' "$RUN_DIR" >&2
  exit 78
fi
DEVICE_KEY=$(python3 - "$IDENTITY_LOG" <<'PY'
import pathlib
import re
import sys

text = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8", errors="replace")
match = re.search(r"(?im)^MAC:\s*((?:[0-9a-f]{2}:){5}[0-9a-f]{2})\s*$", text)
if not match:
    raise SystemExit(1)
print("esp32s3-" + match.group(1).lower().replace(":", ""))
PY
) || {
  printf 'FAIL chip_identity_parse run_dir=%s\n' "$RUN_DIR" >&2
  exit 78
}

# Capture and verify the exact region that this operation can overwrite before
# the first write.  A fresh backup avoids relying on stale device or deployed-
# state assumptions, and the restore plan remains beside all other run evidence.
BACKUP_DIR="$RUN_DIR/backup"
BACKUP_BIN="$BACKUP_DIR/current-app0.bin"
python3 "$TRANSFER_SCRIPT" backup \
  --esptool "$ESPTOOL_BIN" \
  --port "$ROM_PORT" \
  --device-key "$DEVICE_KEY" \
  --chip esp32s3 \
  --offset 0x10000 \
  --size 0x300000 \
  --run-dir "$BACKUP_DIR" \
  --output "$BACKUP_BIN" \
  --chunk-size 16384 \
  --attempts 3 \
  --baud 115200 \
  --before usb-reset \
  --stub disabled
python3 - "$RUN_DIR" "$BACKUP_BIN" "$DEVICE_KEY" "$ESPTOOL_BIN" \
  "$TRANSFER_SCRIPT" <<'PY'
import hashlib
import json
import pathlib
import sys

run_dir = pathlib.Path(sys.argv[1])
backup = pathlib.Path(sys.argv[2]).resolve()
restore = {
    "schemaVersion": 1,
    "kind": "hardmac.restore-plan",
    "deviceKey": sys.argv[3],
    "region": {"name": "app0", "offset": "0x10000", "sizeBytes": 0x300000},
    "backup": {
        "path": str(backup),
        "sizeBytes": backup.stat().st_size,
        "sha256": hashlib.sha256(backup.read_bytes()).hexdigest(),
        "verification": "sampled device reread plus assembled SHA-256",
    },
    "portAssumption": "replace <ROM_PORT> with the same verified device in ROM mode",
    "command": [
        "python3", sys.argv[5], "flash",
        "--esptool", sys.argv[4],
        "--port", "<ROM_PORT>",
        "--device-key", sys.argv[3],
        "--chip", "esp32s3",
        "--offset", "0x10000",
        "--run-dir", str((run_dir / "restore-transfer").resolve()),
        "--artifact", str(backup),
        "--max-size", "0x300000",
        "--chunk-size", "16384",
        "--attempts", "3",
        "--baud", "115200",
        "--before", "usb-reset",
        "--stub", "disabled",
    ],
}
(run_dir / "restore-plan.json").write_text(
    json.dumps(restore, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
    encoding="utf-8",
)
PY

python3 "$TRANSFER_SCRIPT" flash \
  --esptool "$ESPTOOL_BIN" \
  --port "$ROM_PORT" \
  --device-key "$DEVICE_KEY" \
  --chip esp32s3 \
  --offset 0x10000 \
  --run-dir "$RUN_DIR/transfer" \
  --chunk-size 16384 \
  --attempts 3 \
  --baud 115200 \
  --before usb-reset \
  --stub disabled \
  --artifact "$FIRMWARE_BIN" \
  --max-size 0x300000

# ESP32-S3 native USB reliably leaves ROM mode after a watchdog reset.  Use a
# read-only chip query to establish the link, then request the full reset.
RESET_OK=0
RESET_ATTEMPT=1
while [ "$RESET_ATTEMPT" -le 3 ]; do
  if "$ESPTOOL_BIN" --chip esp32s3 --port "$ROM_PORT" --baud 115200 \
    --before usb-reset --after watchdog-reset --no-stub chip-id \
    >>"$RUN_DIR/reset.log" 2>&1
  then
    RESET_OK=1
    break
  fi
  printf 'WARN watchdog_reset_retry attempt=%s\n' "$RESET_ATTEMPT" >&2
  RESET_ATTEMPT=$((RESET_ATTEMPT + 1))
done
if [ "$RESET_OK" -ne 1 ]; then
  printf 'FAIL watchdog_reset run_dir=%s\n' "$RUN_DIR" >&2
  exit 79
fi

REBOOT_STARTED_AT=$(date +%s)
DEADLINE=$(( $(date +%s) + 10 ))
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  sleep 0.5
  for port in $(find_ports); do
    if "$SCRIPT_DIR/cdc-status.py" "$port" --timeout 1 >/dev/null 2>&1; then
      python3 - "$RUN_DIR" "$MANIFEST_PATH" "$FIRMWARE_BIN" "$DEVICE_KEY" \
        "$APP_PORT" "$ROM_PORT" "$port" <<'PY'
import hashlib
import json
import pathlib
import sys
from datetime import datetime, timezone

run_dir = pathlib.Path(sys.argv[1])
manifest = json.loads(pathlib.Path(sys.argv[2]).read_text(encoding="utf-8"))
transfer = json.loads((run_dir / "transfer/result.json").read_text(encoding="utf-8"))
record = {
    "schemaVersion": 1,
    "kind": "hardmac.run",
    "createdAt": datetime.now(timezone.utc).isoformat(),
    "profile": ".hardmac/workflow.json",
    "lane": manifest["lane"],
    "deviceKey": sys.argv[4],
    "applicationPortBefore": sys.argv[5],
    "romPort": sys.argv[6],
    "applicationPortAfter": sys.argv[7],
    "artifact": {
        "path": str(pathlib.Path(sys.argv[3]).resolve()),
        "sizeBytes": pathlib.Path(sys.argv[3]).stat().st_size,
        "sha256": hashlib.sha256(pathlib.Path(sys.argv[3]).read_bytes()).hexdigest(),
    },
    "transfer": transfer,
    "runtimeReturn": "passed",
    "remainingAcceptance": ["run the changed subsystem's real-device checks"],
}
(run_dir / "run.json").write_text(
    json.dumps(record, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
    encoding="utf-8",
)
PY
      TRANSFER_TIMING=$(python3 - "$RUN_DIR/transfer/result.json" <<'PY'
import json
import pathlib
import sys

value = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
print(f"write_seconds={value['writeSeconds']} readback_seconds={value['readbackSeconds']}")
PY
)
      printf 'PASS pokepod_flash_verified port=%s sha256=%s\n' \
        "$port" "$(shasum -a 256 "$FIRMWARE_BIN" | awk '{print $1}')"
      printf 'TIMING %s reboot_seconds=%s total_seconds=%s\n' \
        "$TRANSFER_TIMING" "$(( $(date +%s) - REBOOT_STARTED_AT ))" \
        "$(( $(date +%s) - FLASH_STARTED_AT ))"
      printf 'EVIDENCE run_dir=%s\n' "$RUN_DIR"
      exit 0
    fi
  done
done

printf 'FAIL application_cdc_did_not_return\n' >&2
exit 74
