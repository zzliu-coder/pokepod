#!/bin/zsh

set -u

ADB_BIN="/opt/homebrew/bin/adb"
DEVICE_ID="BE87E832"
QUEUE_DIR=""
QUEUE_PIPE=""
WORKER_PID=""

show_message() {
  /usr/bin/osascript -e "display dialog \"$1\" buttons {\"好\"} default button \"好\" with title \"Poke3 传书\""
}

choose_destination() {
  local destination
  destination=$(/usr/bin/osascript <<'APPLESCRIPT'
set destinationChoices to {"漫画（Books/Comics）", "图书（Books/Books）", "收件箱（Books/Inbox）", "下载（Download）"}
set pickedDestination to choose from list destinationChoices with prompt "选择文件要放到 Poke3 的哪个目录" default items {"漫画（Books/Comics）"} OK button name "确定" cancel button name "取消"
if pickedDestination is false then error number -128
return item 1 of pickedDestination
APPLESCRIPT
  ) || return 1

  case "$destination" in
    "漫画（Books/Comics）")
      TARGET_DIR="/sdcard/Books/Comics"
      TARGET_LABEL="存储 → Books → Comics"
      ;;
    "图书（Books/Books）")
      TARGET_DIR="/sdcard/Books/Books"
      TARGET_LABEL="存储 → Books → Books"
      ;;
    "下载（Download）")
      TARGET_DIR="/sdcard/Download"
      TARGET_LABEL="存储 → Download"
      ;;
    *)
      TARGET_DIR="/sdcard/Books/Inbox"
      TARGET_LABEL="存储 → Books → Inbox"
      ;;
  esac

  "$ADB_BIN" -s "$DEVICE_ID" shell mkdir -p "$TARGET_DIR"
}

queue_worker() {
  local target_dir
  local item_path
  local item_name

  while IFS=$'\t' read -r target_dir item_path; do
    [[ -z "$target_dir" || -z "$item_path" ]] && continue
    item_name="${item_path:t}"
    print
    print -r -- "▶ 开始：$item_name"
    if "$ADB_BIN" -s "$DEVICE_ID" push "$item_path" "$target_dir/" \
        >"$QUEUE_DIR/last-transfer.log" 2>&1; then
      print -r -- "✓ 完成：$item_name"
    else
      print -r -- "✗ 失败：$item_name"
      /bin/cat "$QUEUE_DIR/last-transfer.log"
    fi
  done
}

start_queue() {
  QUEUE_DIR=$(/usr/bin/mktemp -d /tmp/poke3-transfer.XXXXXX) || return 1
  QUEUE_PIPE="$QUEUE_DIR/queue"
  /usr/bin/mkfifo "$QUEUE_PIPE" || return 1
  queue_worker <"$QUEUE_PIPE" &
  WORKER_PID=$!
  exec 3>"$QUEUE_PIPE"
}

enqueue_paths() {
  local item_path
  local item_name
  local queued_count=0

  for item_path in "$@"; do
    [[ -z "$item_path" ]] && continue
    if [[ ! -e "$item_path" ]]; then
      print -r -- "找不到：$item_path"
      continue
    fi
    item_name="${item_path:t}"
    print -r -- "$TARGET_DIR"$'\t'"$item_path" >&3
    print -r -- "＋ 已加入队列：$item_name"
    (( queued_count += 1 ))
  done

  print -r -- "本次加入 $queued_count 项；可继续拖入。"
}

finish_queue() {
  if [[ -n "$WORKER_PID" ]]; then
    exec 3>&- 2>/dev/null || true
    print
    print "正在等待队列传完……"
    wait "$WORKER_PID" 2>/dev/null || true
    WORKER_PID=""
  fi
  if [[ -n "$QUEUE_DIR" && -d "$QUEUE_DIR" ]]; then
    /bin/rm -f "$QUEUE_PIPE" "$QUEUE_DIR/last-transfer.log"
    /bin/rmdir "$QUEUE_DIR" 2>/dev/null || true
    QUEUE_DIR=""
  fi
}

if [[ ! -x "$ADB_BIN" ]]; then
  show_message "没有找到 ADB。"
  exit 1
fi

DEVICE_STATE="$("$ADB_BIN" -s "$DEVICE_ID" get-state 2>/dev/null || true)"
if [[ "$DEVICE_STATE" != "device" ]]; then
  "$ADB_BIN" start-server >/dev/null 2>&1
  for attempt in {1..8}; do
    sleep 1
    DEVICE_STATE="$("$ADB_BIN" -s "$DEVICE_ID" get-state 2>/dev/null || true)"
    [[ "$DEVICE_STATE" == "device" ]] && break
  done
fi

if [[ "$DEVICE_STATE" != "device" ]]; then
  if /usr/sbin/system_profiler SPUSBDataType 2>/dev/null | /usr/bin/grep -q "ONYX"; then
    show_message "Poke3 已插好 USB，但当前只有文件传输接口，ADB 接口没有出现。请在 Poke3 上把 USB 调试关闭再打开，然后重试。"
  else
    show_message "Mac 没有识别到 Poke3。请开机并重新插好 USB 数据线。"
  fi
  exit 1
fi

choose_destination || exit 0
"$ADB_BIN" -s "$DEVICE_ID" shell svc power stayon true >/dev/null 2>&1 || true
"$ADB_BIN" -s "$DEVICE_ID" shell input keyevent 224 >/dev/null 2>&1 || true
start_queue || {
  show_message "无法创建传输队列。"
  exit 1
}
trap finish_queue EXIT HUP INT TERM

print
print "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
print "拖拽传书已开启"
print "把文件或文件夹拖进窗口，按回车加入队列。"
print "传输过程中可以继续拖入；队列会按顺序处理。"
print "当前目标：$TARGET_LABEL"
print "输入 d 更换后续文件的目标目录；输入 q 等待队列完成后退出。"
print "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

while true; do
  print
  print -n "拖入文件/文件夹 > "
  IFS= read -r DROP_LINE || break

  case "$DROP_LINE" in
    q|Q)
      break
      ;;
    d|D)
      choose_destination || true
      continue
      ;;
    "")
      continue
      ;;
  esac

  RAW_PATHS=(${(z)DROP_LINE})
  DROPPED_PATHS=("${(@Q)RAW_PATHS}")
  enqueue_paths "${DROPPED_PATHS[@]}"
done

finish_queue
trap - EXIT HUP INT TERM
print
print "队列已完成，可以关闭窗口。"
