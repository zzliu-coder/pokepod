#!/bin/zsh

set -u
setopt pipefail

ADB="/opt/homebrew/bin/adb"
DEVICE="BE87E832"
SOURCE_ROOT="/Users/zheliu/Library/Mobile Documents/iCloud~com~apple~iBooks/Documents"
REMOTE_DIR="/sdcard/Books/Books"
RUN_TAG="$(/bin/date +%Y%m%d-%H%M%S)"
WORK_ROOT="/Users/zheliu/Documents/Codex/2026-07-27/referenced-chatgpt-conversation-this-is-untrusted-3/work/Poke3TransferApp/repair-$RUN_TAG"
MANIFEST="$WORK_ROOT/manifest.tsv"
LIMIT="${LIMIT:-0}"

/bin/mkdir -p "$WORK_ROOT/packages"
print -r -- $'status\tname\tsha256\tnote' >"$MANIFEST"

if [[ "$("$ADB" -s "$DEVICE" get-state 2>/dev/null || true)" != "device" ]]; then
  print "Poke3 ADB 未连接"
  exit 2
fi

"$ADB" -s "$DEVICE" shell svc power stayon true >/dev/null
"$ADB" -s "$DEVICE" shell input keyevent 224 >/dev/null

REMOTE_OUTPUT="$("$ADB" -s "$DEVICE" shell \
  "for p in '$REMOTE_DIR'/*.epub; do [ -d \"\$p\" ] && printf '%s\n' \"\$p\"; done" |
  /usr/bin/tr -d '\r')"
if [[ -n "$REMOTE_OUTPUT" ]]; then
  REMOTE_PATHS=("${(@f)REMOTE_OUTPUT}")
else
  REMOTE_PATHS=()
fi
TOTAL=${#REMOTE_PATHS}
[[ "$LIMIT" -gt 0 && "$LIMIT" -lt "$TOTAL" ]] && TOTAL="$LIMIT"

print "待修复：$TOTAL 本"
success=0
failed=0
index=0

for remote_path in "${REMOTE_PATHS[@]}"; do
  (( index += 1 ))
  [[ "$LIMIT" -gt 0 && "$index" -gt "$LIMIT" ]] && break

  name="${remote_path:t}"
  source_path="$SOURCE_ROOT/$name"
  package_dir="$WORK_ROOT/packages/$index"
  package_path="$package_dir/$name"
  remote_temp="$REMOTE_DIR/.poke3-repair-$RUN_TAG-$index.epub"
  remote_backup="$REMOTE_DIR/.poke3-original-$RUN_TAG-$index"

  print
  print "[$index/$TOTAL] $name"

  if [[ ! -d "$source_path" ]]; then
    print "  跳过：Mac 原始包不存在"
    print -r -- "failed"$'\t'"$name"$'\t\t'"local source missing" >>"$MANIFEST"
    (( failed += 1 ))
    continue
  fi

  if [[ ! -f "$source_path/mimetype" || ! -f "$source_path/META-INF/container.xml" ]]; then
    print "  跳过：EPUB 结构不完整"
    print -r -- "failed"$'\t'"$name"$'\t\t'"invalid source package" >>"$MANIFEST"
    (( failed += 1 ))
    continue
  fi

  /bin/mkdir -p "$package_dir"
  (
    cd "$source_path" &&
      /usr/bin/zip -q -X0 "$package_path" mimetype &&
      /usr/bin/zip -q -Xr9D "$package_path" . \
        -x mimetype '*.DS_Store' '__MACOSX/*'
  )
  if [[ "$?" -ne 0 ]] || ! /usr/bin/unzip -tqq "$package_path"; then
    print "  跳过：本地封装校验失败"
    print -r -- "failed"$'\t'"$name"$'\t\t'"local zip validation failed" >>"$MANIFEST"
    /bin/rm -rf "$package_dir"
    (( failed += 1 ))
    continue
  fi

  first_entry="$(/usr/bin/zipinfo -1 "$package_path" | /usr/bin/head -1)"
  compression="$(/usr/bin/zipinfo -v "$package_path" | /usr/bin/awk '
    /mimetype/ { found=1 }
    found && /compression method:/ { print; exit }
  ')"
  if [[ "$first_entry" != "mimetype" || "$compression" != *"none (stored)"* ]]; then
    print "  跳过：mimetype 顺序或压缩方式错误"
    print -r -- "failed"$'\t'"$name"$'\t\t'"invalid mimetype placement" >>"$MANIFEST"
    /bin/rm -rf "$package_dir"
    (( failed += 1 ))
    continue
  fi

  local_sha="$(/usr/bin/shasum -a 256 "$package_path" | /usr/bin/awk '{print $1}')"
  if ! "$ADB" -s "$DEVICE" push "$package_path" "$remote_temp" >/dev/null; then
    print "  跳过：上传临时文件失败"
    print -r -- "failed"$'\t'"$name"$'\t'"$local_sha"$'\t'"temporary upload failed" >>"$MANIFEST"
    /bin/rm -rf "$package_dir"
    (( failed += 1 ))
    continue
  fi

  remote_sha="$("$ADB" -s "$DEVICE" shell "sha256sum ${(q)remote_temp}" 2>/dev/null |
    /usr/bin/awk '{print $1}' | /usr/bin/tr -d '\r')"
  if [[ "$remote_sha" != "$local_sha" ]]; then
    "$ADB" -s "$DEVICE" shell "rm -f ${(q)remote_temp}" >/dev/null 2>&1
    print "  跳过：上传哈希不一致"
    print -r -- "failed"$'\t'"$name"$'\t'"$local_sha"$'\t'"temporary hash mismatch" >>"$MANIFEST"
    /bin/rm -rf "$package_dir"
    (( failed += 1 ))
    continue
  fi

  replace_command="mv ${(q)remote_path} ${(q)remote_backup} && mv ${(q)remote_temp} ${(q)remote_path}"
  if ! "$ADB" -s "$DEVICE" shell "$replace_command" >/dev/null; then
    "$ADB" -s "$DEVICE" shell "rm -f ${(q)remote_temp}" >/dev/null 2>&1
    print "  跳过：替换操作失败，原目录保留"
    print -r -- "failed"$'\t'"$name"$'\t'"$local_sha"$'\t'"replace failed" >>"$MANIFEST"
    /bin/rm -rf "$package_dir"
    (( failed += 1 ))
    continue
  fi

  final_sha="$("$ADB" -s "$DEVICE" shell "sha256sum ${(q)remote_path}" 2>/dev/null |
    /usr/bin/awk '{print $1}' | /usr/bin/tr -d '\r')"
  if [[ "$final_sha" == "$local_sha" ]]; then
    "$ADB" -s "$DEVICE" shell "rm -rf ${(q)remote_backup}" >/dev/null
    print "  完成：哈希一致"
    print -r -- "success"$'\t'"$name"$'\t'"$local_sha"$'\t'"verified and replaced" >>"$MANIFEST"
    (( success += 1 ))
  else
    rollback_command="rm -f ${(q)remote_path} && mv ${(q)remote_backup} ${(q)remote_path}"
    "$ADB" -s "$DEVICE" shell "$rollback_command" >/dev/null 2>&1
    print "  失败：最终校验异常，已恢复原目录"
    print -r -- "failed"$'\t'"$name"$'\t'"$local_sha"$'\t'"final verification failed; rolled back" >>"$MANIFEST"
    (( failed += 1 ))
  fi

  /bin/rm -rf "$package_dir"
done

print
print "完成：成功 $success，本；失败 $failed 本"
print "记录：$MANIFEST"
[[ "$failed" -eq 0 ]]
