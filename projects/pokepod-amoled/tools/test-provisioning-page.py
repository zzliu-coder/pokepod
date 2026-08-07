#!/usr/bin/env python3
"""Static contract for the captive portal's mobile keyboard behavior."""

from pathlib import Path


firmware_dir = Path(__file__).parents[1] / "firmware" / "PokePodAmoled"
source = (firmware_dir / "ProvisioningPortal.cpp").read_text(encoding="utf-8")
main_source = (firmware_dir / "PokePodAmoled.ino").read_text(encoding="utf-8")

assert "id='wifi-step'" in source
assert "id='tencent-step'" in source
assert "id='success-step'" in source
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
assert "id='save'" in source
assert "id='credential-editor'" in source
assert "更换腾讯云密钥" in source
assert "高级设置" in source
assert "form.addEventListener('submit',submitForm)" in source
assert "fetch('/save'" in source
assert "function pollValidation()" in source
assert "function showSuccess()" in source
assert r'\"validating\":' in source
assert r'\"saved\":' in source
save_handler = source[source.index("void ProvisioningPortal::saveRequest()"):
                      source.index("void ProvisioningPortal::redirectPortal()")]
assert "showPortal();" not in save_handler
assert "sendSaveJson(202, true);" in save_handler
portal_loop = "if (provisioningPortal.active()) provisioningPortal.loop(now);"
assert portal_loop in main_source
assert main_source.index(portal_loop) < main_source.index(
    "if (!microphoneStreaming) {", main_source.index(portal_loop))
assert source.index("id='wifi-step'") < source.index("id='tencent-step'")
print("PASS test_provisioning_page")
