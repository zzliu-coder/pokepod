#!/usr/bin/env python3
"""Pin firmware-side USB session semantics and BLE/USB status mapping."""

from pathlib import Path
import re


project = Path(__file__).parents[1]
cdc = (project / "cdc-status.py").read_text()
bridge = (project / "firmware/PokePodAmoled/UsbLinkBridge.cpp").read_text()
bridge_header = (project / "firmware/PokePodAmoled/UsbLinkBridge.h").read_text()
app = (project / "firmware/PokePodAmoled/PokePodApp.cpp").read_text()
link = (project / "firmware/PokePodAmoled/LinkTransportSession.cpp").read_text()
link_service = (
    project / "firmware/PokePodAmoled/PokePodLinkService.cpp"
).read_text()
board = (project / "firmware/PokePodAmoled/BoardServices.cpp").read_text()
reconcile = (
    project / "firmware/PokePodAmoled/UsbLinkSessionReconcile.h"
).read_text()
assert "return started_ && static_cast<bool>(USB);" in bridge
assert "ARDUINO_USB_CDC_LINE_STATE_EVENT" in bridge
assert "data->line_state.dtr" in bridge
assert "ARDUINO_USB_CDC_DISCONNECTED_EVENT" in bridge
assert "bool hostSessionActive() const" in bridge_header
assert "UsbCdcSessionSnapshot hostSessionSnapshot() const" in bridge_header
assert "bool takeHostSessionClosed(uint32_t &generation)" in bridge_header
assert "void discardHostSessionBuffers()" in bridge_header
assert "public LinkWriteChannel" in bridge_header
assert "LinkWriteAttempt writeSome(" in bridge_header
assert "usb.takeHostSessionClosed(closedUsbSessionGeneration)" in app
assert "closedUsbSessionGeneration == usbSession.generation" in app
assert "usbSession.generation != lastUsbSessionGeneration" in app
assert "usbLinkSessionAction(" in app
assert "linkService->usbHostSessionGeneration()" in app
assert "UsbLinkSessionAction::disconnectAndDiscard" in app
assert "UsbLinkSessionAction::disconnectRetainingNewBytes" in app
assert "linkBoundUsbGeneration == currentUsbGeneration" in reconcile
assert "usbLinkMagicRequiresEpochReset(" in reconcile
assert "usb_->hostSessionSnapshot().generation" in link
magic = link.split("void PokePodLinkService::consumeByte", 1)[1].split(
    "if (receivePhase_ == ReceivePhase::header)", 1
)[0]
assert "usbLinkMagicRequiresEpochReset(" in magic
assert magic.index("disconnect();") < magic.index("activateConnectionGeneration();")
assert "memcpy(headerBytes_, magic, sizeof(magic));" in magic
assert "usbHostSessionGeneration_ = 0;" in link
assert "while (cdc_.available() > 0)" in bridge
assert "tud_cdc_" not in bridge
assert "cdc_.setTxTimeoutMs(kUsbLinkTxTimeoutMs);" in bridge
assert "const size_t written = cdc_.write(data, wanted);" in bridge
assert "cdc_.availableForWrite()" not in bridge
assert "termios.TIOCMBIS if asserted else termios.TIOCMBIC" in cdc
assert "termios.HUPCL" in cdc
assert "set_modem_lines(fd, True)" in cdc
assert "set_modem_lines(fd, False)" in cdc
assert "time.sleep(LINK_CLOSE_SETTLE_SECONDS)" in cdc
assert "close_link_session(fd)" in cdc
assert "bootUsbLinkStarted = linkService->begin(" in app
assert "bootUsbLinkStarted = board.sdReady() && linkService->begin(" not in app
assert "storageBacked_ = board.sdReady();" in link_service
assert '"mode\\\":\\\"diagnostic_only\\\"' in link_service
diagnostic_poll = link.split(
    "bool PokePodLinkService::pollDeferredCleanup(LinkPollPhaseGate &gate)", 1
)[1].split("void PokePodLinkService::pollDeferredCleanup()", 1)[0]
assert "if (!storageBacked_)" in diagnostic_poll
assert "advanceLinkOperationSettlement();" in diagnostic_poll
assert "constexpr uint8_t kMountAttempts = 3;" in board
assert "SD_MMC.end();" in board
assert "delay(100U * attempt);" in board
usb_begin = app.split("LinkTransport::usb, &wirelessSync.get(),", 1)[1]
assert "nullptr, &provisioningCoordinator, &usb," in usb_begin[:160]
discard = app.split(
    "if (usbSessionAction == UsbLinkSessionAction::disconnectAndDiscard)", 1
)[1].split("} else if", 1)[0]
assert discard.index("linkService->disconnect();") < discard.index(
    "usb.discardHostSessionBuffers();"
)
assert '#include "UsbPhysicalConnectionPolicy.h"' in app
assert "return usbPhysicalConnected(usb.hostConnected(), status.pmu," in app
assert "view.usbConnected = usbCableConnected();" in app
assert "lastVbusPresent" in app

dashboard = (project / "firmware/PokePodAmoled/Dashboard.cpp").read_text()
top_bar = dashboard.split("void Dashboard::drawTopBar", 1)[1].split(
    "void Dashboard::drawPageIndicator", 1
)[0]
assert re.search(
    r"UiIcon::bluetooth,\s*282,\s*8,\s*"
    r"view\.bleVoiceReady\s*\?\s*ui::kWireless\s*:\s*ui::kMuted",
    top_bar,
)
assert re.search(
    r"UiIcon::mac,\s*326,\s*8,\s*"
    r"view\.usbConnected\s*\?\s*ui::kAccent\s*:\s*ui::kMuted",
    top_bar,
)
assert 'UiIcon::bluetooth, "蓝牙"' in dashboard
assert "void Dashboard::drawBluetoothPairing" in dashboard

print("PASS test_usb_connection_contract (firmware source only)")
