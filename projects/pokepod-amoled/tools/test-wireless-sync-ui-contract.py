#!/usr/bin/env python3
"""Keep the one-tap UI tied to the existing truthful sync lifecycle."""

from pathlib import Path

root = Path(__file__).parents[1]
source = root / "firmware" / "PokePodAmoled"
link_header = (source / "PokePodLinkService.h").read_text()
link_source = (source / "PokePodLinkService.cpp").read_text()
sync_source = (source / "WirelessSyncService.cpp").read_text()
app_source = (source / "PokePodApp.cpp").read_text()
ui_policy = (source / "UiPolicy.h").read_text()

assert "maintenanceCompletionRevision() const" in link_header
assert 'strcmp(operation, "endMaintenance") == 0' in link_source
assert "completedMaintenance = true;" in link_source
assert "maintenanceCompletion_.endResultPersisted" in link_source
assert "maintenanceCompletion_.resultFetched(transactionId, fullySent)" in link_source
assert "maintenanceCompletion_.beginAccepted();" in link_source
assert "beginResultPersisted" not in link_source
begin_assignment = link_source.index("activeMaintenance_ = maintenanceId;")
begin_revision = link_source.index(
    "maintenanceCompletion_.beginAccepted();", begin_assignment
)
begin_success = link_source.index("success = true;", begin_assignment)
assert begin_assignment < begin_revision < begin_success
assert "persisted && beganMaintenance" not in link_source
assert "maintenanceCompletion_.disconnect();" in link_source
assert "++maintenanceCompletionRevision_" not in link_source
assert "link_.maintenanceCompletionRevision()" in sync_source
assert "link_.maintenanceStartRevision()" in sync_source
assert "link_.maintenanceCompletedStartRevision() == startRevision" in sync_source
assert "lastCompletedAtMs_ = nowMs == 0 ? 1 : nowMs;" in sync_source
assert "lastCompletedAtMs_ = 0;" in sync_source
assert 'lastError_ == "listener-start-failed"' in sync_source
assert 'lastError_ == "bonjour-start-failed"' in sync_source

assert "computerSyncEntryDecision(wirelessSync.openWindow())" in app_source
assert "wirelessSync.open(now);" in app_source
assert "dashboard.openComputerSync();" in app_source
assert "action == UiAction::closeComputerSync" in app_source
assert "toggleComputerSync" not in app_source
assert "toggleComputerSync" not in ui_policy

print("PASS test_wireless_sync_ui_contract")
