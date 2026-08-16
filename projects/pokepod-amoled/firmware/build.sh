#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)
WORK_DIR=${POKEPOD_BUILD_ROOT:-"$PROJECT_DIR/work/pokepod-build"}
VENDOR_DIR=${POKEPOD_VENDOR_DIR:-"$PROJECT_DIR/work/pokepod-vendor/waveshare"}
SKETCH_DIR="$SCRIPT_DIR/PokePodAmoled"
GFX_MANIFEST="$SCRIPT_DIR/gfx-minimal-files.txt"
WAVESHARE_COMMIT="ba32b5cbca96f0e04b0736d04959b6e832268d3f"
FQBN='esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default,DFUOnBoot=default,UploadMode=cdc,CPUFreq=240,FlashMode=qio,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB,PSRAM=opi,DebugLevel=info,EraseFlash=none'
APP_ONLY_FLASH_OFFSET=0x10000
APP_SLOT_BYTES=0x300000
FINGERPRINT_TOOL="$PROJECT_DIR/tools/build-input-fingerprint.py"
ARTIFACT_TOOL="$PROJECT_DIR/tools/write-artifact-manifest.py"
FLASH_SIZE_POLICY_TOOL="$PROJECT_DIR/tools/flash-size-policy.py"
FIRMWARE_VERSION_HEADER="$SKETCH_DIR/FirmwareVersion.h"
BUILD_ENV_TOOL="$PROJECT_DIR/tools/pokepod_build_env.py"
PORTABLE_TOOL="$PROJECT_DIR/tools/portable_build_utils.py"
BUILD_MODE=${POKEPOD_BUILD_MODE:-fast}
FORCE_BUILD=0
SOURCE_REVISION=$(git -C "$PROJECT_DIR" rev-parse --verify HEAD 2>/dev/null || true)
SOURCE_TREE=$(git -C "$PROJECT_DIR" rev-parse --verify "${SOURCE_REVISION}^{tree}" 2>/dev/null || true)
SOURCE_DIRTY=1
if [ -n "$SOURCE_REVISION" ] &&
   [ -z "$(git -C "$PROJECT_DIR" status --porcelain --untracked-files=all -- . 2>/dev/null)" ]; then
  SOURCE_DIRTY=0
fi
SOURCE_DATE_EPOCH_VALUE=interactive
BUILD_EPOCH_CPP_FLAG=
# Do not let an inherited value change a binary without appearing in the build
# fingerprint. Release builds replace this with the exact Git commit time.
unset SOURCE_DATE_EPOCH

while [ "$#" -gt 0 ]; do
  case "$1" in
    --fast)
      BUILD_MODE=fast
      shift
      ;;
    --release|--clean)
      BUILD_MODE=release
      shift
      ;;
    --force)
      FORCE_BUILD=1
      shift
      ;;
    --help)
      cat <<'EOF'
Usage: ./firmware/build.sh [--fast|--release|--force] [arduino-cli options]

  --fast      Reuse the persistent daily cache (default).
  --release   Use a separate build directory and force a clean build.
  --force     Run the fast compiler even when the input fingerprint matches.

Environment discovery:
  ARDUINO_CLI, ARDUINO_DATA_DIR, ARDUINO_USER_DIR, GFX_LIBRARY
  POKEPOD_BUILD_ROOT, POKEPOD_VENDOR_DIR

Production uses Arduino-ESP32 3.3.8. A non-production core requires both
POKEPOD_CORE_MATRIX=1 and an explicit POKEPOD_ESP32_CORE_VERSION.
EOF
      exit 0
      ;;
    --)
      shift
      break
      ;;
    *)
      break
      ;;
  esac
done
case "$BUILD_MODE" in
  fast|release) ;;
  *)
    echo "Unsupported PokePod build mode: $BUILD_MODE" >&2
    exit 64
    ;;
esac
# Reject caller-supplied compiler properties before validating Git metadata.
# This keeps malformed release invocations deterministic even for a source
# archive that intentionally has no .git directory.
for build_argument in "$@"; do
  case "$build_argument" in
    --build-property|--build-property=*)
      echo "Release build rejects caller-supplied --build-property" >&2
      exit 64
      ;;
  esac
done
if [ -z "$SOURCE_REVISION" ] || [ "${#SOURCE_REVISION}" -ne 40 ] ||
   ! printf '%s' "$SOURCE_REVISION" | grep -Eq '^[0-9a-f]{40}$'; then
  echo "PokePod build requires a real Git commit SHA" >&2
  exit 65
fi
if [ -z "$SOURCE_TREE" ] || [ "${#SOURCE_TREE}" -ne 40 ] ||
   ! printf '%s' "$SOURCE_TREE" | grep -Eq '^[0-9a-f]{40}$'; then
  echo "PokePod build requires a real Git tree SHA" >&2
  exit 65
fi
# Historical release-gate shape retained for source compatibility:
: '
if [ "$BUILD_MODE" = release ]; then
  for build_argument in "$@"
'
if [ "$BUILD_MODE" = release ] &&
   [ -n "$(git -C "$PROJECT_DIR" status --porcelain --untracked-files=all -- . 2>/dev/null)" ]; then
  echo "Release build requires a clean PokePod tree" >&2
  git -C "$PROJECT_DIR" status --short --untracked-files=all -- . >&2
  exit 65
fi
if [ "$BUILD_MODE" = release ] && [ -z "$SOURCE_REVISION" ]; then
  echo "Release build requires a Git commit" >&2
  exit 65
fi
if [ "$BUILD_MODE" = release ]; then
  SOURCE_DATE_EPOCH_VALUE=$(
    git -C "$PROJECT_DIR" show -s --format=%ct "$SOURCE_REVISION" 2>/dev/null || true
  )
  case "$SOURCE_DATE_EPOCH_VALUE" in
    ''|*[!0-9]*)
      echo "Release build requires a positive Git commit timestamp" >&2
      exit 65
      ;;
  esac
  if [ "$SOURCE_DATE_EPOCH_VALUE" -le 0 ]; then
    echo "Release build requires a positive Git commit timestamp" >&2
    exit 65
  fi
  SOURCE_DATE_EPOCH=$SOURCE_DATE_EPOCH_VALUE
  export SOURCE_DATE_EPOCH
  BUILD_EPOCH_CPP_FLAG=-DPOKEPOD_BUILD_EPOCH_UTC=$SOURCE_DATE_EPOCH_VALUE
fi

# Identity flags are generated only from the Git object database above. A
# caller cannot replace them through environment variables or extra Arduino
# build properties.
# Historical source gate marker: --build-property "compiler.cpp.extra_flags=$BUILD_EPOCH_CPP_FLAG"
COMPILER_CPP_EXTRA_FLAGS="-DPOKEPOD_SOURCE_REVISION=\"$SOURCE_REVISION\" -DPOKEPOD_SOURCE_TREE=\"$SOURCE_TREE\" -DPOKEPOD_SOURCE_DIRTY=$SOURCE_DIRTY -DPOKEPOD_APP_ELF_SHA256=\"unknown\""
if [ -n "$BUILD_EPOCH_CPP_FLAG" ]; then
  COMPILER_CPP_EXTRA_FLAGS="$COMPILER_CPP_EXTRA_FLAGS $BUILD_EPOCH_CPP_FLAG"
fi

BUILD_DIR="$WORK_DIR/build-$BUILD_MODE"
BUILD_LOG="$WORK_DIR/build-$BUILD_MODE.log"
OUTPUT_DIR="$WORK_DIR/output/$BUILD_MODE"
RESOURCE_REVIEW_FILE="$WORK_DIR/resource-review.json"
CACHE_DIR="$WORK_DIR/cache"
SUCCESS_FINGERPRINT="$CACHE_DIR/$BUILD_MODE-success.sha256"
CURRENT_FINGERPRINT="$CACHE_DIR/$BUILD_MODE-current.sha256"
STARTED_AT=$(date +%s)
SDK_OVERLAY_DIR="$WORK_DIR/sdk-single-connection"
SDK_VARIANT=qio_opi

if [ ! -f "$FINGERPRINT_TOOL" ]; then
  echo "Build fingerprint tool not found: $FINGERPRINT_TOOL" >&2
  exit 1
fi
if [ ! -f "$ARTIFACT_TOOL" ]; then
  echo "Artifact manifest tool not found: $ARTIFACT_TOOL" >&2
  exit 1
fi
if [ ! -f "$FLASH_SIZE_POLICY_TOOL" ]; then
  echo "Flash size policy tool not found: $FLASH_SIZE_POLICY_TOOL" >&2
  exit 1
fi
if [ ! -f "$FIRMWARE_VERSION_HEADER" ]; then
  echo "Firmware version source not found: $FIRMWARE_VERSION_HEADER" >&2
  exit 1
fi
if [ ! -f "$BUILD_ENV_TOOL" ]; then
  echo "Build environment resolver not found: $BUILD_ENV_TOOL" >&2
  exit 1
fi
if [ ! -f "$PORTABLE_TOOL" ]; then
  echo "Portable build utility not found: $PORTABLE_TOOL" >&2
  exit 1
fi
BUILD_ENV_ASSIGNMENTS=$(python3 "$BUILD_ENV_TOOL" --format shell)
eval "$BUILD_ENV_ASSIGNMENTS"
if [ "$BUILD_MODE" = release ] && [ "$POKEPOD_CORE_PROFILE" != production ]; then
  echo "Release build requires the production ESP32 core profile" >&2
  exit 66
fi
if [ ! -x "$ARDUINO_CLI" ]; then
  echo "Arduino CLI not found: $ARDUINO_CLI" >&2
  exit 1
fi
if [ ! -f "$GFX_LIBRARY/library.properties" ]; then
  echo "Arduino GFX compatibility library not found: $GFX_LIBRARY" >&2
  exit 1
fi
if [ ! -f "$GFX_MANIFEST" ]; then
  echo "PokePod GFX source manifest not found: $GFX_MANIFEST" >&2
  exit 1
fi
if ! grep -q '^version=1\.6\.5$' "$GFX_LIBRARY/library.properties"; then
  echo "Expected Arduino GFX 1.6.5 at: $GFX_LIBRARY" >&2
  exit 1
fi
if [ ! -f "$ESP32_PLATFORM_DIR/platform.txt" ] ||
   [ ! -f "$ESP32_PLATFORM_DIR/boards.txt" ]; then
  echo "ESP32 Arduino platform not found: $ESP32_PLATFORM_DIR" >&2
  exit 1
fi
if [ ! -f "$ESP32_S3_SDK_DIR/sdkconfig" ] ||
   [ ! -f "$ESP32_S3_SDK_DIR/$SDK_VARIANT/include/sdkconfig.h" ]; then
  echo "ESP32-S3 SDK configuration not found: $ESP32_S3_SDK_DIR" >&2
  exit 1
fi

mkdir -p "$(dirname -- "$VENDOR_DIR")" "$OUTPUT_DIR" "$CACHE_DIR" \
  "$SDK_OVERLAY_DIR/$SDK_VARIANT/include"

# Arduino GFX 1.6.5 contains more than 200 source files for unrelated panels
# and data buses. Build a symlink-only Arduino library containing the exact
# upstream files PokePod uses. The manifest hash gives changed views a new path
# while stable views keep stable mtimes and remain cacheable.
GFX_VIEW_ID=$(
  python3 "$FINGERPRINT_TOOL" \
    --file "$GFX_MANIFEST" \
    --file "$GFX_LIBRARY/library.properties" \
    --literal "gfx-source=$GFX_LIBRARY" | cut -c1-16
)
GFX_MINIMAL_LIBRARY="$WORK_DIR/gfx-minimal/$GFX_VIEW_ID"
mkdir -p "$GFX_MINIMAL_LIBRARY/src"
for gfx_metadata in library.properties license.txt; do
  gfx_source="$GFX_LIBRARY/$gfx_metadata"
  gfx_target="$GFX_MINIMAL_LIBRARY/$gfx_metadata"
  if [ ! -L "$gfx_target" ] || [ "$(readlink "$gfx_target")" != "$gfx_source" ]; then
    ln -sfn "$gfx_source" "$gfx_target"
  fi
done
while IFS= read -r gfx_relative || [ -n "$gfx_relative" ]; do
  case "$gfx_relative" in
    ''|'#'*) continue ;;
  esac
  gfx_source="$GFX_LIBRARY/src/$gfx_relative"
  gfx_target="$GFX_MINIMAL_LIBRARY/src/$gfx_relative"
  if [ ! -f "$gfx_source" ]; then
    echo "PokePod GFX source is missing: $gfx_source" >&2
    exit 1
  fi
  mkdir -p "$(dirname -- "$gfx_target")"
  if [ ! -L "$gfx_target" ] || [ "$(readlink "$gfx_target")" != "$gfx_source" ]; then
    ln -sfn "$gfx_source" "$gfx_target"
  fi
done < "$GFX_MANIFEST"

# Reuse the historical daily cache once. Release builds always have their own
# directory, so a clean release can no longer erase the fast edit-build loop.
if [ "$BUILD_MODE" = fast ] && [ ! -e "$BUILD_DIR" ] && \
   [ -f "$WORK_DIR/build/build.options.json" ]; then
  mv "$WORK_DIR/build" "$BUILD_DIR"
fi
mkdir -p "$BUILD_DIR"

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
  sdk_link="$SDK_OVERLAY_DIR/$sdk_name"
  if [ ! -L "$sdk_link" ] || [ "$(readlink "$sdk_link")" != "$sdk_item" ]; then
    ln -sfn "$sdk_item" "$sdk_link"
  fi
done
for sdk_item in "$ESP32_S3_SDK_DIR/$SDK_VARIANT"/*; do
  sdk_name=$(basename -- "$sdk_item")
  [ "$sdk_name" = include ] && continue
  sdk_link="$SDK_OVERLAY_DIR/$SDK_VARIANT/$sdk_name"
  if [ ! -L "$sdk_link" ] || [ "$(readlink "$sdk_link")" != "$sdk_item" ]; then
    ln -sfn "$sdk_item" "$sdk_link"
  fi
done
for sdk_item in "$ESP32_S3_SDK_DIR/$SDK_VARIANT/include"/*; do
  sdk_name=$(basename -- "$sdk_item")
  [ "$sdk_name" = sdkconfig.h ] && continue
  sdk_link="$SDK_OVERLAY_DIR/$SDK_VARIANT/include/$sdk_name"
  if [ ! -L "$sdk_link" ] || [ "$(readlink "$sdk_link")" != "$sdk_item" ]; then
    ln -sfn "$sdk_item" "$sdk_link"
  fi
done
awk '
  /^CONFIG_BT_NIMBLE_MAX_CONNECTIONS=/ {
    print "CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1"; next
  }
  /^CONFIG_NIMBLE_MAX_CONNECTIONS=/ {
    print "CONFIG_NIMBLE_MAX_CONNECTIONS=1"; next
  }
  /^# CONFIG_BT_CTRL_BLE_LLCP_CONN_UPDATE is not set$/ {
    print "CONFIG_BT_CTRL_BLE_LLCP_CONN_UPDATE=y"; next
  }
  /^CONFIG_BT_CTRL_BLE_LLCP_CONN_UPDATE=/ {
    print "CONFIG_BT_CTRL_BLE_LLCP_CONN_UPDATE=y"; next
  }
  { print }
' "$ESP32_S3_SDK_DIR/sdkconfig" > "$SDK_OVERLAY_DIR/sdkconfig.next"
if cmp -s "$SDK_OVERLAY_DIR/sdkconfig.next" "$SDK_OVERLAY_DIR/sdkconfig"; then
  rm "$SDK_OVERLAY_DIR/sdkconfig.next"
else
  mv "$SDK_OVERLAY_DIR/sdkconfig.next" "$SDK_OVERLAY_DIR/sdkconfig"
fi
awk '
  BEGIN { llcp_conn_update = 0 }
  /^#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS / {
    print "#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 1"; next
  }
  /^#define CONFIG_NIMBLE_MAX_CONNECTIONS / {
    print "#define CONFIG_NIMBLE_MAX_CONNECTIONS 1"; next
  }
  /^#define CONFIG_BT_CTRL_BLE_LLCP_CONN_UPDATE / {
    print "#define CONFIG_BT_CTRL_BLE_LLCP_CONN_UPDATE 1";
    llcp_conn_update = 1;
    next
  }
  { print }
  END {
    if (!llcp_conn_update) {
      print "#define CONFIG_BT_CTRL_BLE_LLCP_CONN_UPDATE 1"
    }
  }
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
rg -qx 'CONFIG_BT_CTRL_BLE_LLCP_CONN_UPDATE=y' \
  "$SDK_OVERLAY_DIR/sdkconfig"
rg -qx '#define CONFIG_BT_CTRL_BLE_LLCP_CONN_UPDATE 1' \
  "$SDK_OVERLAY_DIR/$SDK_VARIANT/include/sdkconfig.h"
if [ ! -d "$VENDOR_DIR/.git" ]; then
  git clone https://github.com/waveshareteam/ESP32-S3-Touch-AMOLED-1.8.git "$VENDOR_DIR"
fi
if ! git -C "$VENDOR_DIR" cat-file -e "$WAVESHARE_COMMIT^{commit}" 2>/dev/null; then
  git -C "$VENDOR_DIR" fetch origin "$WAVESHARE_COMMIT"
fi
if [ "$(git -C "$VENDOR_DIR" rev-parse HEAD)" != "$WAVESHARE_COMMIT" ]; then
  git -C "$VENDOR_DIR" checkout --detach "$WAVESHARE_COMMIT"
fi
if [ -n "$(git -C "$VENDOR_DIR" status --porcelain --untracked-files=no)" ]; then
  echo "Pinned Waveshare vendor checkout is dirty: $VENDOR_DIR" >&2
  exit 1
fi

EXTRA_ARGUMENTS_HASH=$(python3 "$PORTABLE_TOOL" argv-sha256 -- "$@")
CLI_VERSION=$("$ARDUINO_CLI" version | tr '\n' ' ')
BUILD_FINGERPRINT=$(python3 "$FINGERPRINT_TOOL" \
  --tree "$SKETCH_DIR" \
  --tree "$GFX_MINIMAL_LIBRARY/src" \
  --file "$GFX_MANIFEST" \
  --file "$GFX_LIBRARY/library.properties" \
  --file "$SCRIPT_DIR/build.sh" \
  --file "$FLASH_SIZE_POLICY_TOOL" \
  --file "$ESP32_PLATFORM_DIR/platform.txt" \
  --file "$ESP32_PLATFORM_DIR/boards.txt" \
  --file "$SDK_OVERLAY_DIR/sdkconfig" \
  --file "$SDK_OVERLAY_DIR/$SDK_VARIANT/include/sdkconfig.h" \
  --literal "arduino-cli=$CLI_VERSION" \
  --literal "esp32-core=$ESP32_CORE_VERSION" \
  --literal "waveshare=$WAVESHARE_COMMIT" \
  --literal "gfx-source=$GFX_LIBRARY" \
  --literal "gfx-view=$GFX_VIEW_ID" \
  --literal "vendor-path=$VENDOR_DIR" \
  --literal "fqbn=$FQBN" \
  --literal "source-date-epoch=$SOURCE_DATE_EPOCH_VALUE" \
  --literal "source-revision=$SOURCE_REVISION" \
  --literal "source-tree=$SOURCE_TREE" \
  --literal "source-dirty=$SOURCE_DIRTY" \
  --literal "extra-arguments=$EXTRA_ARGUMENTS_HASH")
printf '%s\n' "$BUILD_FINGERPRINT" > "$CURRENT_FINGERPRINT"

bind_artifact_identity() {
  artifact_path="$OUTPUT_DIR/artifact.json"
  elf_path="$OUTPUT_DIR/PokePodAmoled.ino.elf"
  elf_sha256=unknown
  if [ -f "$elf_path" ]; then
    elf_sha256=$(python3 "$PORTABLE_TOOL" sha256-value "$elf_path")
  fi
  python3 - "$artifact_path" "$SOURCE_TREE" "$elf_sha256" <<'PY'
import json
import os
import sys
from pathlib import Path

artifact_path = Path(sys.argv[1])
source_tree = sys.argv[2]
elf_sha256 = sys.argv[3]
payload = json.loads(artifact_path.read_text(encoding="utf-8"))
payload["sourceTree"] = source_tree
payload["imageIdentity"] = {
    "magic": "PKPDIMG2",
    "schema": 2,
    "product": "PokePodAmoled",
    "firmwareVersion": payload.get("firmwareVersion"),
    "sourceRevision": payload.get("sourceRevision"),
    "sourceTree": source_tree,
    "sourceDirty": payload.get("sourceDirty"),
    "appElfSha256": elf_sha256,
}
temporary = artifact_path.with_suffix(artifact_path.suffix + ".next")
temporary.write_text(
    json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8"
)
os.replace(temporary, artifact_path)
PY
}

write_artifact_manifest() {
  firmware_bin="$OUTPUT_DIR/PokePodAmoled.ino.bin"
  source_revision=${SOURCE_REVISION:-unknown}
  if [ -n "$(git -C "$PROJECT_DIR" status --porcelain --untracked-files=all -- . 2>/dev/null)" ]; then
    source_dirty=true
  else
    source_dirty=false
  fi
  firmware_sha=$(python3 "$PORTABLE_TOOL" sha256-value "$firmware_bin")
  firmware_size=$(python3 "$PORTABLE_TOOL" size "$firmware_bin")
  python3 "$FLASH_SIZE_POLICY_TOOL" \
    --program-bytes "$firmware_size" \
    --slot-bytes "$APP_SLOT_BYTES" \
    --output "$OUTPUT_DIR/flash-resource.json" \
    --enforce
  resource_review_approved=false
  if [ "$BUILD_MODE" = release ]; then
    python3 "$FLASH_SIZE_POLICY_TOOL" \
      --program-bytes "$firmware_size" \
      --slot-bytes "$APP_SLOT_BYTES" \
      --require-release-review \
      --review "$RESOURCE_REVIEW_FILE" \
      --source-revision "$source_revision" \
      --binary "$firmware_bin"
    resource_review_approved=true
  fi
  created_at=$(date -u +%Y-%m-%dT%H:%M:%SZ)
  python3 "$ARTIFACT_TOOL" \
    --output "$OUTPUT_DIR/artifact.json" \
    --lane "$BUILD_MODE" \
    --source-revision "$source_revision" \
    --source-dirty "$source_dirty" \
    --build-input "$BUILD_FINGERPRINT" \
    --binary-sha256 "$firmware_sha" \
    --binary-size "$firmware_size" \
    --flash-policy "$OUTPUT_DIR/flash-resource.json" \
    --resource-review-approved "$resource_review_approved" \
    --created-at "$created_at" \
    --fqbn "$FQBN" \
    --core-version "$ESP32_CORE_VERSION" \
    --core-profile "$POKEPOD_CORE_PROFILE" \
    --app-offset "$APP_ONLY_FLASH_OFFSET" \
    --vendor-revision "$WAVESHARE_COMMIT" \
    --firmware-version-header "$FIRMWARE_VERSION_HEADER"
  bind_artifact_identity
}

if [ "$BUILD_MODE" = fast ] && [ "$FORCE_BUILD" -eq 0 ] && [ "$#" -eq 0 ] && \
   [ -f "$SUCCESS_FINGERPRINT" ] && \
   [ "$(cat "$SUCCESS_FINGERPRINT")" = "$BUILD_FINGERPRINT" ] && \
   [ -f "$BUILD_DIR/build.options.json" ] && \
   [ -f "$OUTPUT_DIR/PokePodAmoled.ino.bin" ] && \
   [ -f "$OUTPUT_DIR/artifact.json" ]; then
  printf 'CACHE HIT pokepod fast build (%s)\n' "$BUILD_FINGERPRINT"
  rg -qx 'CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1' "$BUILD_DIR/sdkconfig"
  rg -qx 'CONFIG_BT_CTRL_BLE_LLCP_CONN_UPDATE=y' "$BUILD_DIR/sdkconfig"
  write_artifact_manifest
  python3 "$PORTABLE_TOOL" sha256 "$OUTPUT_DIR"/*
  printf 'Build mode: fast; elapsed: %ss\n' "$(($(date +%s) - STARTED_AT))"
  exit 0
fi

CLEAN_FLAG=
if [ "$BUILD_MODE" = release ]; then
  CLEAN_FLAG=--clean
fi
printf 'Build mode: %s; fingerprint: %s\n' "$BUILD_MODE" "$BUILD_FINGERPRINT"
if ! "$ARDUINO_CLI" compile $CLEAN_FLAG \
  --jobs 0 \
  --warnings all \
  --fqbn "$FQBN" \
  --build-property "compiler.cpp.extra_flags=$COMPILER_CPP_EXTRA_FLAGS" \
  --build-property "compiler.sdk.path=$SDK_OVERLAY_DIR" \
  --library "$GFX_MINIMAL_LIBRARY" \
  --library "$VENDOR_DIR/examples/arduino-v2/libraries/Arduino_DriveBus" \
  --library "$VENDOR_DIR/examples/arduino-v2/libraries/Adafruit_XCA9554" \
  --library "$VENDOR_DIR/examples/arduino-v2/libraries/Adafruit_BusIO" \
  --library "$VENDOR_DIR/examples/arduino-v2/libraries/SensorLib" \
  --library "$VENDOR_DIR/examples/arduino/libraries/XPowersLib" \
  --library "$VENDOR_DIR/examples/arduino-v2/examples/15_ES8311" \
  "$@" \
  --build-path "$BUILD_DIR" \
  --output-dir "$OUTPUT_DIR" \
  "$SKETCH_DIR" >"$BUILD_LOG" 2>&1
then
  cat "$BUILD_LOG"
  exit 1
fi
cat "$BUILD_LOG"
rg -qx 'CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1' "$BUILD_DIR/sdkconfig"
rg -qx 'CONFIG_BT_CTRL_BLE_LLCP_CONN_UPDATE=y' "$BUILD_DIR/sdkconfig"
if rg -q '^CONFIG_BT_NIMBLE_MAX_CONNECTIONS=[2-9]' \
  "$BUILD_DIR/sdkconfig"; then
  printf 'Build used a multi-connection NimBLE configuration\n' >&2
  exit 3
fi
if rg -q "${SKETCH_DIR}/.*warning:" "$BUILD_LOG"; then
  printf 'Project source emitted compiler warnings\n' >&2
  exit 2
fi
printf '%s\n' "$BUILD_FINGERPRINT" > "$SUCCESS_FINGERPRINT"
printf '%s\n' "$BUILD_FINGERPRINT" > "$OUTPUT_DIR/build-input.sha256"
printf '%s\n' "$BUILD_MODE" > "$OUTPUT_DIR/build-mode.txt"
if [ "$BUILD_MODE" = release ]; then
  cp "$BUILD_LOG" "$WORK_DIR/build.log"
fi
write_artifact_manifest
python3 "$PORTABLE_TOOL" sha256 "$OUTPUT_DIR"/*
printf 'Build mode: %s; elapsed: %ss\n' "$BUILD_MODE" \
  "$(($(date +%s) - STARTED_AT))"
