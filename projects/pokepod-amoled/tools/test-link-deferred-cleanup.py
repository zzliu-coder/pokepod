#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
service = (root / "firmware/PokePodAmoled/PokePodLinkService.cpp").read_text()
header = (root / "firmware/PokePodAmoled/PokePodLinkService.h").read_text()
helper = (root / "firmware/PokePodAmoled/DeferredFileCleanup.h").read_text()

required_service = (
    "incomingCleanupPending_",
    "outgoingCleanupPending_",
    "pollDeferredCleanup();",
    "incomingCleanup_.poll()",
    "outgoingCleanup_.poll()",
    "maintenanceCompletion_.resultFetched",
)
for token in required_service:
    assert token in service or token in header, token

assert "if (!lease && outgoingFile_) outgoingFile_.close()" not in service
assert "if (incomingFile_) incomingFile_.close();" not in service
assert "if (!lease) return false;" in helper
assert helper.index("if (!lease) return false;") < helper.index("file_->close()")
assert helper.index("file_->close()") < helper.index("reservation_->release()")
print("PASS link_deferred_cleanup_contract")
