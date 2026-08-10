#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "PokePodAmoled"

library_h = (FIRMWARE / "CapsuleLibrary.h").read_text()
library_cpp = (FIRMWARE / "CapsuleLibrary.cpp").read_text()
ui_policy = (FIRMWARE / "UiPolicy.h").read_text()
dashboard = (FIRMWARE / "Dashboard.cpp").read_text()
app = (FIRMWARE / "PokePodAmoled.ino").read_text()
link = (FIRMWARE / "PokePodLinkService.cpp").read_text()

assert "bool readOnly = false" in library_h
assert "CapsuleBatchResult purge(const std::vector<String> &ids)" in library_h
assert "record.readOnly = !capsuleRecordWritable(metadata)" in library_cpp
assert "!record.readOnly && !record.archived" in library_cpp
assert "record == nullptr || record->readOnly" in library_cpp

purge_start = library_cpp.index("CapsuleBatchResult CapsuleLibrary::purge")
purge_end = library_cpp.index("CapsuleBatchResult CapsuleLibrary::batch", purge_start)
purge = library_cpp[purge_start:purge_end]
assert purge.index("fs_->rename(source, target)") < purge.index("removeTree(transaction")
assert "purge-local-" in purge
assert "rolledBackFully" in purge

assert "requestPurge" in ui_policy and "confirmPurge" in ui_policy
assert '"永久删除"' in dashboard
assert '"录音和文字将无法恢复"' in dashboard
assert "capsuleLibrary.purge(pendingPurgeIds)" in app
assert '"版本过新，请在 Mac 处理"' in app
assert 'strcmp(operation, "purgeCapsules") == 0' in link
assert "library_->purge(ids)" in link

print("capsule local management contract: PASS")
