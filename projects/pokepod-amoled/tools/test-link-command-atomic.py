#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = (ROOT / "firmware/PokePodAmoled/PokePodLinkService.cpp").read_text()


def body(start: str, end: str) -> str:
    left = CPP.index(start)
    right = CPP.index(end, left + len(start))
    return CPP[left:right]


favorite = body("bool PokePodLinkService::mutateFavoriteOrTags(",
                "bool PokePodLinkService::moveOrCopy(")
move_copy = body("bool PokePodLinkService::moveOrCopy(",
                 "bool PokePodLinkService::trashOperation(")
trash = body("bool PokePodLinkService::trashOperation(",
             "bool PokePodLinkService::folderOperation(")
folder = body("bool PokePodLinkService::folderOperation(",
              "bool PokePodLinkService::cleanupPurgeStaging(")
copy_tree = body("bool PokePodLinkService::copyTree(",
                 "bool PokePodLinkService::rewriteCopiedMetadata(")
collect = body("bool PokePodLinkService::collectFiles(",
               "String PokePodLinkService::deviceId(")
handle = body("void PokePodLinkService::handleCommandFile(",
              "void PokePodLinkService::finishCommandStorageCleanup(")
finish_cleanup = body("void PokePodLinkService::finishCommandStorageCleanup(",
                      "bool PokePodLinkService::safeFolder(")
file_cleanup = body("void PokePodLinkService::closeStorageFile(",
                    "bool PokePodLinkService::writeTextAtomic(")

assert "String original" in favorite
assert favorite.count("rollbackLinkBatch") >= 2
assert "transferPermitted()" in favorite
assert favorite.count("(void)transferPermitted()") >= 4

assert "struct PlannedTransfer" in move_copy
assert "originalCapsule" in move_copy
assert "rollbackLinkBatch" in move_copy
assert "removeTree(plan.target)" in move_copy
assert "bool failed = false" in move_copy
assert "++applied;" in move_copy
assert "if (failed || applied != plans.size())" in move_copy
assert move_copy.count("(void)transferPermitted()") >= 4

assert "struct PlannedTrashMutation" in trash
assert "originalTrash" in trash and "originalCapsule" in trash
assert "rollbackLinkBatch" in trash
assert trash.count("(void)transferPermitted()") >= 4

assert "if (!collectFiles" in folder
assert "folder contents changed during preflight" in folder
assert "purge-folder-" in folder
assert "rollbackLinkBatch" in folder
assert folder.count("(void)transferPermitted()") >= 2

assert "uint8_t buffer[4096]" in copy_tree
assert copy_tree.count("transferPermitted()") >= 4
assert "openNextFile" in copy_tree

assert collect.count("transferPermitted()") >= 4
assert "openNextFile" in collect

assert "commandStorageReservation_" in handle
assert "commandCleanupPending_" in handle
assert "commandStorageReservation_.release()" in finish_cleanup
assert "deferredCommandFiles_.empty()" in finish_cleanup
assert "deferredTreeCleanupStack_.empty()" in finish_cleanup
assert "while (file)" not in file_cleanup
assert "deferStorageFile" in file_cleanup
assert "bool PokePodLinkService::finishCopiedFiles" in file_cleanup
assert "return false;" in file_cleanup

print("PASS link_command_atomic_contract")
