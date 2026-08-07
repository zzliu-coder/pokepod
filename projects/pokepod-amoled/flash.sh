#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
FIRMWARE_BIN=${1:-"$SCRIPT_DIR/work/pokepod-build/output/PokePodAmoled.ino.bin"}
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

"$ESPTOOL_BIN" --chip esp32s3 --port "$ROM_PORT" --baud 460800 \
  --before no-reset --after no-reset write-flash 0x10000 "$FIRMWARE_BIN"
"$ESPTOOL_BIN" --chip esp32s3 --port "$ROM_PORT" --baud 460800 \
  --before no-reset --after no-reset verify-flash 0x10000 "$FIRMWARE_BIN"
# ESP32-S3's native USB Serial/JTAG RTS reset only resets the cores.  A chip
# that entered the ROM downloader through USB would keep the sampled BOOT
# strap and remain in download mode.  The watchdog reset is a full system
# reset, so the strap is sampled again and the application starts without a
# manual RESET press.
"$ESPTOOL_BIN" --chip esp32s3 --port "$ROM_PORT" \
  --before no-reset --after watchdog-reset run >/dev/null

DEADLINE=$(( $(date +%s) + 10 ))
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
  sleep 0.5
  for port in $(find_ports); do
    if "$SCRIPT_DIR/cdc-status.py" "$port" --timeout 1 >/dev/null 2>&1; then
      printf 'PASS pokepod_flash_verified port=%s sha256=%s\n' \
        "$port" "$(shasum -a 256 "$FIRMWARE_BIN" | awk '{print $1}')"
      exit 0
    fi
  done
done

printf 'FAIL application_cdc_did_not_return\n' >&2
exit 74
