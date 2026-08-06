#!/bin/sh
set -eu

DICTATION_ENABLED=$(defaults read com.apple.HIToolbox AppleDictationAutoEnable 2>/dev/null || printf '0')
if [ "$DICTATION_ENABLED" != "1" ]; then
  printf 'FAIL macos_dictation_disabled\n'
  exit 42
fi

USER_HOME=$(dscl . -read "/Users/$(id -un)" NFSHomeDirectory 2>/dev/null | awk '{print $2}')
HOTKEY_PLIST="$USER_HOME/Library/Preferences/com.apple.symbolichotkeys.plist"
if ! /usr/bin/python3 - "$HOTKEY_PLIST" <<'PY'
import plistlib
import sys

with open(sys.argv[1], "rb") as source:
    settings = plistlib.load(source)
hotkey = settings.get("AppleSymbolicHotKeys", {}).get("164", {})
parameters = hotkey.get("value", {}).get("parameters", [])
if hotkey.get("enabled") is not True or parameters != [122, 6, 524288]:
    raise SystemExit(1)
PY
then
  printf 'FAIL dictation_shortcut_expected_option_z\n'
  exit 43
fi

AUDIO_JSON=$(mktemp "${TMPDIR:-/tmp}/pokepod-audio.XXXXXX")
trap 'rm -f "$AUDIO_JSON"' EXIT
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
  exit 44
fi

printf 'PASS mac_dictation shortcut=option_z microphone=tinyusb_uac1 permissions=not_required\n'
