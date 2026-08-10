#!/usr/bin/env python3
"""Keep the one-tap UI tied to the existing truthful sync lifecycle."""

from pathlib import Path

root = Path(__file__).parents[1]
source = root / "firmware" / "PokePodAmoled"
link_header = (source / "PokePodLinkService.h").read_text()
link_source = (source / "PokePodLinkService.cpp").read_text()
sync_source = (source / "WirelessSyncService.cpp").read_text()
app_source = (source / "PokePodAmoled.ino").read_text()
ui_policy = (source / "UiPolicy.h").read_text()

assert "maintenanceCompletionRevision() const" in link_header
assert 'strcmp(operation, "endMaintenance") == 0' in link_source
assert "completedMaintenance = true;" in link_source
assert "if (persisted && completedMaintenance)" in link_source
assert "link_.maintenanceCompletionRevision()" in sync_source
assert "lastCompletedAtMs_ = nowMs == 0 ? 1 : nowMs;" in sync_source
assert 'lastError_ == "listener-start-failed"' in sync_source
assert 'lastError_ == "bonjour-start-failed"' in sync_source

assert "computerSyncEntryDecision(wirelessSync.openWindow())" in app_source
assert "wirelessSync.open(now);" in app_source
assert "dashboard.openComputerSync();" in app_source
assert "action == UiAction::closeComputerSync" in app_source
assert "toggleComputerSync" not in app_source
assert "toggleComputerSync" not in ui_policy

print("PASS test_wireless_sync_ui_contract")
