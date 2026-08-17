#!/usr/bin/env python3
from pathlib import Path

root = Path(__file__).resolve().parents[1]
library = (root / "firmware/PokePodAmoled/CapsuleLibrary.cpp").read_text()
header = (root / "firmware/PokePodAmoled/CapsuleLibrary.h").read_text()
coordinator = (root / "firmware/PokePodAmoled/StorageCoordinator.h").read_text()
host_test = (root / "firmware/tests/test_capsule_scan_stepper.cpp").read_text()
app = (root / "firmware/PokePodAmoled/PokePodApp.cpp").read_text()
link_service = (root / "firmware/PokePodAmoled/PokePodLinkService.cpp").read_text()
link_commands = (root / "firmware/PokePodAmoled/LinkCapsuleCommands.cpp").read_text()
link = link_service + link_commands

required = {
    "public start": "bool startScan(",
    "public step": "CapsuleScanState stepScan()",
    "sticky request": "bool requestScan(",
    "loop poll": "CapsuleScanState pollScan()",
    "public cancel": "void cancelScan()",
    "public hydrate": "bool hydrate(",
    "dedicated owner": "capsuleScan",
    "read reservation": "startScanWithOwner(budget, StorageOwner::capsuleScan)",
    "short lease": "StorageIoLease scanIo",
    "nonblocking lease": "scanOwner_, StorageAccess::read, 0",
    "nonblocking reservation": "owner, StorageAccess::read, 0",
    "cooperative startup": "CapsuleLibraryStartupState pollStartup",
    "startup authority": "startupAuthorityReservation_",
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
    "library.requestScan", "library.pollScan", "library.scanRequested",
    "StorageOwner::capsuleTransaction, StorageAccess::mutation",
    "runProductionCooperativeStartup", "kCapsuleLocatorCapacity",
    "primitives) + enumerations <= 1",
):
    if required_test_path not in host_test:
        raise SystemExit("FAIL host test misses production path: " + required_test_path)

if app.count("capsuleLibrary->pollScan()") != 1:
    raise SystemExit("FAIL app must poll the capsule scan exactly once per loop")
if "capsuleLibrary->scan()" in app:
    raise SystemExit("FAIL app runtime still invokes synchronous capsule scan")
if "capsuleLibrary->requestScan()" not in app:
    raise SystemExit("FAIL recorder fallback does not queue an async refresh")
if "library_->scan()" in link:
    raise SystemExit("FAIL Link runtime still invokes synchronous capsule scan")
if link.count("library_->requestScan()") < 3:
    raise SystemExit("FAIL Link stop/rescan/command completion refresh is incomplete")
if 'strcmp(batchJournalState_.operation, "rescan") == 0' not in link or \
        '(queued ? "queued" : "committed")' not in link:
    raise SystemExit("FAIL Link rescan response claims synchronous completion")

if "capsuleLibrary->pollStartup(nowMs)" not in app:
    raise SystemExit("FAIL app does not cooperatively advance library startup")
if "while (scanStepper_.active())" in library:
    raise SystemExit("FAIL startup/runtime scan still contains a synchronous drain loop")
if "if (!startupReady()) return nullptr;" not in library:
    raise SystemExit("FAIL consumers can observe an unpublished startup index")

print("PASS production capsule scan service contract")
