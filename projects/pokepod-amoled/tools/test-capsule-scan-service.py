#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
library = (root / "firmware/PokePodAmoled/CapsuleLibrary.cpp").read_text()
header = (root / "firmware/PokePodAmoled/CapsuleLibrary.h").read_text()
coordinator = (root / "firmware/PokePodAmoled/StorageCoordinator.h").read_text()
host_test = (root / "firmware/tests/test_capsule_scan_stepper.cpp").read_text()

required = {
    "public start": "bool startScan(",
    "public step": "CapsuleScanState stepScan()",
    "public cancel": "void cancelScan()",
    "public hydrate": "bool hydrate(",
    "dedicated owner": "capsuleScan",
    "read reservation": "startScanWithOwner(budget, StorageOwner::capsuleScan)",
    "short lease": "StorageIoLease scanIo",
    "nonblocking lease": "scanOwner_, StorageAccess::read, 0",
    "nonblocking reservation": "owner == StorageOwner::capsuleScan ? 0 : 1000",
    "double buffer": "std::swap(locators_, scanLocators_)",
    "shared codec": "CapsuleMetadataCodec::decode",
    "step timing": "maximumScanStepUs()",
    "lease timing": "maximumScanLeaseUs()",
}
combined = header + library + coordinator
missing = [name for name, needle in required.items() if needle not in combined]
if missing:
    raise SystemExit("FAIL capsule scan service: " + ", ".join(missing))

old_scan_body = library.split("bool CapsuleLibrary::scan()", 1)[1].split(
    "bool CapsuleLibrary::startScan", 1
)[0]
if "acquireIo" in old_scan_body or "scanFolder" in old_scan_body:
    raise SystemExit("FAIL scan() still owns a whole-tree physical IO lease")

for forbidden in ("AtomicFixtureScanner", "struct FixtureScanner"):
    if forbidden in host_test:
        raise SystemExit("FAIL host test duplicates the production scanner")
for required_test_path in (
    "CapsuleLibrary library", "library.startScan", "library.stepScan",
    "library.hydrate", "seedFixture(0, 360", "cycle < 10000",
):
    if required_test_path not in host_test:
        raise SystemExit("FAIL host test misses production path: " + required_test_path)

print("PASS production capsule scan service contract")
