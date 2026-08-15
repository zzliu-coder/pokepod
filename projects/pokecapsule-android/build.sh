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

has_release_task=false
for task in "$@"; do
  if [[ "${task:l}" == *release* ]]; then
    has_release_task=true
    break
  fi
done
release_store_file=${POKECAPSULE_RELEASE_STORE_FILE:-}
release_store_password=${POKECAPSULE_RELEASE_STORE_PASSWORD:-}
release_key_alias=${POKECAPSULE_RELEASE_KEY_ALIAS:-}
release_key_password=${POKECAPSULE_RELEASE_KEY_PASSWORD:-}
explicit_release_store_file=false
explicit_release_store_password=false
explicit_release_key_alias=false
explicit_release_key_password=false
for argument in "$@"; do
  case "$argument" in
    -PreleaseStoreFile=*)
      release_store_file=${argument#*=}
      explicit_release_store_file=true
      ;;
    -PreleaseStorePassword=*)
      release_store_password=${argument#*=}
      explicit_release_store_password=true
      ;;
    -PreleaseKeyAlias=*)
      release_key_alias=${argument#*=}
      explicit_release_key_alias=true
      ;;
    -PreleaseKeyPassword=*)
      release_key_password=${argument#*=}
      explicit_release_key_password=true
      ;;
  esac
done
if [[ "$has_release_task" == true ]]; then
  missing=()
  [[ -n "$release_store_file" ]] || missing+=(releaseStoreFile)
  [[ -n "$release_store_password" ]] || missing+=(releaseStorePassword)
  [[ -n "$release_key_alias" ]] || missing+=(releaseKeyAlias)
  [[ -n "$release_key_password" ]] || missing+=(releaseKeyPassword)
  if (( ${#missing[@]} > 0 )); then
    print -u2 "Release 构建需要外部签名参数：${(j:, :)missing}。"
    print -u2 "请通过 Gradle -P 参数或安全 CI secret 注入；不会使用 debug key。"
    exit 2
  fi

  # Keep passwords out of logs. Explicit -P values remain authoritative;
  # environment values are appended only when their matching -P is absent.
  gradle_signing_properties=()
  [[ "$explicit_release_store_file" == true ]] || \
    gradle_signing_properties+=("-PreleaseStoreFile=$release_store_file")
  [[ "$explicit_release_store_password" == true ]] || \
    gradle_signing_properties+=("-PreleaseStorePassword=$release_store_password")
  [[ "$explicit_release_key_alias" == true ]] || \
    gradle_signing_properties+=("-PreleaseKeyAlias=$release_key_alias")
  [[ "$explicit_release_key_password" == true ]] || \
    gradle_signing_properties+=("-PreleaseKeyPassword=$release_key_password")
else
  gradle_signing_properties=()
fi

"$GRADLE_BIN" --no-daemon "${gradle_signing_properties[@]}" "$@"
