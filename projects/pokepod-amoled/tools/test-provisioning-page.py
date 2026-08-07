#!/usr/bin/env python3
"""Static contract for the captive portal's mobile keyboard behavior."""

from pathlib import Path


source = (Path(__file__).parents[1] / "firmware" / "PokePodAmoled" /
          "ProvisioningPortal.cpp").read_text(encoding="utf-8")

assert "id='wifi-step'" in source
assert "id='tencent-step'" in source
assert "function blurKeyboard()" in source
assert "document.activeElement" in source
assert "window.scrollTo(0,0)" in source
assert "overflow-y:auto" in source
assert "scrollIntoView" not in source
assert "behavior:'smooth'" not in source
assert "id='ssid'" in source
assert "id='rescan'" in source
assert "id='wifi-password'" in source
assert "touch-action:pan-y" in source
assert "-webkit-overflow-scrolling:touch" in source
assert "class='actions two'" in source
assert source.index("id='wifi-step'") < source.index("id='tencent-step'")
print("PASS test_provisioning_page")
