#!/usr/bin/env python3
"""Pin USB mounted semantics and the distinct BLE/USB status icon mapping."""

from pathlib import Path
import re


project = Path(__file__).parents[1]
bridge = (project / "firmware/PokePodAmoled/UsbLinkBridge.cpp").read_text()
assert "return started_ && static_cast<bool>(USB);" in bridge

sdk_usb = Path(
    "/Users/zheliu/Library/Arduino15/packages/esp32/hardware/esp32/3.3.8/cores/esp32/USB.cpp"
)
assert sdk_usb.is_file(), f"Arduino-ESP32 USB source missing: {sdk_usb}"
sdk_source = sdk_usb.read_text()
operator = re.search(
    r"ESPUSB::operator bool\(\) const\s*\{(.*?)\}", sdk_source, re.DOTALL
)
assert operator is not None
assert "_started && tinyusb_device_mounted" in operator.group(1)

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
assert 'UiIcon::bluetooth, "蓝牙配对"' in dashboard
assert "void Dashboard::drawBluetoothPairing" in dashboard

print("PASS test_usb_connection_contract (mounted USB, distinct BLE/USB icons)")
