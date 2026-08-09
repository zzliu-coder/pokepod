#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
WORK_DIR="$PROJECT_DIR/work/pokepod-build"
VENDOR_DIR="$PROJECT_DIR/work/pokepod-vendor/waveshare"
SKETCH_DIR="$SCRIPT_DIR/PokePodAmoled"
ARDUINO_CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
GFX_LIBRARY="/Users/zheliu/Documents/Arduino/libraries/GFX_Library_for_Arduino"
WAVESHARE_COMMIT="ba32b5cbca96f0e04b0736d04959b6e832268d3f"
ESP32_CORE_VERSION=$(
  "$ARDUINO_CLI" core list 2>/dev/null |
    awk '$1 == "esp32:esp32" { print $2; exit }'
)
ESP32_S3_SDK_DIR="/Users/zheliu/Library/Arduino15/packages/esp32/tools/esp32s3-libs/$ESP32_CORE_VERSION"
SDK_OVERLAY_DIR="$WORK_DIR/sdk-single-connection"
SDK_VARIANT=qio_opi

if [ ! -x "$ARDUINO_CLI" ]; then
  echo "Arduino CLI not found: $ARDUINO_CLI" >&2
  exit 1
fi
if [ ! -f "$GFX_LIBRARY/library.properties" ]; then
  echo "Arduino GFX compatibility library not found: $GFX_LIBRARY" >&2
  exit 1
fi
if ! grep -q '^version=1\.6\.5$' "$GFX_LIBRARY/library.properties"; then
  echo "Expected Arduino GFX 1.6.5 at: $GFX_LIBRARY" >&2
  exit 1
fi
if [ -z "$ESP32_CORE_VERSION" ]; then
  echo "ESP32 Arduino core is not installed" >&2
  exit 1
fi
if [ ! -f "$ESP32_S3_SDK_DIR/sdkconfig" ] ||
   [ ! -f "$ESP32_S3_SDK_DIR/$SDK_VARIANT/include/sdkconfig.h" ]; then
  echo "ESP32-S3 SDK configuration not found: $ESP32_S3_SDK_DIR" >&2
  exit 1
fi

case "$ESP32_CORE_VERSION" in
  3.3.8|3.3.9|3.3.10|3.3.11) ;;
  *)
    echo "Unsupported ESP32 Arduino core: $ESP32_CORE_VERSION" >&2
    exit 1
    ;;
esac

mkdir -p "$(dirname -- "$VENDOR_DIR")" "$WORK_DIR/build" "$WORK_DIR/output" \
  "$SDK_OVERLAY_DIR/$SDK_VARIANT/include"

# Arduino-ESP32 ships a generic SDK configured for three NimBLE controller
# connections. PokePod Voice has passkey callbacks that do not carry a
# connection handle, so the stack-facing Arduino BLE layer must be compiled
# for exactly one connection. Keep the installed toolchain immutable and build
# against a local symlink overlay containing only the two adjusted configs.
for sdk_item in "$ESP32_S3_SDK_DIR"/*; do
  sdk_name=$(basename -- "$sdk_item")
  case "$sdk_name" in
    sdkconfig|"$SDK_VARIANT") continue ;;
  esac
  ln -sfn "$sdk_item" "$SDK_OVERLAY_DIR/$sdk_name"
done
for sdk_item in "$ESP32_S3_SDK_DIR/$SDK_VARIANT"/*; do
  sdk_name=$(basename -- "$sdk_item")
  [ "$sdk_name" = include ] && continue
  ln -sfn "$sdk_item" "$SDK_OVERLAY_DIR/$SDK_VARIANT/$sdk_name"
done
for sdk_item in "$ESP32_S3_SDK_DIR/$SDK_VARIANT/include"/*; do
  sdk_name=$(basename -- "$sdk_item")
  [ "$sdk_name" = sdkconfig.h ] && continue
  ln -sfn "$sdk_item" "$SDK_OVERLAY_DIR/$SDK_VARIANT/include/$sdk_name"
done
awk '
  /^CONFIG_BT_NIMBLE_MAX_CONNECTIONS=/ {
    print "CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1"; next
  }
  /^CONFIG_NIMBLE_MAX_CONNECTIONS=/ {
    print "CONFIG_NIMBLE_MAX_CONNECTIONS=1"; next
  }
  { print }
' "$ESP32_S3_SDK_DIR/sdkconfig" > "$SDK_OVERLAY_DIR/sdkconfig.next"
if cmp -s "$SDK_OVERLAY_DIR/sdkconfig.next" "$SDK_OVERLAY_DIR/sdkconfig"; then
  rm "$SDK_OVERLAY_DIR/sdkconfig.next"
else
  mv "$SDK_OVERLAY_DIR/sdkconfig.next" "$SDK_OVERLAY_DIR/sdkconfig"
fi
awk '
  /^#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS / {
    print "#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 1"; next
  }
  /^#define CONFIG_NIMBLE_MAX_CONNECTIONS / {
    print "#define CONFIG_NIMBLE_MAX_CONNECTIONS 1"; next
  }
  { print }
' "$ESP32_S3_SDK_DIR/$SDK_VARIANT/include/sdkconfig.h" \
  > "$SDK_OVERLAY_DIR/$SDK_VARIANT/include/sdkconfig.h.next"
if cmp -s "$SDK_OVERLAY_DIR/$SDK_VARIANT/include/sdkconfig.h.next" \
    "$SDK_OVERLAY_DIR/$SDK_VARIANT/include/sdkconfig.h"; then
  rm "$SDK_OVERLAY_DIR/$SDK_VARIANT/include/sdkconfig.h.next"
else
  mv "$SDK_OVERLAY_DIR/$SDK_VARIANT/include/sdkconfig.h.next" \
    "$SDK_OVERLAY_DIR/$SDK_VARIANT/include/sdkconfig.h"
fi
rg -qx 'CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1' "$SDK_OVERLAY_DIR/sdkconfig"
rg -qx '#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 1' \
  "$SDK_OVERLAY_DIR/$SDK_VARIANT/include/sdkconfig.h"
if [ ! -d "$VENDOR_DIR/.git" ]; then
  git clone https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8.git "$VENDOR_DIR"
fi
if ! git -C "$VENDOR_DIR" cat-file -e "$WAVESHARE_COMMIT^{commit}" 2>/dev/null; then
  git -C "$VENDOR_DIR" fetch origin "$WAVESHARE_COMMIT"
fi
git -C "$VENDOR_DIR" checkout --detach "$WAVESHARE_COMMIT"

BUILD_LOG="$WORK_DIR/build.log"
CLEAN_FLAG=--clean
if [ "${POKEPOD_INCREMENTAL:-0}" = 1 ]; then
  CLEAN_FLAG=
fi
if ! "$ARDUINO_CLI" compile $CLEAN_FLAG \
  --warnings all \
  --fqbn 'esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,DFUOnBoot=default,UploadMode=cdc,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,DebugLevel=info,EraseFlash=none' \
  --build-property "compiler.sdk.path=$SDK_OVERLAY_DIR" \
  --library "$GFX_LIBRARY" \
  --library "$VENDOR_DIR/examples/arduino-v2/libraries/Arduino_DriveBus" \
  --library "$VENDOR_DIR/examples/arduino-v2/libraries/Adafruit_XCA9554" \
  --library "$VENDOR_DIR/examples/arduino-v2/libraries/Adafruit_BusIO" \
  --library "$VENDOR_DIR/examples/arduino-v2/libraries/SensorLib" \
  --library "$VENDOR_DIR/examples/arduino/libraries/XPowersLib" \
  --library "$VENDOR_DIR/examples/arduino-v2/examples/15_ES8311" \
  "$@" \
  --build-path "$WORK_DIR/build" \
  --output-dir "$WORK_DIR/output" \
  "$SKETCH_DIR" >"$BUILD_LOG" 2>&1
then
  cat "$BUILD_LOG"
  exit 1
fi
cat "$BUILD_LOG"
rg -qx 'CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1' "$WORK_DIR/build/sdkconfig"
if rg -q '^CONFIG_BT_NIMBLE_MAX_CONNECTIONS=[2-9]' \
  "$WORK_DIR/build/sdkconfig"; then
  printf 'Build used a multi-connection NimBLE configuration\n' >&2
  exit 3
fi
if rg -q "${SKETCH_DIR}/.*warning:" "$BUILD_LOG"; then
  printf 'Project source emitted compiler warnings\n' >&2
  exit 2
fi
shasum -a 256 "$WORK_DIR"/output/*
