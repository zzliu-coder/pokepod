#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
python3 "$PROJECT_DIR/tools/run-cpp-host-tests.py"

python3 "$PROJECT_DIR/tools/run-python-test-gate.py" source

if rg -q 'USBAudioCard|USBHIDKeyboard|UsbVoiceBridge|dictate-start|dictate-stop' \
  "$SCRIPT_DIR/PokePodAmoled" "$SCRIPT_DIR/build.sh"
then
  printf 'FAIL legacy_usb_voice_surface\n' >&2
  exit 1
fi
rg -q 'class UsbLinkBridge' "$SCRIPT_DIR/PokePodAmoled/UsbLinkBridge.h"
rg -q 'class BleVoiceService' "$SCRIPT_DIR/PokePodAmoled/BleVoiceService.h"
rg -q 'CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1' "$SCRIPT_DIR/build.sh"
rg -q 'CONFIG_BT_CTRL_BLE_LLCP_CONN_UPDATE=y' "$SCRIPT_DIR/build.sh"
rg -q 'enableLoopWDT();' "$SCRIPT_DIR/PokePodAmoled/PokePodApp.cpp"
rg -q 'requires a single NimBLE controller connection' \
  "$SCRIPT_DIR/PokePodAmoled/BleVoiceService.h"
printf 'PASS wireless_voice_deletion_contract\n'
printf 'PASS source_only_gate\n'
