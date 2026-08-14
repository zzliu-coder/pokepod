#!/usr/bin/env python3
"""Pin firmware-side USB session semantics and BLE/USB status mapping."""

from pathlib import Path
import re


project = Path(__file__).parents[1]
bridge = (project / "firmware/PokePodAmoled/UsbLinkBridge.cpp").read_text()
bridge_header = (project / "firmware/PokePodAmoled/UsbLinkBridge.h").read_text()
app = (project / "firmware/PokePodAmoled/PokePodApp.cpp").read_text()
assert "return started_ && static_cast<bool>(USB);" in bridge
assert "ARDUINO_USB_CDC_LINE_STATE_EVENT" in bridge
assert "data->line_state.dtr" in bridge
assert "ARDUINO_USB_CDC_DISCONNECTED_EVENT" in bridge
assert "bool hostSessionActive() const" in bridge_header
assert "bool takeHostSessionClosed()" in bridge_header
assert "void discardHostSessionBuffers()" in bridge_header
assert "const bool usbHostSessionClosed = usb.takeHostSessionClosed();" in app
assert "(lastUsbHostConnected && !usbHostConnected) || usbHostSessionClosed" in app
close_guard = re.search(
    r"if \(bootUsbLinkStarted &&\s*"
    r"\(\(lastUsbHostConnected && !usbHostConnected\) \|\| "
    r"usbHostSessionClosed\)\) \{\s*"
    r"usb\.discardHostSessionBuffers\(\);\s*"
    r"linkService\.disconnect\(\);",
    app,
)
assert close_guard is not None
assert "while (cdc_.available() > 0)" in bridge
assert "tud_cdc_read_flush();" in bridge
assert "tud_cdc_write_clear();" in bridge
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
