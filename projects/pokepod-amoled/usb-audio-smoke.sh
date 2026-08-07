#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
RUN_ROOT=${1:-"$SCRIPT_DIR/work/usb-audio-smoke/$(date +%Y%m%d-%H%M%S)"}
mkdir -p "$RUN_ROOT"

FFMPEG=$(command -v ffmpeg || true)
FFPROBE=$(command -v ffprobe || true)
if [ -z "$FFMPEG" ] || [ -z "$FFPROBE" ]; then
  printf 'FAIL tool_missing ffmpeg_or_ffprobe\n' >&2
  exit 20
fi

SYSTEM_AUDIO="$RUN_ROOT/system-audio.txt"
DEVICE_LIST="$RUN_ROOT/avfoundation-devices.txt"
CAPTURE_LOG="$RUN_ROOT/capture.log"
WAV_FILE="$RUN_ROOT/tinyusb-uac1.wav"
LINK_PROBE="$RUN_ROOT/link-during-uac.json"
RESULT_FILE="$RUN_ROOT/result.txt"

system_profiler SPAudioDataType >"$SYSTEM_AUDIO"
if ! rg -q 'TinyUSB UAC1:' "$SYSTEM_AUDIO"; then
  printf 'FAIL audio_device_missing TinyUSB_UAC1\n' | tee "$RESULT_FILE"
  exit 21
fi

"$FFMPEG" -hide_banner -f avfoundation -list_devices true -i '' \
  >"$DEVICE_LIST" 2>&1 || true
AUDIO_INDEX=$(awk '/AVFoundation audio devices:/{audio=1; next} audio{print}' "$DEVICE_LIST" |
  sed -n 's/.*\[\([0-9][0-9]*\)\] TinyUSB UAC1$/\1/p' | head -n 1)
if [ -z "$AUDIO_INDEX" ]; then
  printf 'FAIL avfoundation_device_missing TinyUSB_UAC1\n' | tee "$RESULT_FILE"
  exit 22
fi

"$FFMPEG" -y -nostdin -hide_banner -loglevel info \
  -f avfoundation -i ":$AUDIO_INDEX" -t 3 \
  -c:a pcm_s16le "$WAV_FILE" >"$CAPTURE_LOG" 2>&1 &
CAPTURE_PID=$!
sleep 0.5
set +e
/usr/bin/python3 - "$SCRIPT_DIR/cdc-status.py" "$LINK_PROBE" <<'PY'
import glob
import importlib.util
import json
import sys
import time

spec = importlib.util.spec_from_file_location("pokepod_link", sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
ports = sorted(glob.glob("/dev/cu.usbmodem*"))
for port in ports:
    try:
        status = module.query(port, "status", 3)
        if status.get("status") != "ok":
            continue
        for _ in range(10):
            if status.get("mic_streaming"):
                break
            time.sleep(0.1)
            status = module.query(port, "status", 3)
        if not status.get("mic_streaming"):
            continue
        fingerprint = module.query(port, "fingerprint", 3)
        if fingerprint.get("status") != "ok" or not fingerprint.get("fingerprint"):
            continue
        with open(sys.argv[2], "w", encoding="utf-8") as output:
            json.dump({"port": port, "status": status,
                       "fingerprint": fingerprint}, output,
                      ensure_ascii=False, sort_keys=True)
        raise SystemExit(0)
    except (OSError, ValueError, TimeoutError, ConnectionError):
        continue
raise SystemExit(1)
PY
LINK_RESULT=$?
set -e
STARTED_AT=$(date +%s)
while kill -0 "$CAPTURE_PID" 2>/dev/null; do
  NOW=$(date +%s)
  if [ $((NOW - STARTED_AT)) -ge 10 ]; then
    kill -KILL "$CAPTURE_PID" 2>/dev/null || true
    wait "$CAPTURE_PID" 2>/dev/null || true
    printf 'FAIL audio_no_frames capture_timeout\n' | tee "$RESULT_FILE"
    exit 30
  fi
  sleep 0.2
done
if ! wait "$CAPTURE_PID"; then
  printf 'FAIL capture_process_error\n' | tee "$RESULT_FILE"
  exit 31
fi
if [ "$LINK_RESULT" -ne 0 ]; then
  printf 'FAIL cdc_blocked_during_uac evidence=%s\n' "$RUN_ROOT" | tee "$RESULT_FILE"
  exit 36
fi
if [ ! -s "$WAV_FILE" ]; then
  printf 'FAIL wav_missing_or_empty\n' | tee "$RESULT_FILE"
  exit 32
fi

SAMPLE_RATE=$("$FFPROBE" -v error -select_streams a:0 \
  -show_entries stream=sample_rate -of default=noprint_wrappers=1:nokey=1 \
  "$WAV_FILE")
CHANNELS=$("$FFPROBE" -v error -select_streams a:0 \
  -show_entries stream=channels -of default=noprint_wrappers=1:nokey=1 \
  "$WAV_FILE")
DURATION=$("$FFPROBE" -v error \
  -show_entries format=duration -of default=noprint_wrappers=1:nokey=1 \
  "$WAV_FILE")
if [ "$SAMPLE_RATE" != "48000" ] || [ "$CHANNELS" != "1" ]; then
  printf 'FAIL pcm_format sample_rate=%s channels=%s\n' \
    "$SAMPLE_RATE" "$CHANNELS" | tee "$RESULT_FILE"
  exit 33
fi
# AVFoundation timestamps live microphone buffers against wall time and does
# not synthesize samples for callback gaps. On this Mac the built-in microphone
# yields about 84% sample duration through the same path; require at least 75%
# so the gate still catches a stalled or severely lossy USB stream.
if ! awk -v duration="$DURATION" 'BEGIN { exit !(duration >= 2.25) }'; then
  printf 'FAIL capture_too_short duration=%s\n' "$DURATION" | tee "$RESULT_FILE"
  exit 34
fi

VOLUME_OUTPUT=$("$FFMPEG" -hide_banner -nostdin -i "$WAV_FILE" \
  -af volumedetect -f null - 2>&1)
MEAN_VOLUME=$(printf '%s\n' "$VOLUME_OUTPUT" |
  sed -n 's/.*mean_volume: \([^ ]*\) dB.*/\1/p' | tail -n 1)
MAX_VOLUME=$(printf '%s\n' "$VOLUME_OUTPUT" |
  sed -n 's/.*max_volume: \([^ ]*\) dB.*/\1/p' | tail -n 1)
if [ -z "$MAX_VOLUME" ] || [ "$MAX_VOLUME" = "-inf" ] ||
   ! awk -v peak="$MAX_VOLUME" 'BEGIN { exit !(peak > -90) }'; then
  printf 'FAIL silent_capture max_db=%s\n' "${MAX_VOLUME:-unknown}" | tee "$RESULT_FILE"
  exit 35
fi

printf 'PASS audio_frames sample_rate=%s channels=%s duration=%s mean_db=%s max_db=%s cdc=concurrent evidence=%s\n' \
  "$SAMPLE_RATE" "$CHANNELS" "$DURATION" "${MEAN_VOLUME:-unknown}" \
  "$MAX_VOLUME" "$RUN_ROOT" |
  tee "$RESULT_FILE"
