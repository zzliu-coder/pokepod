#!/bin/sh
set -eu

USER_HOME=$(dscl . -read "/Users/$(id -un)" NFSHomeDirectory 2>/dev/null | awk '{print $2}')
HOTKEY_PLIST="$USER_HOME/Library/Preferences/com.apple.symbolichotkeys.plist"
HITOOLBOX_PLIST=$(mktemp "${TMPDIR:-/tmp}/pokepod-hitoolbox.XXXXXX")
AUDIO_JSON=$(mktemp "${TMPDIR:-/tmp}/pokepod-audio.XXXXXX")
trap 'rm -f "$HITOOLBOX_PLIST" "$AUDIO_JSON"' EXIT
defaults export com.apple.HIToolbox "$HITOOLBOX_PLIST"

if ! /usr/bin/python3 - "$HITOOLBOX_PLIST" <<'PY'
import plistlib
import sys

with open(sys.argv[1], "rb") as source:
    settings = plistlib.load(source)
selected = settings.get("AppleSelectedInputSources", [])
if not any(item.get("Bundle ID") == "com.tencent.inputmethod.wetype"
           for item in selected if isinstance(item, dict)):
    raise SystemExit(1)
PY
then
  printf 'FAIL wetype_not_selected_input_source\n'
  exit 42
fi

if ! pgrep -x WeType >/dev/null; then
  printf 'FAIL wetype_process_not_running\n'
  exit 43
fi

WETYPE_SETTINGS="$USER_HOME/Library/Application Support/WeType/mmkv/wetype.settings"
if ! /usr/bin/python3 - "$WETYPE_SETTINGS" <<'PY'
import sys

with open(sys.argv[1], "rb") as source:
    payload = source.read()
marker = b"voicePTTShortcut_keyCodes"
offset = payload.find(marker)
if offset < 0 or b"[58,6]" not in payload[offset:offset + 128]:
    raise SystemExit(1)
PY
then
  printf 'FAIL wetype_ptt_shortcut_expected_option_z\n'
  exit 44
fi

if /usr/bin/python3 - "$HOTKEY_PLIST" <<'PY'
import plistlib
import sys

with open(sys.argv[1], "rb") as source:
    settings = plistlib.load(source)
hotkey = settings.get("AppleSymbolicHotKeys", {}).get("164", {})
parameters = hotkey.get("value", {}).get("parameters", [])
raise SystemExit(0 if hotkey.get("enabled") is True and
                 parameters == [122, 6, 524288] else 1)
PY
then
  printf 'FAIL apple_dictation_conflicts_with_wetype_option_z\n'
  exit 45
fi

system_profiler SPAudioDataType -json >"$AUDIO_JSON"
if ! /usr/bin/python3 - "$AUDIO_JSON" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as source:
    payload = json.load(source)

def dictionaries(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from dictionaries(child)
    elif isinstance(value, list):
        for child in value:
            yield from dictionaries(child)

matches = [item for item in dictionaries(payload)
           if item.get("_name") == "TinyUSB UAC1"
           and item.get("coreaudio_device_manufacturer") == "PokeCapsule"]
if not matches:
    raise SystemExit(1)
if not any(item.get("coreaudio_default_audio_input_device") == "spaudio_yes"
           for item in matches):
    raise SystemExit(2)
PY
then
  printf 'FAIL tinyusb_uac1_not_default_input\n'
  exit 46
fi

printf 'PASS mac_wetype_voice shortcut=hold_option_z microphone=tinyusb_uac1 input=wetype\n'
