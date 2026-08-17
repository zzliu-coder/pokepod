#!/bin/sh
set -eu

# Keep the Arduino CLI supply input deterministic without a JavaScript setup
# Action. The upstream v1.5.0 release checksum is pinned per supported host.

version=${ARDUINO_CLI_VERSION:-1.5.0}
destination=${ARDUINO_CLI_INSTALL_DIR:-"$HOME/.local/bin"}
case "$version" in
  1.5.0) ;;
  *)
    printf 'Unsupported locked Arduino CLI version: %s\n' "$version" >&2
    exit 64
    ;;
esac

case "$(uname -s):$(uname -m)" in
  Linux:x86_64)
    archive="arduino-cli_1.5.0_Linux_64bit.tar.gz"
    expected="f4e49fb6f5d6a043f7df792ef057143c542140508081aaa52d214a9ab33141c8"
    ;;
  Linux:aarch64|Linux:arm64)
    archive="arduino-cli_1.5.0_Linux_ARM64.tar.gz"
    expected="1f601bdcd2fbe04ec029cea349680508604bac50f6c2b14dc508930d56d93241"
    ;;
  Darwin:x86_64)
    archive="arduino-cli_1.5.0_macOS_64bit.tar.gz"
    expected="7127f113670ae2d2677d7029d1f47359592c6b0d3bb14627bcc80dcb76566cef"
    ;;
  Darwin:arm64)
    archive="arduino-cli_1.5.0_macOS_ARM64.tar.gz"
    expected="62f3c5749e6b7f8a3d2a147da4fe32d704c1319ff40d4ba05e9d85f27b784404"
    ;;
  *)
    printf 'Unsupported Arduino CLI host: %s %s\n' "$(uname -s)" "$(uname -m)" >&2
    exit 69
    ;;
esac

mkdir -p "$destination"
if [ -x "$destination/arduino-cli" ]; then
  installed=$("$destination/arduino-cli" version | sed -n '1p')
  case "$installed" in
    *"Version: $version"*|*"version $version"*)
      printf '%s\n' "$installed"
      printf 'PASS locked_arduino_cli\n'
      exit 0
      ;;
  esac
fi

temporary=$(mktemp -d "${TMPDIR:-/tmp}/pokepod-arduino-cli.XXXXXX")
trap 'rm -rf "$temporary"' EXIT INT TERM HUP
url="https://github.com/arduino/arduino-cli/releases/download/v${version}/${archive}"
curl -fL --retry 3 --connect-timeout 20 "$url" -o "$temporary/$archive"
printf '%s  %s\n' "$expected" "$temporary/$archive" | sha256sum -c -
tar -xzf "$temporary/$archive" -C "$temporary" arduino-cli
install -m 0755 "$temporary/arduino-cli" "$destination/arduino-cli"
installed=$("$destination/arduino-cli" version | sed -n '1p')
case "$installed" in
  *"Version: $version"*|*"version $version"*) ;;
  *)
    printf 'Unexpected Arduino CLI version: %s\n' "$installed" >&2
    exit 65
    ;;
esac
printf '%s\n' "$installed"
printf 'PASS locked_arduino_cli\n'
