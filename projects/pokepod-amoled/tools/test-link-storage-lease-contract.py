#!/usr/bin/env python3

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = (ROOT / "firmware/PokePodAmoled/PokePodLinkService.cpp").read_text()


def methods(source: str):
    matches = list(re.finditer(r"(?:bool|void|String|StorageOwner)\s+"
                               r"PokePodLinkService::([A-Za-z0-9_]+)\s*\(",
                               source))
    for index, match in enumerate(matches):
        end = matches[index + 1].start() if index + 1 < len(matches) else len(source)
        yield match.group(1), source[match.start():end]


direct_fs_allowed = {
    "beginIncoming",
    "beginCommandLoad",
    "advanceCommandLoad",
    "advanceBatchStartupRecovery",
    "advanceStartupPartCleanup",
    "advanceManifestScan",
    "advanceManifestFile",
    "cleanupPurgeStaging",
    "stepDeferredTreeCleanup",
    "sendFile",
    "storageExists",
    "storageRename",
    "storageRemove",
    "storageMkdir",
    "storageRmdir",
    "validFontFile",
    "readText",
}

for name, text in methods(CPP):
    if "fs_->" in text:
        assert name in direct_fs_allowed, f"raw FS operation in {name}"
        assert "acquireIo(" in text, f"no owner-scoped lease in {name}"

assert "StorageAccess::read, 1000" not in CPP
assert "StorageAccess::mutation, 1000" not in CPP
assert "return transferGate_ == nullptr ? 1000U : 0U" in CPP

exists = dict(methods(CPP))["storageExists"]
assert "storageIoTimeout()" in exists
assert "1000" not in exists
assert exists.count("transferPermitted()") >= 2

for name in ("handleImmediate",):
    text = dict(methods(CPP))[name]
    assert "fs_->" not in text, f"command path bypasses storage helper: {name}"
assert "PokePodLinkService::executeCommand" not in CPP

loader = dict(methods(CPP))["advanceCommandLoad"]
assert "kCommandReadBytesPerPoll" in loader
assert "StorageAccess::read, 0" in loader
assert "transferPermitted()" in loader

deferred = dict(methods(CPP))["deferStorageFile"]
assert "storageOwner()" in deferred
assert "file = File()" in deferred

finish_files = dict(methods(CPP))["finishCopiedFiles"]
step_files = dict(methods(CPP))["stepDeferredFileCleanup"]
assert "getWriteError()" in finish_files
assert "return ok;" in finish_files
assert "getWriteError()" in step_files
assert "deferredCommandFileFailed_ = true" in step_files

poll = dict(methods(CPP))["poll"]
assert "!deferredCommandFiles_.empty()" in poll
assert "!deferredTreeCleanupStack_.empty()" in poll

print("PASS link_storage_lease_contract")
