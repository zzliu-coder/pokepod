#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TEST_TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/pokepod-host-tests.XXXXXX")
trap 'rm -rf -- "$TEST_TMP_DIR"' EXIT HUP INT TERM

for source in "$SCRIPT_DIR"/tests/test_*.cpp; do
  name=$(basename "$source" .cpp)
  clang++ -std=c++17 -Wall -Wextra -Werror \
    -I"$SCRIPT_DIR/PokePodAmoled" \
    "$source" -o "$TEST_TMP_DIR/$name"
  "$TEST_TMP_DIR/$name"
  printf 'PASS %s\n' "$name"
done

python3 "$SCRIPT_DIR/../tools/test-provisioning-page.py"
python3 "$SCRIPT_DIR/../tools/test-device-config-atomic.py"
python3 "$SCRIPT_DIR/../tools/test-cjk-font.py"
python3 "$SCRIPT_DIR/../tools/test-fixed-font-coverage.py"
python3 "$SCRIPT_DIR/../tools/test-wireless-sync-ui-contract.py"
python3 "$SCRIPT_DIR/../tools/test-usb-connection-contract.py"
python3 "$SCRIPT_DIR/../tools/test-flash-policy.py"
python3 "$SCRIPT_DIR/../tools/test-power-diagnostics-contract.py"
python3 "$SCRIPT_DIR/../tools/test-scroll-contract.py"
python3 "$SCRIPT_DIR/../tools/test-wireless-sync-contract.py"
python3 "$SCRIPT_DIR/../tools/test-link-buffer-policy.py"
python3 "$SCRIPT_DIR/../tools/test-tencent-network-contract.py"

if rg -q 'USBAudioCard|USBHIDKeyboard|UsbVoiceBridge|dictate-start|dictate-stop' \
  "$SCRIPT_DIR/PokePodAmoled" "$SCRIPT_DIR/build.sh"
then
  printf 'FAIL legacy_usb_voice_surface\n' >&2
  exit 1
fi
rg -q 'class UsbLinkBridge' "$SCRIPT_DIR/PokePodAmoled/UsbLinkBridge.h"
rg -q 'class BleVoiceService' "$SCRIPT_DIR/PokePodAmoled/BleVoiceService.h"
rg -q 'CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1' "$SCRIPT_DIR/build.sh"
rg -q 'requires a single NimBLE controller connection' \
  "$SCRIPT_DIR/PokePodAmoled/BleVoiceService.h"
printf 'PASS wireless_voice_deletion_contract\n'
