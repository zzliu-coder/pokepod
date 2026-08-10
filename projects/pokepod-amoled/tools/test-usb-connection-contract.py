#!/usr/bin/env python3
"""Pin USB mounted semantics and the distinct BLE/USB status icon mapping."""

from pathlib import Path
import re


project = Path(__file__).parents[1]
bridge = (project / "firmware/PokePodAmoled/UsbLinkBridge.cpp").read_text()
bridge_header = (project / "firmware/PokePodAmoled/UsbLinkBridge.h").read_text()
app = (project / "firmware/PokePodAmoled/PokePodAmoled.ino").read_text()
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
    r"if \(\(lastUsbHostConnected && !usbHostConnected\) \|\| "
    r"usbHostSessionClosed\) \{\s*"
    r"usb\.discardHostSessionBuffers\(\);\s*"
    r"linkService\.disconnect\(\);",
    app,
)
assert close_guard is not None
assert "while (cdc_.available() > 0)" in bridge
assert "tud_cdc_read_flush();" in bridge
assert "tud_cdc_write_clear();" in bridge

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

sdk_cdc_header = sdk_usb.with_name("USBCDC.h")
sdk_cdc_source = sdk_usb.with_name("USBCDC.cpp")
assert sdk_cdc_header.is_file() and sdk_cdc_source.is_file()
cdc_header = sdk_cdc_header.read_text()
cdc_source = sdk_cdc_source.read_text()
assert "ARDUINO_USB_CDC_LINE_STATE_EVENT" in cdc_header
assert "bool dtr;" in cdc_header
assert "bool rts;" in cdc_header
assert "l.line_state.dtr = dtr;" in cdc_source
assert "ARDUINO_USB_CDC_LINE_STATE_EVENT" in cdc_source

sdk_cdc_device = Path(
    "/Users/zheliu/Library/Arduino15/packages/esp32/tools/esp32s3-libs/3.3.8/"
    "include/arduino_tinyusb/tinyusb/src/class/cdc/cdc_device.h"
)
assert sdk_cdc_device.is_file(), f"TinyUSB CDC source missing: {sdk_cdc_device}"
cdc_device = sdk_cdc_device.read_text()
assert "void tud_cdc_n_read_flush(uint8_t itf);" in cdc_device
assert "bool tud_cdc_n_write_clear(uint8_t itf);" in cdc_device

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
