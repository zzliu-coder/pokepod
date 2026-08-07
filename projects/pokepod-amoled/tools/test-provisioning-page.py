#!/usr/bin/env python3
"""Static contract for the captive portal's mobile keyboard behavior."""

from pathlib import Path


source = (Path(__file__).parents[1] / "firmware" / "PokePodAmoled" /
          "ProvisioningPortal.cpp").read_text(encoding="utf-8")

assert "id='wifi-step'" in source
assert "id='tencent-step'" in source
assert "function blurKeyboard()" in source
assert "document.activeElement" in source
assert "document.scrollingElement" in source
assert "document.documentElement.scrollTop=0" in source
assert "document.body.scrollTop=0" in source
assert "window.scrollTo(0,0)" not in source
assert "overflow-y:auto" not in source
assert "overflow:hidden" not in source
assert "scrollIntoView" not in source
assert "behavior:'smooth'" not in source
assert "id='ssid'" in source
assert "id='rescan'" in source
assert "id='wifi-password'" in source
assert "--viewport-height:100dvh" in source
assert "window.visualViewport" in source
assert "position:sticky" in source
assert "class='actions two'" in source
assert "id='credential-editor'" in source
assert "更换腾讯云密钥" in source
assert "高级设置" in source
assert source.index("id='wifi-step'") < source.index("id='tencent-step'")
print("PASS test_provisioning_page")
