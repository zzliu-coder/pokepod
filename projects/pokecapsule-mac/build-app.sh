#!/bin/zsh
set -euo pipefail

SCRIPT_DIR=${0:A:h}
cd "$SCRIPT_DIR"

swift build -c release --disable-sandbox --scratch-path "$SCRIPT_DIR/.build"

DIST="$SCRIPT_DIR/dist"
CAPSULE_APP="$DIST/PokeCapsule.app"
VOICE_APP="$DIST/PokePod Voice.app"
mkdir -p "$DIST"
STAGING_ROOT=$(mktemp -d "$DIST/.build-app.XXXXXX")
trap 'rm -rf "$STAGING_ROOT"' EXIT

assemble_app() {
  local app_name=$1
  local executable=$2
  local plist=$3
  local icon_name=$4
  local staged_app="$STAGING_ROOT/$app_name.app"
  local contents="$staged_app/Contents"
  mkdir -p "$contents/MacOS" "$contents/Resources"
  cp "$SCRIPT_DIR/.build/release/$executable" "$contents/MacOS/$executable"
  cp "$SCRIPT_DIR/Resources/$plist" "$contents/Info.plist"
  cp "$SCRIPT_DIR/Resources/PokeCapsule.icns" "$contents/Resources/$icon_name"
  plutil -lint "$contents/Info.plist"
  codesign --force --sign - "$staged_app"
  codesign --verify --deep --strict "$staged_app"
}

assemble_app "PokeCapsule" "PokeCapsule" "Info.plist" "PokeCapsule.icns"
# Keep the two local apps buildable from a clean checkout.  Voice does not
# need a second binary icon asset; using the tracked capsule icon avoids a
# hidden/untracked resource being required for every release build.
assemble_app "PokePod Voice" "PokePodVoice" "PokePodVoice-Info.plist" "PokeCapsule.icns"

mkdir -p "$DIST/backups"
BACKUP_ROOT=$(mktemp -d "$DIST/backups/build-app.XXXXXX")
[[ ! -d "$CAPSULE_APP" ]] || mv "$CAPSULE_APP" "$BACKUP_ROOT/PokeCapsule.app"
[[ ! -d "$VOICE_APP" ]] || mv "$VOICE_APP" "$BACKUP_ROOT/PokePod Voice.app"

rollback() {
  [[ ! -d "$CAPSULE_APP" ]] || mv "$CAPSULE_APP" "$STAGING_ROOT/failed-PokeCapsule.app"
  [[ ! -d "$VOICE_APP" ]] || mv "$VOICE_APP" "$STAGING_ROOT/failed-PokePod Voice.app"
  [[ ! -d "$BACKUP_ROOT/PokeCapsule.app" ]] || mv "$BACKUP_ROOT/PokeCapsule.app" "$CAPSULE_APP"
  [[ ! -d "$BACKUP_ROOT/PokePod Voice.app" ]] || mv "$BACKUP_ROOT/PokePod Voice.app" "$VOICE_APP"
}
trap 'rollback; rm -rf "$STAGING_ROOT"' ERR
mv "$STAGING_ROOT/PokeCapsule.app" "$CAPSULE_APP"
mv "$STAGING_ROOT/PokePod Voice.app" "$VOICE_APP"
trap - ERR

echo "$CAPSULE_APP"
echo "$VOICE_APP"
