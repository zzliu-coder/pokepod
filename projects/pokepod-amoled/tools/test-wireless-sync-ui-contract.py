#!/usr/bin/env python3
"""Keep the one-tap UI tied to the existing truthful sync lifecycle."""

from pathlib import Path

root = Path(__file__).parents[1]
source = root / "firmware" / "PokePodAmoled"
link_header = (source / "PokePodLinkService.h").read_text()
link_source = (source / "PokePodLinkService.cpp").read_text()
link_transport = (source / "LinkTransportSession.cpp").read_text()
link_implementation = link_source + link_transport
command_source = (source / "LinkCapsuleCommands.cpp").read_text()
sync_source = (source / "WirelessSyncService.cpp").read_text()
app_source = (source / "PokePodApp.cpp").read_text()
ui_policy = (source / "UiPolicy.h").read_text()
dashboard_source = (source / "Dashboard.cpp").read_text()
file_transfer = (source / "LinkFileTransfer.cpp").read_text()

assert "maintenanceCompletionRevision() const" in link_header
assert 'strcmp(operation, "endMaintenance") == 0' in command_source
assert "maintenanceCompletion_.endResultPersisted" in command_source
assert "maintenanceCompletion_.resultFetched(" in link_transport
finish_function = file_transfer.index("void LinkFileTransfer::finish(bool success)")
cleanup_poll = file_transfer.index("if (!cleanup_.poll()) return false;")
result_fetched = file_transfer.index("linkFileResultFetched(")
abort_function = file_transfer.index("void LinkFileTransfer::abort()")
file_final = file_transfer.index(
    "completion == LinkFileTransferFrameCompletion::final"
)
finish_after_final = file_transfer.index("finish(true);", file_final)
assert finish_function < cleanup_poll < result_fetched < abort_function
assert file_final < finish_after_final
abort_body = file_transfer[abort_function:file_final]
assert "cleanupSuccess_ = false;" in abort_body
assert "linkFileResultFetched" not in abort_body
assert "maintenanceCompletion_.beginAccepted();" in command_source
assert "beginResultPersisted" not in command_source
batch_result = command_source.index(
    "void PokePodLinkService::applyBatchResultSideEffects()"
)
durable_side_effect = command_source.index(
    "void PokePodLinkService::applyDurableCommandSideEffects("
)
begin_assignment = command_source.index("activeMaintenance_ = targetId;",
                                     durable_side_effect)
begin_revision = command_source.index(
    "maintenanceCompletion_.beginAccepted();", begin_assignment
)
assert batch_result < durable_side_effect < begin_assignment < begin_revision
assert "if (!batchExecutor_.success()) return;" in command_source
assert "!batchExecutor_.responseAllowed()) return" not in command_source
assert "applyCompletedCommandSideEffects(root);" in command_source
assert "if (persisted) applyBatchResultSideEffects();" in command_source
assert "persisted && beganMaintenance" not in command_source
assert "maintenanceCompletion_.disconnect();" in link_transport
assert "++maintenanceCompletionRevision_" not in link_implementation
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
assert "kSyncEntry" not in ui_policy
assert "kSyncEntry" not in dashboard_source
assert "syncEntryLabel" not in dashboard_source
assert "rootScreen &&" not in ui_policy
assert "y >= ui::kDeviceStorageTop && y < ui::kDeviceRaiseTop" in ui_policy

print("PASS test_wireless_sync_ui_contract")
