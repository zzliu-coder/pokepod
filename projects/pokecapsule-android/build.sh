#!/bin/zsh
set -euo pipefail

SCRIPT_DIR=${0:A:h}
REQUIRED_GRADLE=$(<"$SCRIPT_DIR/gradle-version.txt")
GRADLE_BIN=${POKECAPSULE_GRADLE_BIN:-}

if [[ -z "$GRADLE_BIN" && -x "$SCRIPT_DIR/gradlew" ]]; then
  GRADLE_BIN="$SCRIPT_DIR/gradlew"
fi
if [[ -z "$GRADLE_BIN" ]]; then
  GRADLE_BIN=$(command -v gradle || true)
fi
if [[ -z "$GRADLE_BIN" || ! -x "$GRADLE_BIN" ]]; then
  print -u2 "找不到 Gradle。请安装 Gradle ${REQUIRED_GRADLE}，或设置 POKECAPSULE_GRADLE_BIN。"
  exit 2
fi

GRADLE_VERSION=$(
  "$GRADLE_BIN" --version 2>/dev/null \
    | sed -n 's/^Gradle \([0-9][0-9.]*\).*$/\1/p' \
    | head -n 1
)
if [[ "$GRADLE_VERSION" != "$REQUIRED_GRADLE" ]]; then
  print -u2 "Gradle 版本不匹配：需要 ${REQUIRED_GRADLE}，实际为 ${GRADLE_VERSION:-unknown}。"
  exit 2
fi

cd "$SCRIPT_DIR"
if [[ "$#" -eq 0 ]]; then
  set -- testPoke3LegacyDebugUnitTest testPhoneModernDebugUnitTest \
    lintPoke3LegacyDebug lintPhoneModernDebug \
    assemblePoke3LegacyDebug assemblePhoneModernDebug \
    lintPoke3LegacyRelease lintPhoneModernRelease \
    assemblePoke3LegacyRelease assemblePhoneModernRelease
fi

"$GRADLE_BIN" --no-daemon "$@"
