#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
service = (root / "firmware/PokePodAmoled/PokePodLinkService.cpp").read_text()
header = (root / "firmware/PokePodAmoled/PokePodLinkService.h").read_text()
helper = (root / "firmware/PokePodAmoled/DeferredFileCleanup.h").read_text()
file_transfer = (root / "firmware/PokePodAmoled/LinkFileTransfer.cpp").read_text()
file_header = (root / "firmware/PokePodAmoled/LinkFileTransfer.h").read_text()

required_service = (
    "incomingCleanupPending_",
    "pollDeferredCleanup();",
    "incomingCleanup_.poll()",
    "fileTransfer_.pollCleanup()",
    "linkFileResultFetched",
)
for token in required_service:
    assert token in service or token in header, token
for token in ("cleanupPending_", "cleanup_.poll()", "cleanupSuccess_"):
    assert token in file_transfer or token in file_header, token

assert "if (!lease && file_) file_.close()" not in file_transfer
assert "if (incomingFile_) incomingFile_.close();" not in service
assert "if (!lease) return false;" in helper
assert helper.index("if (!lease) return false;") < helper.index("file_->close()")
assert helper.index("file_->close()") < helper.index("reservation_->release()")
print("PASS link_deferred_cleanup_contract")
