#!/bin/zsh
set -euo pipefail

SCRIPT_DIR=${0:A:h}
cd "$SCRIPT_DIR"

swift build -c release --disable-sandbox --scratch-path "$SCRIPT_DIR/.build"

APP="$SCRIPT_DIR/dist/PokeCapsule.app"
CONTENTS="$APP/Contents"
rm -rf "$APP"
mkdir -p "$CONTENTS/MacOS" "$CONTENTS/Resources"
cp "$SCRIPT_DIR/.build/release/PokeCapsule" "$CONTENTS/MacOS/PokeCapsule"
cp "$SCRIPT_DIR/Resources/Info.plist" "$CONTENTS/Info.plist"
cp "$SCRIPT_DIR/Resources/PokeCapsule.icns" "$CONTENTS/Resources/PokeCapsule.icns"
codesign --force --sign - "$APP"
echo "$APP"
