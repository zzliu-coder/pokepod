#!/bin/zsh
set -euo pipefail

SCRIPT_DIR=${0:A:h}
cd "$SCRIPT_DIR"

swift build -c release --disable-sandbox --scratch-path "$SCRIPT_DIR/.build"

APP="$SCRIPT_DIR/dist/PokeCapsule.app"
mkdir -p "$SCRIPT_DIR/dist"
STAGING_ROOT=$(mktemp -d "$SCRIPT_DIR/dist/.build-app.XXXXXX")
trap 'rm -rf "$STAGING_ROOT"' EXIT
STAGING_APP="$STAGING_ROOT/PokeCapsule.app"
CONTENTS="$STAGING_APP/Contents"
mkdir -p "$CONTENTS/MacOS" "$CONTENTS/Resources"
cp "$SCRIPT_DIR/.build/release/PokeCapsule" "$CONTENTS/MacOS/PokeCapsule"
cp "$SCRIPT_DIR/Resources/Info.plist" "$CONTENTS/Info.plist"
cp "$SCRIPT_DIR/Resources/PokeCapsule.icns" "$CONTENTS/Resources/PokeCapsule.icns"
plutil -lint "$CONTENTS/Info.plist"
codesign --force --sign - "$STAGING_APP"
codesign --verify --deep --strict "$STAGING_APP"

if [[ -d "$APP" ]]; then
  BACKUP_DIR="$SCRIPT_DIR/dist/backups"
  mkdir -p "$BACKUP_DIR"
  BACKUP_APP="$BACKUP_DIR/PokeCapsule-$(date +%Y%m%d-%H%M%S).app"
  mv "$APP" "$BACKUP_APP"
  echo "Previous app preserved at $BACKUP_APP"
fi
mv "$STAGING_APP" "$APP"
echo "$APP"
