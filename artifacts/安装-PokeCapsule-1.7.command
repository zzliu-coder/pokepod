#!/bin/zsh
set -euo pipefail

SCRIPT_DIR=${0:A:h}
ADB=${ADB:-$(command -v adb || true)}
APK_ARG=${1:-}
APK=${APK_ARG:-}
if [[ -n "$APK" && "$APK" != /* ]]; then
  APK="$SCRIPT_DIR/$APK"
fi
BACKUP_ROOT=${POKECAPSULE_BACKUP_ROOT:-"$HOME/Documents/PokeCapsule-Backups"}
STAMP=$(date +%Y%m%d-%H%M%S)

finish() {
  printf '\n按回车关闭窗口。'
  read -r
}
trap finish EXIT

printf 'PokeCapsule 1.7 安全安装器\n\n'

if [[ ! -x "$ADB" ]]; then
  printf '找不到 ADB：%s\n' "$ADB"
  exit 1
fi

"$ADB" start-server >/dev/null

devices=()
while IFS=$'\t' read -r serial state; do
  [[ "$state" == device ]] && devices+=("$serial")
done < <("$ADB" devices | tail -n +2)

if (( ${#devices[@]} == 0 )); then
  printf '没有在线设备。请保持 Poke3 开机、USB 调试开启，并接受 RSA 授权。\n'
  exit 1
fi

poke_serials=()
for serial in "${devices[@]}"; do
  manufacturer=$("$ADB" -s "$serial" shell getprop ro.product.manufacturer | tr -d '\r')
  model=$("$ADB" -s "$serial" shell getprop ro.product.model | tr -d '\r')
  device=$("$ADB" -s "$serial" shell getprop ro.product.device | tr -d '\r')
  identity="$manufacturer $model $device"
  if [[ "${identity:l}" == *onyx* || "${identity:l}" == *poke3* ]]; then
    poke_serials+=("$serial")
  fi
done

if (( ${#poke_serials[@]} != 1 )); then
  printf '检测到 %d 台在线设备，其中可确认的 Poke3 为 %d 台。为避免装错设备，安装已停止。\n' "${#devices[@]}" "${#poke_serials[@]}"
  "$ADB" devices -l
  exit 1
fi

SERIAL="${poke_serials[1]}"
MANUFACTURER=$("$ADB" -s "$SERIAL" shell getprop ro.product.manufacturer | tr -d '\r')
MODEL=$("$ADB" -s "$SERIAL" shell getprop ro.product.model | tr -d '\r')
DEVICE=$("$ADB" -s "$SERIAL" shell getprop ro.product.device | tr -d '\r')

if [[ -z "$APK" ]]; then
  candidates=()
  while IFS= read -r candidate; do
    [[ -n "$candidate" ]] && candidates+=("$candidate")
  done < <(find "$SCRIPT_DIR" -maxdepth 1 -type f -name '*.apk' -print | sort)

  preferred=()
  for candidate in "${candidates[@]}"; do
    name=$(basename "$candidate" | tr '[:upper:]' '[:lower:]')
    if [[ "$name" == *poke3legacy* || "$name" == *poke3* ]]; then
      preferred+=("$candidate")
    fi
  done
  if (( ${#preferred[@]} == 1 )); then
    APK="${preferred[1]}"
  elif (( ${#candidates[@]} == 1 )); then
    APK="${candidates[1]}"
  elif (( ${#candidates[@]} == 0 )); then
    printf '没有找到 APK。请把 APK 放在安装器旁边，或作为第一个参数传入。\n'
    exit 1
  else
    printf '检测到多个 APK，无法安全猜测版本。请把目标 APK 作为第一个参数传入：\n'
    printf '  %s\n' "${candidates[@]}"
    exit 1
  fi
fi

if [[ ! -f "$APK" ]]; then
  printf '找不到安装包：%s\n' "$APK"
  exit 1
fi
apk_name=$(basename "$APK" | tr '[:upper:]' '[:lower:]')
if [[ "$apk_name" == *phonemodern* ]]; then
  printf '当前目标是 Poke3，拒绝安装 phoneModern APK：%s\n' "$APK"
  exit 1
fi
BACKUP_DIR="$BACKUP_ROOT/$STAMP-$SERIAL"
mkdir -p "$BACKUP_DIR"

{
  printf 'serial=%s\n' "$SERIAL"
  printf 'manufacturer=%s\n' "$MANUFACTURER"
  printf 'model=%s\n' "$MODEL"
  printf 'device=%s\n' "$DEVICE"
  "$ADB" -s "$SERIAL" shell dumpsys package com.zheliu.pokecapsule | grep -E 'versionCode=|versionName=' | head -n 4 || true
} > "$BACKUP_DIR/identity-before.txt"

printf '已确认设备：%s %s（%s）\n' "$MANUFACTURER" "$MODEL" "$SERIAL"
printf '正在备份胶囊资料……\n'
"$ADB" -s "$SERIAL" pull /sdcard/PokeCapsule "$BACKUP_DIR/PokeCapsule" >/dev/null

PACKAGE_PATH=$("$ADB" -s "$SERIAL" shell pm path com.zheliu.pokecapsule | head -n 1 | sed 's/^package://' | tr -d '\r')
if [[ -n "$PACKAGE_PATH" ]]; then
  printf '正在备份旧版 APK……\n'
  "$ADB" -s "$SERIAL" pull "$PACKAGE_PATH" "$BACKUP_DIR/PokeCapsule-before.apk" >/dev/null
fi

find "$BACKUP_DIR" -type f ! -name SHA256SUMS.txt -print0 \
  | xargs -0 shasum -a 256 > "$BACKUP_DIR/SHA256SUMS.txt"

printf '备份完成：%s\n' "$BACKUP_DIR"
printf '正在覆盖安装 1.7.0……\n'
"$ADB" -s "$SERIAL" install -r "$APK"

"$ADB" -s "$SERIAL" shell dumpsys package com.zheliu.pokecapsule | grep -E 'versionCode=22|versionName=1.7.0' > "$BACKUP_DIR/version-after.txt"
if ! grep -q 'versionName=1.7.0' "$BACKUP_DIR/version-after.txt"; then
  printf '安装命令结束，但版本复查未通过。旧 APK 和资料备份均已保留。\n'
  exit 1
fi

"$ADB" -s "$SERIAL" shell am start -n com.zheliu.pokecapsule/.ui.MainActivity >/dev/null
printf '\n安装成功。Poke3 已运行 PokeCapsule 1.7.0，原始录音和胶囊资料保持不变。\n'
