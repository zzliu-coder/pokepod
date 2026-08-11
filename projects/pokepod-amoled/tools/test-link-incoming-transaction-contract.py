#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
service = (root / "firmware/PokePodAmoled/PokePodLinkService.cpp").read_text()
header = (root / "firmware/PokePodAmoled/PokePodLinkService.h").read_text()

for path in (
    "/PokeCapsule/.system/commands/incoming/upload.part",
    "/PokeCapsule/.staging/link-upload.part",
    "/PokeCapsule/.system/fonts/cjk20.a4.part",
):
    assert path in service

assert "transactionId + \".json.part\"" not in service
assert "TransactionPurpose::startupRecovery" in service
assert "transactionRunner_.startRecovery(StorageOwner::capsuleTransaction)" in service
assert "if (!startupReady_) return;" in service
assert "advanceStartupPartCleanup();" in service
assert "transactionPreparedPath_ = temporary;" in service
assert "preparedPath, incomingStorageReservation_" in service
assert service.index("incomingCleanup_.begin(") < service.index(
    "incomingStorageReservation_.release();", service.index(
        "void PokePodLinkService::finishIncomingTransaction"))
assert "transactionPurpose_ == TransactionPurpose::incoming ||" in service
assert "transactionPurpose_ == TransactionPurpose::commandText" in service
assert "? &transactionGate_ : nullptr" in (" ".join(service.split()))
assert "String transactionPreparedPath_;" in header

print("link incoming transaction contract: PASS")
