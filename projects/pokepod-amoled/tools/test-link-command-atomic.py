#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = (ROOT / "firmware/PokePodAmoled/PokePodLinkService.cpp").read_text()
HEADER = (ROOT / "firmware/PokePodAmoled/PokePodLinkService.h").read_text()
EXECUTOR = (ROOT / "firmware/PokePodAmoled/LinkCommandExecutor.h").read_text()
TREE = (ROOT / "firmware/PokePodAmoled/LinkTreeStepper.cpp").read_text()
TREE_HEADER = (ROOT / "firmware/PokePodAmoled/LinkTreeStepper.h").read_text()


def body(start: str, end: str) -> str:
    left = CPP.index(start)
    right = CPP.index(end, left + len(start))
    return CPP[left:right]


# Mutating commands have one durable, cooperative execution path.  The old
# synchronous batch helpers must stay deleted so deadline and recovery policy
# cannot be bypassed by a fallback.
for legacy in (
    "PokePodLinkService::mutateFavoriteOrTags(",
    "PokePodLinkService::moveOrCopy(",
    "PokePodLinkService::trashOperation(",
    "PokePodLinkService::folderOperation(",
    "PokePodLinkService::copyTree(",
    "PokePodLinkService::collectFiles(",
):
    assert legacy not in CPP

handle = body("void PokePodLinkService::handleCommandFile(",
              "bool PokePodLinkService::tryStartTextCommand(")
advance = body("void PokePodLinkService::advanceBatchCommand(",
               "bool PokePodLinkService::startBatchWork(")
work = body("bool PokePodLinkService::startBatchWork(",
            "void PokePodLinkService::finishBatchWork(")
cleanup = body("void PokePodLinkService::finishCommandStorageCleanup(",
               "bool PokePodLinkService::safeFolder(")

assert "BatchStart::started" in handle
assert "BatchStart::rejected" in handle
assert "beginCommandLoad" in handle
assert "executeCommand" not in CPP
assert "readText(path, 128U * 1024U)" not in CPP
assert "loadStatus(transactionId, owner, state)" in CPP
assert "startupBatchCandidateInvalid_" in CPP
assert "mutationRecoveryBlocked_ && !alreadyComplete" in CPP
assert "applyCompletedCommandSideEffects(root);" in CPP
assert "applyDurableCommandSideEffects(batchJournalState_.operation" in CPP
assert "!batchExecutor_.success()) return;" in CPP
assert "!batchExecutor_.responseAllowed()) return" not in CPP
assert "batchNextIdItem_" in CPP
assert "rememberBatchId(rawId)" in CPP
assert "cJSON_GetArrayItem(ids, static_cast<int>(prior))" not in CPP
loader = body("void PokePodLinkService::advanceCommandLoad(",
              "void PokePodLinkService::finishCommandLoad(")
assert "kCommandReadBytesPerPoll" in loader
assert "StorageAccess::read, 0" in loader
assert "transferPermitted()" in loader
assert "batchForegroundPermitted" in advance
assert "batchExecutor_.poll" in advance
for action in (
    "checkpoint", "preflightItem", "applyItem", "finalizeApply",
    "rollbackFinalize", "rollbackItem", "persistResult", "cleanup",
):
    assert f"Action::{action}" in work

assert "sessionActive_ && transferPermitted()" in CPP
assert "commandStorageReservation_" in handle
assert "requestId != 0" in cleanup
assert "deferredCommandFiles_.empty()" in cleanup
assert "deferredTreeCleanupStack_.empty()" in cleanup
assert "commandStorageReservation_.release()" in cleanup

# The executor advances at most one item/action per poll and treats an in-flight
# apply as uncertain, so a live failure rolls that item back as boot recovery
# does.  Journal evidence survives blocked result/cleanup phases.
assert "kMaximumItems = 500" in EXECUTOR
assert "if (applied_ < total_) ++applied_;" in EXECUTOR
assert "preserveJournal_" in EXECUTOR
assert "kMaximumCleanupFailures" in EXECUTOR
assert "kMaximumResultFailures" in EXECUTOR

# Recursive copies are cooperative and independently verify the destination
# before it can become backup or publish authority.
assert "kBytesPerPoll = 4096" in TREE_HEADER
assert "sourceCrcState_" in TREE and "targetCrcState_" in TREE
assert "sourceLength_" in TREE and "targetLength_" in TREE
assert "FileCopyPhase::verify" in TREE

# Wi-Fi never waits in storage arbitration; USB may use its bounded timeout.
storage_exists = body("bool PokePodLinkService::storageExists(",
                      "bool PokePodLinkService::storageRename(")
assert "storageIoTimeout()" in storage_exists
assert "1000" not in storage_exists

assert "LinkCommandExecutor batchExecutor_" in HEADER
assert "LinkTreeStepper batchTreeStepper_" in HEADER

print("PASS link_command_atomic_contract")
