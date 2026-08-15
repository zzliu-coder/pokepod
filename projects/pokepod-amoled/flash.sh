#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
FLASH_MODE=release
MODE_EXPLICIT=0
FIRMWARE_BIN=
IDENTITY_AUTHORITY=
RESCUE_TARGET_SLOT=
RESCUE_KNOWN_GOOD_SLOT=
RESCUE_MODE=0
ROM_PORT=${POKEPOD_ROM_PORT:-}
if [ -n "$ROM_PORT" ]; then
  ROM_PORT_EXPLICIT=1
else
  ROM_PORT_EXPLICIT=0
fi

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
    --identity-authority)
      [ "$#" -ge 2 ] || { printf 'FAIL identity_authority_argument_missing\n' >&2; exit 64; }
      IDENTITY_AUTHORITY=$2
      shift 2
      ;;
    --rom-rescue)
      RESCUE_MODE=1
      shift
      ;;
    --rescue-target)
      [ "$#" -ge 2 ] || { printf 'FAIL rescue_target_argument_missing\n' >&2; exit 64; }
      RESCUE_TARGET_SLOT=$2
      RESCUE_MODE=1
      shift 2
      ;;
    --known-good-slot)
      [ "$#" -ge 2 ] || { printf 'FAIL known_good_slot_argument_missing\n' >&2; exit 64; }
      RESCUE_KNOWN_GOOD_SLOT=$2
      RESCUE_MODE=1
      shift 2
      ;;
    --rom-port)
      [ "$#" -ge 2 ] || { printf 'FAIL rom_port_argument_missing\n' >&2; exit 64; }
      ROM_PORT=$2
      ROM_PORT_EXPLICIT=1
      shift 2
      ;;
    --help)
      cat <<'EOF'
Usage: ./flash.sh [--release|--fast] [--firmware PATH] --identity-authority PATH [--rom-port PATH]

  --release   Flash the separately built release artifact (default).
  --fast      Explicitly flash the fast iteration artifact.
  --firmware  Use a manifest-backed artifact at an explicit path.
  --identity-authority
              Required private JSON authority for one expected PokePod.
              It binds the Link deviceId and board variant to the ROM eFuse
              MAC, ESP32-S3 chip family and 16 MiB flash before backup/write.
  --rom-rescue
              Enable explicit dual-slot ROM rescue. Requires both
              --rescue-target and --known-good-slot; only the target slot and
              the OTA selector may be written.
  --rescue-target app0|app1
              One application slot to receive the candidate image.
  --known-good-slot app0|app1
              The other slot whose bytes are backed up, rechecked and never
              written by this operation.
  --rom-port  Explicit ROM port for recovery when no application identity
              answers. Required in recovery; the script never guesses a ROM.
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

if [ -z "$IDENTITY_AUTHORITY" ]; then
  printf 'FAIL identity_authority_required\n' >&2
  exit 64
fi
if [ "$RESCUE_MODE" -eq 1 ]; then
  case "$RESCUE_TARGET_SLOT" in
    app0|app1) ;;
    *) printf 'FAIL rescue_target_required value=%s\n' "$RESCUE_TARGET_SLOT" >&2; exit 64 ;;
  esac
  case "$RESCUE_KNOWN_GOOD_SLOT" in
    app0|app1) ;;
    *) printf 'FAIL known_good_slot_required value=%s\n' "$RESCUE_KNOWN_GOOD_SLOT" >&2; exit 64 ;;
  esac
  if [ "$RESCUE_TARGET_SLOT" = "$RESCUE_KNOWN_GOOD_SLOT" ]; then
    printf 'FAIL rescue_target_and_known_good_must_differ\n' >&2
    exit 64
  fi
fi
if [ ! -s "$IDENTITY_AUTHORITY" ]; then
  printf 'FAIL identity_authority_missing path=%s\n' "$IDENTITY_AUTHORITY" >&2
  exit 76
fi

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
IDENTITY_VALIDATOR="$SCRIPT_DIR/tools/validate-flash-identity.py"
RESCUE_PARTITION_VALIDATOR="$SCRIPT_DIR/tools/validate-rescue-partitions.py"

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
if [ ! -s "$IDENTITY_VALIDATOR" ]; then
  printf 'FAIL identity_validator_missing path=%s\n' "$IDENTITY_VALIDATOR" >&2
  exit 76
fi
if [ "$RESCUE_MODE" -eq 1 ] && [ ! -s "$RESCUE_PARTITION_VALIDATOR" ]; then
  printf 'FAIL rescue_partition_validator_missing path=%s\n' "$RESCUE_PARTITION_VALIDATOR" >&2
  exit 76
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

RUN_ROOT="$SCRIPT_DIR/work/hardmac-runs"
mkdir -p "$RUN_ROOT"
RUN_DIR=$(mktemp -d "$RUN_ROOT/$(date +%Y%m%d-%H%M%S)-flash.XXXXXX")
umask 077
AUTHORITY_SUMMARY="$RUN_DIR/identity-authority-summary.json"
python3 "$IDENTITY_VALIDATOR" authority \
  --authority "$IDENTITY_AUTHORITY" --output "$AUTHORITY_SUMMARY"

find_ports() {
  find /dev -maxdepth 1 -name 'cu.usbmodem*' -print 2>/dev/null | sort
}

APP_PORT=""
PORTS_BEFORE=$(find_ports)
APP_IDENTITY="$RUN_DIR/application-identity.json"
APP_IDENTITY_RESPONSES=0
APP_IDENTITY_CANDIDATE=0
for port in $PORTS_BEFORE; do
  APP_IDENTITY_CANDIDATE=$((APP_IDENTITY_CANDIDATE + 1))
  CANDIDATE_PATH="$RUN_DIR/application-identity-candidate-$APP_IDENTITY_CANDIDATE.json"
  if python3 "$IDENTITY_VALIDATOR" capture-application \
    --probe "$SCRIPT_DIR/cdc-status.py" --port "$port" \
    --output "$CANDIDATE_PATH" --timeout 2 \
    >/dev/null 2>&1
  then
    APP_IDENTITY_RESPONSES=$((APP_IDENTITY_RESPONSES + 1))
    APP_PORT=$port
  fi
done

if [ "$APP_IDENTITY_RESPONSES" -gt 0 ]; then
  if ! python3 "$IDENTITY_VALIDATOR" select-application \
    --authority "$IDENTITY_AUTHORITY" --output "$APP_IDENTITY" \
    "$RUN_DIR"/application-identity-candidate-*.json >/dev/null
  then
    printf 'FAIL expected_application_identity_match responses=%s run_dir=%s\n' \
      "$APP_IDENTITY_RESPONSES" "$RUN_DIR" >&2
    exit 78
  fi
else
  if [ "$ROM_PORT_EXPLICIT" -ne 1 ] || [ -z "$ROM_PORT" ]; then
    printf 'FAIL recovery_requires_explicit_rom_port run_dir=%s\n' "$RUN_DIR" >&2
    exit 73
  fi
fi

if [ -n "$APP_PORT" ]; then
  # Arduino-ESP32 USBCDC recognizes 1200 baud as a request to enter the ROM
  # USB Serial/JTAG downloader. This replaces the BOOT+RESET hand sequence.
  stty -f "$APP_PORT" 1200
fi

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
  [ -n "$ROM_PORT" ] && break
  sleep 0.25
done

if [ -z "$ROM_PORT" ]; then
  printf '%s\n' \
    'FAIL rom_port_missing; hold BOOT, tap RESET once, then release BOOT and rerun' >&2
  exit 73
fi

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

FLASH_ID_LOG="$RUN_DIR/flash-id.log"
FLASH_ID_OK=0
FLASH_ID_ATTEMPT=1
while [ "$FLASH_ID_ATTEMPT" -le 3 ]; do
  if "$ESPTOOL_BIN" --chip esp32s3 --port "$ROM_PORT" --baud 115200 \
    --before usb-reset --after no-reset --no-stub flash-id \
    >"$FLASH_ID_LOG" 2>&1
  then
    FLASH_ID_OK=1
    break
  fi
  printf 'WARN flash_identity_retry attempt=%s\n' "$FLASH_ID_ATTEMPT" >&2
  FLASH_ID_ATTEMPT=$((FLASH_ID_ATTEMPT + 1))
done
cat "$FLASH_ID_LOG"
if [ "$FLASH_ID_OK" -ne 1 ]; then
  printf 'FAIL flash_identity_unavailable run_dir=%s\n' "$RUN_DIR" >&2
  exit 78
fi

IDENTITY_VERDICT="$RUN_DIR/identity-verdict.json"
if [ -n "$APP_PORT" ]; then
  DEVICE_KEY=$(python3 "$IDENTITY_VALIDATOR" evidence \
    --authority "$IDENTITY_AUTHORITY" --application "$APP_IDENTITY" \
    --chip-log "$IDENTITY_LOG" --flash-log "$FLASH_ID_LOG" \
    --output "$IDENTITY_VERDICT") || DEVICE_KEY=
else
  DEVICE_KEY=$(python3 "$IDENTITY_VALIDATOR" evidence \
    --authority "$IDENTITY_AUTHORITY" \
    --chip-log "$IDENTITY_LOG" --flash-log "$FLASH_ID_LOG" \
    --output "$IDENTITY_VERDICT") || DEVICE_KEY=
fi
if [ -z "$DEVICE_KEY" ]; then
  printf 'FAIL expected_device_identity_mismatch run_dir=%s\n' "$RUN_DIR" >&2
  exit 78
fi

if [ "$RESCUE_MODE" -eq 0 ]; then
  # Capture and verify the exact region that this operation can overwrite
  # before the first write.  A fresh backup avoids relying on stale device or
  # deployed-state assumptions, and the restore plan remains beside all other
  # run evidence.
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
else
  # ROM rescue is a separate, explicit lane.  Read and validate all mutable
  # boot surfaces before any write, then make a plan that proves the opposite
  # application slot is the known-good rollback slot.
  RESCUE_BACKUP_DIR="$RUN_DIR/rescue-backup"
  PARTITION_TABLE_BIN="$RESCUE_BACKUP_DIR/partition-table.bin"
  OTADATA_BIN="$RESCUE_BACKUP_DIR/otadata.bin"
  APP0_BIN="$RESCUE_BACKUP_DIR/current-app0.bin"
  APP1_BIN="$RESCUE_BACKUP_DIR/current-app1.bin"
  python3 "$TRANSFER_SCRIPT" backup \
    --esptool "$ESPTOOL_BIN" --port "$ROM_PORT" --device-key "$DEVICE_KEY" \
    --chip esp32s3 --offset 0x8000 --size 0x1000 \
    --run-dir "$RESCUE_BACKUP_DIR/partition-table" --output "$PARTITION_TABLE_BIN" \
    --chunk-size 4096 --attempts 3 --baud 115200 --before usb-reset --stub disabled
  python3 "$TRANSFER_SCRIPT" backup \
    --esptool "$ESPTOOL_BIN" --port "$ROM_PORT" --device-key "$DEVICE_KEY" \
    --chip esp32s3 --offset 0xE000 --size 0x2000 \
    --run-dir "$RESCUE_BACKUP_DIR/otadata" --output "$OTADATA_BIN" \
    --chunk-size 4096 --attempts 3 --baud 115200 --before usb-reset --stub disabled
  python3 "$TRANSFER_SCRIPT" backup \
    --esptool "$ESPTOOL_BIN" --port "$ROM_PORT" --device-key "$DEVICE_KEY" \
    --chip esp32s3 --offset 0x10000 --size 0x300000 \
    --run-dir "$RESCUE_BACKUP_DIR/app0" --output "$APP0_BIN" \
    --chunk-size 16384 --attempts 3 --baud 115200 --before usb-reset --stub disabled
  python3 "$TRANSFER_SCRIPT" backup \
    --esptool "$ESPTOOL_BIN" --port "$ROM_PORT" --device-key "$DEVICE_KEY" \
    --chip esp32s3 --offset 0x310000 --size 0x300000 \
    --run-dir "$RESCUE_BACKUP_DIR/app1" --output "$APP1_BIN" \
    --chunk-size 16384 --attempts 3 --baud 115200 --before usb-reset --stub disabled

  RESCUE_LAYOUT="$RUN_DIR/rescue-partition-layout.json"
  python3 "$RESCUE_PARTITION_VALIDATOR" layout \
    --partition-table "$PARTITION_TABLE_BIN" --output "$RESCUE_LAYOUT"
  RESCUE_PLAN="$RUN_DIR/rescue-plan.json"
  ARTIFACT_SIZE=$(wc -c <"$FIRMWARE_BIN" | tr -d ' ')
  python3 "$RESCUE_PARTITION_VALIDATOR" plan \
    --partition-table "$PARTITION_TABLE_BIN" \
    --target-slot "$RESCUE_TARGET_SLOT" \
    --known-good-slot "$RESCUE_KNOWN_GOOD_SLOT" \
    --artifact-size "$ARTIFACT_SIZE" --output "$RESCUE_PLAN"
  RESCUE_OFFSET=$(python3 - "$RESCUE_PLAN" <<'PY'
import json
import pathlib
import sys
value = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
print(hex(value["target"]["offset"]))
PY
)
  OTADATA_CANDIDATE="$RUN_DIR/otadata-candidate.bin"
  python3 "$RESCUE_PARTITION_VALIDATOR" otadata \
    --input "$OTADATA_BIN" --target-slot "$RESCUE_TARGET_SLOT" \
    --output "$OTADATA_CANDIDATE"

  python3 - "$RUN_DIR" "$DEVICE_KEY" "$PARTITION_TABLE_BIN" "$OTADATA_BIN" \
    "$APP0_BIN" "$APP1_BIN" "$RESCUE_TARGET_SLOT" "$RESCUE_KNOWN_GOOD_SLOT" \
    "$ESPTOOL_BIN" "$TRANSFER_SCRIPT" "$RESCUE_OFFSET" <<'PY'
import hashlib
import json
import pathlib
import sys

run_dir = pathlib.Path(sys.argv[1])
regions = {
    "partitionTable": (pathlib.Path(sys.argv[3]).resolve(), "0x8000", 0x1000),
    "otadata": (pathlib.Path(sys.argv[4]).resolve(), "0xE000", 0x2000),
    "app0": (pathlib.Path(sys.argv[5]).resolve(), "0x10000", 0x300000),
    "app1": (pathlib.Path(sys.argv[6]).resolve(), "0x310000", 0x300000),
}
backup = {}
for name, (path, offset, size) in regions.items():
    backup[name] = {
        "path": str(path),
        "offset": offset,
        "sizeBytes": size,
        "actualSizeBytes": path.stat().st_size,
        "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
    }
target = sys.argv[7]
known_good = sys.argv[8]
restore = {
    "schemaVersion": 1,
    "kind": "hardmac.rescue-restore-plan",
    "deviceKey": sys.argv[2],
    "targetSlot": target,
    "knownGoodSlot": known_good,
    "backup": backup,
    "writePolicy": {
        "partitionTable": "never-write",
        "otadata": "restore-from-backup-or-generated-candidate",
        "targetSlot": "write-candidate-only",
        "knownGoodSlot": "never-write",
    },
    "restoreCommands": {
        "targetSlot": [
            "python3", sys.argv[10], "flash", "--esptool", sys.argv[9],
            "--port", "<ROM_PORT>", "--device-key", sys.argv[2], "--chip", "esp32s3",
            "--offset", sys.argv[11], "--run-dir", str((run_dir / "restore-target").resolve()),
            "--artifact", backup[target]["path"], "--max-size", "0x300000",
        ],
        "otadata": [
            "python3", sys.argv[10], "flash", "--esptool", sys.argv[9],
            "--port", "<ROM_PORT>", "--device-key", sys.argv[2], "--chip", "esp32s3",
            "--offset", "0xE000", "--run-dir", str((run_dir / "restore-otadata").resolve()),
            "--artifact", backup["otadata"]["path"], "--max-size", "0x2000",
        ],
    },
    "operatorRule": "verify ROM identity again before any restore write",
}
(run_dir / "restore-plan.json").write_text(
    json.dumps(restore, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
    encoding="utf-8",
)
PY
fi

# Close the backup-to-write identity gap. A cable reconnect or port reuse can
# change which physical board is behind the same ROM path while the backup is
# running. Re-read both ROM identity surfaces immediately before the first
# write, bind them to the same authority, and require the original device key.
PREWRITE_IDENTITY_LOG="$RUN_DIR/prewrite-chip-id.log"
PREWRITE_IDENTITY_OK=0
PREWRITE_IDENTITY_ATTEMPT=1
while [ "$PREWRITE_IDENTITY_ATTEMPT" -le 3 ]; do
  if "$ESPTOOL_BIN" --chip esp32s3 --port "$ROM_PORT" --baud 115200 \
    --before usb-reset --after no-reset --no-stub chip-id \
    >"$PREWRITE_IDENTITY_LOG" 2>&1
  then
    PREWRITE_IDENTITY_OK=1
    break
  fi
  printf 'WARN prewrite_chip_identity_retry attempt=%s\n' \
    "$PREWRITE_IDENTITY_ATTEMPT" >&2
  PREWRITE_IDENTITY_ATTEMPT=$((PREWRITE_IDENTITY_ATTEMPT + 1))
done
cat "$PREWRITE_IDENTITY_LOG"
if [ "$PREWRITE_IDENTITY_OK" -ne 1 ]; then
  printf 'FAIL prewrite_chip_identity_unavailable run_dir=%s\n' "$RUN_DIR" >&2
  exit 78
fi

PREWRITE_FLASH_ID_LOG="$RUN_DIR/prewrite-flash-id.log"
PREWRITE_FLASH_ID_OK=0
PREWRITE_FLASH_ID_ATTEMPT=1
while [ "$PREWRITE_FLASH_ID_ATTEMPT" -le 3 ]; do
  if "$ESPTOOL_BIN" --chip esp32s3 --port "$ROM_PORT" --baud 115200 \
    --before usb-reset --after no-reset --no-stub flash-id \
    >"$PREWRITE_FLASH_ID_LOG" 2>&1
  then
    PREWRITE_FLASH_ID_OK=1
    break
  fi
  printf 'WARN prewrite_flash_identity_retry attempt=%s\n' \
    "$PREWRITE_FLASH_ID_ATTEMPT" >&2
  PREWRITE_FLASH_ID_ATTEMPT=$((PREWRITE_FLASH_ID_ATTEMPT + 1))
done
cat "$PREWRITE_FLASH_ID_LOG"
if [ "$PREWRITE_FLASH_ID_OK" -ne 1 ]; then
  printf 'FAIL prewrite_flash_identity_unavailable run_dir=%s\n' "$RUN_DIR" >&2
  exit 78
fi

PREWRITE_IDENTITY_VERDICT="$RUN_DIR/prewrite-identity-verdict.json"
PREWRITE_DEVICE_KEY=$(python3 "$IDENTITY_VALIDATOR" evidence \
  --authority "$IDENTITY_AUTHORITY" \
  --chip-log "$PREWRITE_IDENTITY_LOG" \
  --flash-log "$PREWRITE_FLASH_ID_LOG" \
  --expected-device-key "$DEVICE_KEY" \
  --output "$PREWRITE_IDENTITY_VERDICT") || PREWRITE_DEVICE_KEY=
if [ -z "$PREWRITE_DEVICE_KEY" ] || [ "$PREWRITE_DEVICE_KEY" != "$DEVICE_KEY" ]; then
  printf 'FAIL prewrite_device_identity_mismatch run_dir=%s\n' "$RUN_DIR" >&2
  exit 78
fi

if [ "$RESCUE_MODE" -eq 0 ]; then
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
else
  # The candidate is written to exactly one slot.  The selector is changed
  # only after that slot's full readback has completed.  The known-good slot
  # is never passed to a write command.
  python3 "$TRANSFER_SCRIPT" flash \
    --esptool "$ESPTOOL_BIN" --port "$ROM_PORT" --device-key "$DEVICE_KEY" \
    --chip esp32s3 --offset "$RESCUE_OFFSET" --run-dir "$RUN_DIR/transfer" \
    --chunk-size 16384 --attempts 3 --baud 115200 --before usb-reset --stub disabled \
    --artifact "$FIRMWARE_BIN" --max-size 0x300000
  python3 "$TRANSFER_SCRIPT" flash \
    --esptool "$ESPTOOL_BIN" --port "$ROM_PORT" --device-key "$DEVICE_KEY" \
    --chip esp32s3 --offset 0xE000 --run-dir "$RUN_DIR/otadata-transfer" \
    --chunk-size 4096 --attempts 3 --baud 115200 --before usb-reset --stub disabled \
    --artifact "$OTADATA_CANDIDATE" --max-size 0x2000

  # A complete post-write reread of the protected slot turns the "we did not
  # issue a write" claim into evidence about the actual device bytes.
  POST_KNOWN_GOOD="$RUN_DIR/postwrite-$RESCUE_KNOWN_GOOD_SLOT.bin"
  if [ "$RESCUE_KNOWN_GOOD_SLOT" = app0 ]; then
    POST_KNOWN_GOOD_OFFSET=0x10000
  else
    POST_KNOWN_GOOD_OFFSET=0x310000
  fi
  python3 "$TRANSFER_SCRIPT" backup \
    --esptool "$ESPTOOL_BIN" --port "$ROM_PORT" --device-key "$DEVICE_KEY" \
    --chip esp32s3 --offset "$POST_KNOWN_GOOD_OFFSET" --size 0x300000 \
    --run-dir "$RUN_DIR/postwrite-$RESCUE_KNOWN_GOOD_SLOT" --output "$POST_KNOWN_GOOD" \
    --chunk-size 16384 --attempts 3 --baud 115200 --before usb-reset --stub disabled
  python3 - "$POST_KNOWN_GOOD" "$RESCUE_KNOWN_GOOD_SLOT" "$APP0_BIN" "$APP1_BIN" <<'PY'
import hashlib
import pathlib
import sys

post = pathlib.Path(sys.argv[1])
slot = sys.argv[2]
before = pathlib.Path(sys.argv[3] if slot == "app0" else sys.argv[4])
post_sha = hashlib.sha256(post.read_bytes()).hexdigest()
before_sha = hashlib.sha256(before.read_bytes()).hexdigest()
if post_sha != before_sha:
    raise SystemExit(
        f"FAIL rescue_known_good_slot_changed slot={slot} "
        f"before={before_sha} after={post_sha}"
    )
print(f"PASS rescue_known_good_slot_preserved slot={slot} sha256={post_sha}")
PY
fi

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
    POST_IDENTITY="$RUN_DIR/application-identity-after.json"
    if python3 "$IDENTITY_VALIDATOR" capture-application \
      --authority "$IDENTITY_AUTHORITY" --probe "$SCRIPT_DIR/cdc-status.py" \
      --port "$port" --output "$POST_IDENTITY" --timeout 2 \
      >/dev/null 2>&1
    then
      if [ "$RESCUE_MODE" -eq 1 ]; then
        python3 - "$POST_IDENTITY" "$RESCUE_TARGET_SLOT" "$MANIFEST_PATH" <<'PY'
import json
import pathlib
import sys

identity = json.loads(pathlib.Path(sys.argv[1]).read_text(encoding="utf-8"))
target = sys.argv[2]
manifest_path = pathlib.Path(sys.argv[3])
manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
partition = identity.get("runningPartition")
if partition != target:
    raise SystemExit(
        f"FAIL rescue_running_partition expected={target} actual={partition}"
    )
source_revision = manifest.get("sourceRevision")
if not isinstance(source_revision, str) or identity.get("sourceRevision") != source_revision:
    raise SystemExit("FAIL rescue_source_revision_mismatch")
expected_elf = manifest.get("appElfSha256") or manifest.get("elfSha256")
review_meta = manifest.get("resourceReview")
review_ref = review_meta.get("evidenceFile") if isinstance(review_meta, dict) else None
if expected_elf is None and isinstance(review_ref, str):
    review_path = (manifest_path.parent / review_ref).resolve()
    review = json.loads(review_path.read_text(encoding="utf-8"))
    expected_elf = review.get("elf", {}).get("sha256")
actual_elf = identity.get("appElfSha256") or identity.get("elfSha256")
if not isinstance(expected_elf, str) or actual_elf != expected_elf:
    raise SystemExit("FAIL rescue_app_elf_sha_mismatch")
print(
    f"PASS rescue_runtime_identity partition={partition} "
    f"sourceRevision={source_revision} appElfSha256={actual_elf}"
)
PY
      fi
      python3 - "$RUN_DIR" "$MANIFEST_PATH" "$FIRMWARE_BIN" "$DEVICE_KEY" \
        "$APP_PORT" "$ROM_PORT" "$port" "$IDENTITY_VERDICT" <<'PY'
import hashlib
import json
import pathlib
import sys
from datetime import datetime, timezone

run_dir = pathlib.Path(sys.argv[1])
manifest = json.loads(pathlib.Path(sys.argv[2]).read_text(encoding="utf-8"))
transfer = json.loads((run_dir / "transfer/result.json").read_text(encoding="utf-8"))
identity = json.loads(pathlib.Path(sys.argv[8]).read_text(encoding="utf-8"))
record = {
    "schemaVersion": 1,
    "kind": "hardmac.run",
    "createdAt": datetime.now(timezone.utc).isoformat(),
    "profile": ".hardmac/workflow.json",
    "lane": manifest["lane"],
    "deviceKey": sys.argv[4],
    "identity": identity,
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
