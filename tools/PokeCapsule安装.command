#!/bin/zsh
set -euo pipefail

ROOT_DIR="${0:A:h:h}"
PACKAGE_ID="com.zheliu.pokecapsule"
MODEL_NAME="ggml-tiny-q5_1.bin"
MODEL_SHA256="818710568da3ca15689e31a743197b520007872ff9576237bda97bd1b469c3d7"
REMOTE_ROOT="/sdcard/PokeCapsule"

find_adb() {
  local candidate
  for candidate in \
    "$ROOT_DIR/tools/adb" \
    "/opt/homebrew/bin/adb" \
    "$ANDROID_HOME/platform-tools/adb" \
    "$ANDROID_SDK_ROOT/platform-tools/adb"; do
    if [[ -n "$candidate" && -x "$candidate" ]]; then
      print -r -- "$candidate"
      return 0
    fi
  done
  if command -v adb >/dev/null 2>&1; then
    command -v adb
    return 0
  fi
  return 1
}

ADB="$(find_adb)" || {
  print "没有找到 Android 调试工具。"
  read "?按回车退出。"
  exit 1
}

"$ADB" start-server >/dev/null
devices=("${(@f)$("$ADB" devices | awk 'NR>1 && $2==\"device\" {print $1}')}")
unauthorized=("${(@f)$("$ADB" devices | awk 'NR>1 && $2==\"unauthorized\" {print $1}')}")

if (( ${#unauthorized[@]} > 0 )); then
  print "Poke3 还未授权。请在屏幕上勾选“始终允许”，再点允许。"
  read "?完成后按回车继续。"
  devices=("${(@f)$("$ADB" devices | awk 'NR>1 && $2==\"device\" {print $1}')}")
fi

if (( ${#devices[@]} != 1 )); then
  print "需要恰好连接一台已授权的 Android 设备；当前找到 ${#devices[@]} 台。"
  read "?按回车退出。"
  exit 1
fi

SERIAL="${devices[1]}"
MODEL_PATH="$ROOT_DIR/artifacts/models/$MODEL_NAME"
APK_PATH="$(find "$ROOT_DIR/projects/pokecapsule-android/app/build/outputs/apk" -type f -name '*release*.apk' -o -name '*debug*.apk' 2>/dev/null | head -1)"

if [[ -z "$APK_PATH" || ! -f "$APK_PATH" ]]; then
  print "没有找到 PokeCapsule APK，请先完成构建。"
  read "?按回车退出。"
  exit 1
fi

if [[ ! -f "$MODEL_PATH" ]]; then
  print "缺少中文转写模型：$MODEL_PATH"
  read "?按回车退出。"
  exit 1
fi

actual_model_hash="$(shasum -a 256 "$MODEL_PATH" | awk '{print $1}')"
if [[ "$actual_model_hash" != "$MODEL_SHA256" ]]; then
  print "模型校验失败，停止安装。"
  read "?按回车退出。"
  exit 1
fi

old_stay_on="$("$ADB" -s "$SERIAL" shell settings get global stay_on_while_plugged_in | tr -d '\r')"
restore_stay_on() {
  if [[ "$old_stay_on" == "null" || -z "$old_stay_on" ]]; then
    "$ADB" -s "$SERIAL" shell settings delete global stay_on_while_plugged_in >/dev/null 2>&1 || true
  else
    "$ADB" -s "$SERIAL" shell settings put global stay_on_while_plugged_in "$old_stay_on" >/dev/null 2>&1 || true
  fi
}
trap restore_stay_on EXIT INT TERM

"$ADB" -s "$SERIAL" shell settings put global stay_on_while_plugged_in 7
"$ADB" -s "$SERIAL" shell input keyevent KEYCODE_WAKEUP

print "正在安装 PokeCapsule…"
"$ADB" -s "$SERIAL" install -r -d "$APK_PATH"

"$ADB" -s "$SERIAL" shell mkdir -p \
  "$REMOTE_ROOT/Inbox" \
  "$REMOTE_ROOT/Archive" \
  "$REMOTE_ROOT/.staging" \
  "$REMOTE_ROOT/.locks" \
  "$REMOTE_ROOT/.commands" \
  "$REMOTE_ROOT/.trash" \
  "$REMOTE_ROOT/.models"

print "正在复制中文转写模型…"
"$ADB" -s "$SERIAL" push "$MODEL_PATH" "$REMOTE_ROOT/.models/$MODEL_NAME"
remote_model_hash="$("$ADB" -s "$SERIAL" shell sha256sum "$REMOTE_ROOT/.models/$MODEL_NAME" | awk '{print $1}' | tr -d '\r')"
if [[ "$remote_model_hash" != "$MODEL_SHA256" ]]; then
  print "设备上的模型校验失败，停止启动。"
  read "?按回车退出。"
  exit 1
fi

"$ADB" -s "$SERIAL" shell pm grant "$PACKAGE_ID" android.permission.RECORD_AUDIO >/dev/null 2>&1 || true
"$ADB" -s "$SERIAL" shell pm grant "$PACKAGE_ID" android.permission.READ_EXTERNAL_STORAGE >/dev/null 2>&1 || true
"$ADB" -s "$SERIAL" shell pm grant "$PACKAGE_ID" android.permission.WRITE_EXTERNAL_STORAGE >/dev/null 2>&1 || true
"$ADB" -s "$SERIAL" shell appops set "$PACKAGE_ID" SYSTEM_ALERT_WINDOW allow >/dev/null 2>&1 || true

"$ADB" -s "$SERIAL" shell monkey -p "$PACKAGE_ID" -c android.intent.category.LAUNCHER 1 >/dev/null

print
print "PokeCapsule 已安装并启动。"
print "模型 SHA-256 校验通过。"
print "如果悬浮按钮没有出现，请在 PokeCapsule 的“设置”里点一次“允许悬浮按钮”。"
read "?按回车关闭此窗口。"

