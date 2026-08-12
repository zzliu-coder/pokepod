#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "firmware/PokePodAmoled/PokePodLinkService.h").read_text()
SOURCE = (ROOT / "firmware/PokePodAmoled/PokePodLinkService.cpp").read_text()
GATE = (ROOT / "firmware/PokePodAmoled/LinkTransferGate.h").read_text()
MATRIX = (ROOT / "firmware/tests/link_operation_migration.md").read_text()

assert '#include "LinkOperation.h"' in HEADER
for symbol in (
    "LinkOperation operation_", "connectionGeneration_",
    "nextConnectionGeneration_", "admitLinkOperation",
    "cancelLinkOperation", "advanceLinkOperationSettlement",
):
    assert symbol in HEADER or symbol in SOURCE, f"missing adapter fact: {symbol}"

for operation in (
    "Immediate read/configure/reboot",
    "Incoming staged file or system font",
    "Incoming command: batch/text/simple",
    "Outgoing file/read/result", "Recursive/shallow manifest",
    "Record start", "Record stop", "Maintenance begin/end",
):
    assert operation in MATRIX, f"missing migration row: {operation}"

for column in (
    "Admit point", "Progress / data / terminal frames",
    "Durable terminal fact", "Resources acquired / released",
    "Cancel and rollback", "Completed eligibility", "Maintenance retain",
):
    assert column in MATRIX, f"missing migration column: {column}"

assert "absoluteDeadline(uint32_t &deadlineMs) const" in GATE
absolute = GATE[GATE.index("bool absoluteDeadline(uint32_t &deadlineMs) const override"):
                GATE.index("bool active() const")]
assert "deadlineMs = deadlineMs_" in absolute
assert ".arm(" not in absolute

print("PASS link_operation_production_contract")
