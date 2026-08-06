#!/bin/sh
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
TEST_TMP_DIR=$(mktemp -d "${TMPDIR:-/tmp}/pokepod-host-tests.XXXXXX")
trap 'rm -rf -- "$TEST_TMP_DIR"' EXIT HUP INT TERM

for source in "$SCRIPT_DIR"/tests/test_*.cpp; do
  name=$(basename "$source" .cpp)
  clang++ -std=c++17 -Wall -Wextra -Werror \
    -I"$SCRIPT_DIR/PokePodAmoled" \
    "$source" -o "$TEST_TMP_DIR/$name"
  "$TEST_TMP_DIR/$name"
  printf 'PASS %s\n' "$name"
done
