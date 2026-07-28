#!/bin/zsh
set -euo pipefail

PACKAGE_ID="com.zheliu.pokecapsule"
ADB="/opt/homebrew/bin/adb"

if [[ ! -x "$ADB" ]] && command -v adb >/dev/null 2>&1; then
  ADB="$(command -v adb)"
fi

if [[ ! -x "$ADB" ]]; then
  print "没有找到 Android 调试工具。"
  read "?按回车退出。"
  exit 1
fi

devices=("${(@f)$("$ADB" devices | awk 'NR>1 && $2==\"device\" {print $1}')}")
if (( ${#devices[@]} != 1 )); then
  print "需要恰好连接一台已授权的 Android 设备。"
  read "?按回车退出。"
  exit 1
fi

"$ADB" -s "${devices[1]}" uninstall "$PACKAGE_ID" || true
print "应用已卸载。/sdcard/PokeCapsule 中的录音和文字全部保留。"
read "?按回车关闭此窗口。"
