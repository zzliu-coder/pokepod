#!/bin/zsh
set -euo pipefail

SCRIPT_DIR=${0:A:h}
GRADLE_BIN=${POKECAPSULE_GRADLE_BIN:-}
ANDROID_SDK=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}
if [[ -z "$GRADLE_BIN" && -x "$SCRIPT_DIR/gradlew" ]]; then
  GRADLE_BIN="$SCRIPT_DIR/gradlew"
fi
if [[ -z "$GRADLE_BIN" ]]; then
  GRADLE_BIN=$(command -v gradle || true)
fi
if [[ -z "$GRADLE_BIN" || ! -x "$GRADLE_BIN" ]]; then
  print -u2 "SKIP release-signing-contract: Gradle 8.14.5 is unavailable"
  exit 2
fi
if [[ -z "$ANDROID_SDK" || ! -d "$ANDROID_SDK/platforms/android-34" ]]; then
  print -u2 "SKIP release-signing-contract: Android SDK 34 is unavailable"
  exit 2
fi

KEYTOOL=${JAVA_HOME:-}/bin/keytool
[[ -x "$KEYTOOL" ]] || KEYTOOL=$(command -v keytool || true)
if [[ -z "$KEYTOOL" || ! -x "$KEYTOOL" ]]; then
  print -u2 "SKIP release-signing-contract: keytool is unavailable"
  exit 2
fi

TEMP_ROOT=$(mktemp -d "${TMPDIR:-/tmp}/pokecapsule-release-signing.XXXXXX")
trap 'rm -rf "$TEMP_ROOT"' EXIT
KEYSTORE="$TEMP_ROOT/release.jks"
STORE_PASSWORD='test-store-password'
KEY_ALIAS='test-release'
KEY_PASSWORD='test-key-password'

set +e
missing_output=$(
  ANDROID_HOME="$ANDROID_SDK" "$GRADLE_BIN" --offline --no-daemon \
    -p "$SCRIPT_DIR" :app:assemblePoke3LegacyRelease 2>&1
)
missing_status=$?
set -e
if (( missing_status == 0 )) || [[ "$missing_output" != *"Release tasks require all four external signer properties"* ]]; then
  print -u2 "FAIL release-signing-contract: 缺少 signer 未 fail-closed"
  print -u2 "$missing_output"
  exit 1
fi

"$KEYTOOL" -genkeypair -noprompt \
  -keystore "$KEYSTORE" -storetype JKS \
  -storepass "$STORE_PASSWORD" -keypass "$KEY_PASSWORD" \
  -alias "$KEY_ALIAS" -keyalg RSA -keysize 2048 -validity 1 \
  -dname 'CN=PokeCapsule release contract test' >/dev/null 2>&1

"$GRADLE_BIN" --offline --no-daemon \
  -p "$SCRIPT_DIR" \
  -PreleaseStoreFile="$KEYSTORE" \
  -PreleaseStorePassword="$STORE_PASSWORD" \
  -PreleaseKeyAlias="$KEY_ALIAS" \
  -PreleaseKeyPassword="$KEY_PASSWORD" \
  :app:assemblePoke3LegacyRelease

APKS=("$SCRIPT_DIR"/app/build/outputs/apk/**/*poke3Legacy*release*.apk(N))
if (( ${#APKS[@]} != 1 )); then
  print -u2 "FAIL release-signing-contract: 完整 signer 未生成唯一 release APK"
  exit 1
fi

APKSIGNER=$(find "$ANDROID_SDK/build-tools" -type f -name apksigner -perm -111 | sort | tail -n 1)
if [[ -z "$APKSIGNER" ]]; then
  print -u2 "SKIP release-signing-contract: apksigner is unavailable after build"
  exit 2
fi
CERT_OUTPUT=$("$APKSIGNER" verify --print-certs "${APKS[1]}" 2>&1)
if [[ "$CERT_OUTPUT" == *"Android Debug"* || "$CERT_OUTPUT" == *"CN=Android Debug"* ]]; then
  print -u2 "FAIL release-signing-contract: release APK 使用 debug certificate"
  print -u2 "$CERT_OUTPUT"
  exit 1
fi

print "PASS release-signing-contract"
