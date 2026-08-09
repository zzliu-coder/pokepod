#!/usr/bin/env python3
"""Static contract for the captive portal's mobile keyboard behavior."""

from pathlib import Path


firmware_dir = Path(__file__).parents[1] / "firmware" / "PokePodAmoled"
source = (firmware_dir / "ProvisioningPortal.cpp").read_text(encoding="utf-8")
main_source = (firmware_dir / "PokePodAmoled.ino").read_text(encoding="utf-8")
dashboard_source = (firmware_dir / "Dashboard.cpp").read_text(encoding="utf-8")
dashboard_header = (firmware_dir / "Dashboard.h").read_text(encoding="utf-8")

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
assert "id='remembered-wrap'" in source
assert "id='remembered-list'" in source
assert "id='connection'" in source
assert "设备网络状态" in source
assert "wifiState" in source
assert "已保存网络可留空" in source
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
assert "fetch('/forget-network'" in source
assert 'server_.on("/forget-network", HTTP_POST' in source
assert "config_->wifiNetwork(next.wifiSsid)" in source
assert r'\"remembered\"' in source
assert "function pollValidation()" in source
assert "function showSuccess()" in source
assert r'\"validating\":' in source
assert r'\"saved\":' in source
assert "ProvisioningState ProvisioningPortal::state() const" in source
assert "statusMessage_ = \"正在连接 \" + next.wifiSsid;" in source
assert "statusMessage_ = \"已连接 \" + candidate_.wifiSsid;" in source
assert "view.portalStatus = provisioningPortal.statusMessage();" in main_source
assert "view.portalState = provisioningPortal.state();" in main_source
assert "String portalStatus;" in dashboard_header
assert "ProvisioningState portalState" in dashboard_header
assert "const ProvisioningDiagnostics *provisioningDiagnostics" in dashboard_header
provisioning_draw = dashboard_source[
    dashboard_source.index("void Dashboard::drawProvisioning"):
    dashboard_source.index("void Dashboard::drawCapsuleOrb")
]
assert "view.portalStatus" in provisioning_draw
assert "provisioningColor(view.portalState)" in provisioning_draw
assert "诊断记录" in provisioning_draw
assert "void Dashboard::drawProvisioningLog" in dashboard_source
assert "provisioningLogStageLabel" in dashboard_source
signature = dashboard_source[
    dashboard_source.index("String Dashboard::signature"):
    dashboard_source.index("String Dashboard::topBarSignature")
]
assert "view.portalStatus" in signature
assert "view.portalState" in signature
save_handler = source[source.index("void ProvisioningPortal::saveRequest()"):
                      source.index("void ProvisioningPortal::beginStationValidation()")]
assert "showPortal();" not in save_handler
assert "sendSaveJson(202, true);" in save_handler
assert "transitionPending_ = true;" in save_handler
assert "transitionAtMs_ = validatingSinceMs_ + 200;" in save_handler
assert "WiFi.softAPdisconnect(false)" not in save_handler
assert "void ProvisioningPortal::beginStationValidation()" in source
assert "void ProvisioningPortal::restorePortalForRetry()" in source
assert "restorePortalForRetry();" in source
assert "WiFi.mode(WIFI_STA);" in source[source.index(
    "void ProvisioningPortal::beginStationValidation()"):
    source.index("void ProvisioningPortal::restorePortalForRetry()")]
begin_handler = source[source.index("bool ProvisioningPortal::begin"):
                        source.index("void ProvisioningPortal::loop")]
assert "statusMessage_ = \"请选择附近的 2.4 GHz 网络或手工输入\";" in begin_handler
assert "startScan();" not in begin_handler
portal_loop = "if (provisioningPortal.active()) provisioningPortal.loop(now);"
assert portal_loop in main_source
assert main_source.index(portal_loop) < main_source.index(
    "wifi.loop(now", main_source.index(portal_loop))
network_section = main_source[
    main_source.index(portal_loop):main_source.index(
        "if (now - lastTouchMs", main_source.index(portal_loop)
    )
]
assert "wifi.loop(now" in network_section
assert "tencentWorker.loop(now" in network_section
assert source.index("id='wifi-step'") < source.index("id='tencent-step'")
change_gate = source[source.index("bool ProvisioningPortal::takeConfigurationChanged()"):
                     source.index("void ProvisioningPortal::installRoutes()")]
assert "if (active_) return false;" in change_gate
print("PASS test_provisioning_page")
