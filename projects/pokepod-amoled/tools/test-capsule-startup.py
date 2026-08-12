#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
header = (root / "firmware/PokePodAmoled/CapsuleLibrary.h").read_text()
source = (root / "firmware/PokePodAmoled/CapsuleLibrary.cpp").read_text()
app = (root / "firmware/PokePodAmoled/PokePodApp.cpp").read_text()
test = (root / "firmware/tests/test_capsule_scan_stepper.cpp").read_text()

required = {
    "startup state": "enum class CapsuleLibraryStartupState",
    "recovery": "recoveringTransactions",
    "bounded scan": "startScanWithOwner({1, 4096}, StorageOwner::recovery)",
    "hidden index": "startupReady() ? locatorCount_ : 0",
    "single primitive assertion": "primitives) + enumerations <= 1",
    "full capacity fixture": "seedFixture(0, kCapsuleLocatorCapacity",
    "interrupted jobs": "const size_t interrupted[]",
    "unknown schema immutable": "state->text(records[7].directory",
    "quarantine": "bad-one.journal.blocked",
    "app poll": "capsuleLibrary.pollStartup(nowMs)",
}
combined = header + source + app + test
missing = [name for name, needle in required.items() if needle not in combined]
if missing:
    raise SystemExit("FAIL capsule startup: " + ", ".join(missing))

begin_body = source.split("bool CapsuleLibrary::begin", 1)[1].split(
    "CapsuleLibraryStartupState CapsuleLibrary::pollStartup", 1
)[0]
for forbidden in ("recoverAll()", "while (", "scan()"):
    if forbidden in begin_body:
        raise SystemExit("FAIL capsule startup begin is synchronous: " + forbidden)

scan_body = source.split("bool CapsuleLibrary::scan()", 1)[1].split(
    "void CapsuleLibrary::failStartup", 1
)[0]
if "while (" in scan_body or "stepScan()" in scan_body:
    raise SystemExit("FAIL CapsuleLibrary::scan drains work synchronously")

if "startupAuthorityReservation_.release();" not in source:
    raise SystemExit("FAIL startup authority has no terminal release")
if "startupMaximumIoBytes_" not in source or "<= 4096" not in test:
    raise SystemExit("FAIL startup 4 KiB IO ceiling is not exercised")

print("PASS cooperative capsule startup contract")
