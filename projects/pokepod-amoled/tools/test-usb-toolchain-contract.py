#!/usr/bin/env python3
"""Verify the locked Arduino-ESP32/TinyUSB behavior used by PokePod USB."""

from pathlib import Path
import re
import sys


project = Path(__file__).parents[1]
sys.path.insert(0, str(project / "tools"))
from pokepod_build_env import (  # noqa: E402
    BuildEnvironmentError,
    resolve_build_environment,
)

try:
    build_environment = resolve_build_environment()
except BuildEnvironmentError as exc:
    raise SystemExit(f"toolchain gate unavailable: {exc}") from exc
sdk_usb = Path(build_environment.esp32_platform_dir) / "cores/esp32/USB.cpp"
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

sdk_cdc_device = (
    Path(build_environment.esp32_s3_sdk_dir)
    / "include/arduino_tinyusb/tinyusb/src/class/cdc/cdc_device.h"
)
assert sdk_cdc_device.is_file(), f"TinyUSB CDC source missing: {sdk_cdc_device}"
cdc_device = sdk_cdc_device.read_text()
assert "void tud_cdc_n_read_flush(uint8_t itf);" in cdc_device
assert "bool tud_cdc_n_write_clear(uint8_t itf);" in cdc_device

print(
    "PASS test_usb_toolchain_contract "
    f"(Arduino-ESP32 {build_environment.esp32_core_version})"
)
