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

# Arduino-ESP32 3.3.8 shipped before TinyUSB PR #3640. Its ESP32-S3 DWC2
# driver can permanently deactivate an isochronous IN endpoint after an
# incomplete transfer, leaving USB microphones enumerated but sending only
# zero-length packets. Build a project-local archive with the single corrected
# DWC2 object from the first fixed release; never mutate the global Arduino
# installation. Newer cores already contain the upstream fix.
set --
TINYUSB_PATCHED_ARCHIVE=""
if [ "$ESP32_CORE_VERSION" = "3.3.8" ]; then
  TOOLCHAIN_DIR="$WORK_DIR/toolchain"
  UPSTREAM_ZIP="$TOOLCHAIN_DIR/esp32s3-libs-3.3.9.zip"
  UPSTREAM_DIR="$TOOLCHAIN_DIR/esp32s3-3.3.9"
  PATCH_DIR="$TOOLCHAIN_DIR/tinyusb-3.3.8-pr3640"
  BASE_SDK="/Users/zheliu/Library/Arduino15/packages/esp32/tools/esp32s3-libs/3.3.8"
  BASE_ARCHIVE="$BASE_SDK/lib/libarduino_tinyusb.a"
  BASE_LD_LIBS="$BASE_SDK/flags/ld_libs"
  TINYUSB_PATCHED_ARCHIVE="$PATCH_DIR/libarduino_tinyusb.a"
  PATCHED_LD_LIBS="$PATCH_DIR/ld_libs"
  UPSTREAM_URL="https://github.com/espressif/arduino-esp32/releases/download/3.3.9/esp32s3-libs-3.3.9.zip"
  UPSTREAM_ZIP_SHA="34684fecef49e92e9fb11784ab5f0892328d3678ad8e8445a2bbf2cddf6a4fb6"
  BASE_ARCHIVE_SHA="afe9a1545350848486edd59b5a798c80c6fd462ffa1d2d71ec1f80a24f53848f"
  PATCH_OBJECT_SHA="04ad2e97999b3a2ba4057c8d89be0f58f92e158460541369db2f4d3e206321f2"
  XTENSA_AR="/Users/zheliu/Library/Arduino15/packages/esp32/tools/esp-x32/2601/bin/xtensa-esp32s3-elf-ar"

  mkdir -p "$TOOLCHAIN_DIR" "$UPSTREAM_DIR" "$PATCH_DIR" "$PATCH_DIR/verify"
  if [ ! -f "$BASE_ARCHIVE" ] || [ ! -f "$BASE_LD_LIBS" ]; then
    echo "Arduino-ESP32 3.3.8 TinyUSB inputs are missing" >&2
    exit 1
  fi
  if [ "$(shasum -a 256 "$BASE_ARCHIVE" | awk '{print $1}')" != "$BASE_ARCHIVE_SHA" ]; then
    echo "Arduino-ESP32 3.3.8 TinyUSB archive hash changed" >&2
    exit 1
  fi
  if [ ! -f "$UPSTREAM_ZIP" ] ||
     [ "$(shasum -a 256 "$UPSTREAM_ZIP" 2>/dev/null | awk '{print $1}')" != "$UPSTREAM_ZIP_SHA" ]; then
    command -v curl >/dev/null 2>&1 || {
      echo "curl is required to obtain the pinned TinyUSB fix" >&2
      exit 1
    }
    curl -L --fail --retry 10 --retry-all-errors --continue-at - \
      --output "$UPSTREAM_ZIP" "$UPSTREAM_URL"
  fi
  if [ "$(shasum -a 256 "$UPSTREAM_ZIP" | awk '{print $1}')" != "$UPSTREAM_ZIP_SHA" ]; then
    echo "Pinned ESP32-S3 library package hash mismatch" >&2
    exit 1
  fi

  unzip -jo "$UPSTREAM_ZIP" esp32s3-libs/lib/libarduino_tinyusb.a \
    -d "$UPSTREAM_DIR" >/dev/null
  (
    cd "$UPSTREAM_DIR"
    "$XTENSA_AR" x libarduino_tinyusb.a dcd_dwc2.c.obj
  )
  if [ "$(shasum -a 256 "$UPSTREAM_DIR/dcd_dwc2.c.obj" | awk '{print $1}')" != "$PATCH_OBJECT_SHA" ]; then
    echo "Pinned TinyUSB DWC2 object hash mismatch" >&2
    exit 1
  fi

  cp "$BASE_ARCHIVE" "$TINYUSB_PATCHED_ARCHIVE"
  (
    cd "$UPSTREAM_DIR"
    "$XTENSA_AR" r "$TINYUSB_PATCHED_ARCHIVE" dcd_dwc2.c.obj
  )
  (
    cd "$PATCH_DIR/verify"
    "$XTENSA_AR" x "$TINYUSB_PATCHED_ARCHIVE" dcd_dwc2.c.obj
  )
  if [ "$(shasum -a 256 "$PATCH_DIR/verify/dcd_dwc2.c.obj" | awk '{print $1}')" != "$PATCH_OBJECT_SHA" ]; then
    echo "Project-local TinyUSB patch verification failed" >&2
    exit 1
  fi
  sed "s#-larduino_tinyusb#$TINYUSB_PATCHED_ARCHIVE#" \
    "$BASE_LD_LIBS" >"$PATCHED_LD_LIBS"
  set -- --build-property "compiler.c.elf.libs=@$PATCHED_LD_LIBS"
elif [ "$ESP32_CORE_VERSION" = "3.3.9" ] ||
     [ "$ESP32_CORE_VERSION" = "3.3.10" ] ||
     [ "$ESP32_CORE_VERSION" = "3.3.11" ]; then
  :
else
  echo "Unsupported ESP32 Arduino core: $ESP32_CORE_VERSION" >&2
  exit 1
fi

mkdir -p "$(dirname -- "$VENDOR_DIR")" "$WORK_DIR/build" "$WORK_DIR/output"
if [ ! -d "$VENDOR_DIR/.git" ]; then
  git clone https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8.git "$VENDOR_DIR"
fi
if ! git -C "$VENDOR_DIR" cat-file -e "$WAVESHARE_COMMIT^{commit}" 2>/dev/null; then
  git -C "$VENDOR_DIR" fetch origin "$WAVESHARE_COMMIT"
fi
git -C "$VENDOR_DIR" checkout --detach "$WAVESHARE_COMMIT"

BUILD_LOG="$WORK_DIR/build.log"
if ! "$ARDUINO_CLI" compile --clean \
  --warnings all \
  --fqbn 'esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,DFUOnBoot=default,UploadMode=cdc,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,DebugLevel=info,EraseFlash=none' \
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
if rg -q "${SKETCH_DIR}/.*warning:" "$BUILD_LOG"; then
  printf 'Project source emitted compiler warnings\n' >&2
  exit 2
fi
if [ -n "$TINYUSB_PATCHED_ARCHIVE" ] &&
   ! rg -Fq "$TINYUSB_PATCHED_ARCHIVE(dcd_dwc2.c.obj)" \
     "$WORK_DIR/output/PokePodAmoled.ino.map"; then
  printf 'Project-local TinyUSB DWC2 fix was not linked\n' >&2
  exit 3
fi

shasum -a 256 "$WORK_DIR"/output/*
