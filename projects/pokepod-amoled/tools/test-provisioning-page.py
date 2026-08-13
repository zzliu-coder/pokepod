#!/usr/bin/env python3
"""Static contract for the captive portal's mobile keyboard behavior."""

from pathlib import Path
import re


firmware_dir = Path(__file__).parents[1] / "firmware" / "PokePodAmoled"
source = (firmware_dir / "ProvisioningPortal.cpp").read_text(encoding="utf-8")
coordinator = (firmware_dir / "ProvisioningCoordinator.cpp").read_text(encoding="utf-8")
wifi_source = (firmware_dir / "WifiController.cpp").read_text(encoding="utf-8")
link_source = (firmware_dir / "PokePodLinkService.cpp").read_text(encoding="utf-8")
link_dispatcher = (firmware_dir / "LinkCommandDispatcher.cpp").read_text(encoding="utf-8")
main_source = (firmware_dir / "PokePodApp.cpp").read_text(encoding="utf-8")
dashboard_source = (firmware_dir / "Dashboard.cpp").read_text(encoding="utf-8")
dashboard_header = (firmware_dir / "Dashboard.h").read_text(encoding="utf-8")
portal_header = (firmware_dir / "ProvisioningPortal.h").read_text(encoding="utf-8")
coordinator_header = (firmware_dir / "ProvisioningCoordinator.h").read_text(encoding="utf-8")
policy_source = (firmware_dir / "ProvisioningPolicy.h").read_text(encoding="utf-8")
all_firmware_source = "\n".join(
    path.read_text(encoding="utf-8")
    for path in sorted(firmware_dir.iterdir())
    if path.suffix in {".cpp", ".h", ".ino"}
)

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
assert "b.addEventListener('click',()=>load(true));" in source
assert "\nload(true);\n</script>" in source
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
assert "'X-PokePod-CSRF':csrf" in source

# Every provisioning session owns a fresh short-lived AP credential. The
# browser never receives a mutation endpoint that could approve a sensitive
# cloud-key change without a physical press on the device.
assert 'constexpr size_t kProvisioningPasswordLength = 10;' in policy_source
assert '"23456789ABCDEFGHJKLMNPQRSTUVWXYZ"' in policy_source
assert "ProvisioningCredentialPolicy" in policy_source
assert "ProvisioningSensitiveConfirmationPolicy" in policy_source
assert "esp_fill_random(destination, length);" in source
assert "credential_.begin(fillProvisioningRandom, nullptr)" in source
assert "password_ = credential_.password();" in source
assert "credential_.close();" in source
assert 'password_ = "";' not in source
assert "88888888" not in all_firmware_source
assert 'server_.on("/confirm"' not in source
assert 'confirmationRequired' in source
assert 'confirmationAction' in source
assert 'confirmationRemainingSeconds' in source
assert "sensitiveConfirmation_.begin(sensitiveAction, millis())" in source
assert "sensitiveConfirmation_.acceptPhysicalPress(nowMs)" in source
assert "monotonicElapsedAtLeast(nowMs, startedMs_, kPortalLifetimeMs)" in source
assert "sensitiveConfirmationPending()" in portal_header
assert "confirmSensitiveChange(uint32_t nowMs)" in portal_header
assert "sensitiveConfirmationPending()" in coordinator_header
assert "confirmSensitiveChange(uint32_t nowMs)" in coordinator_header
assert "bootProvisioningConfirmationConsumed" in main_source
assert "provisioningCoordinator.confirmSensitiveChange(now)" in main_source
assert "if (bootProvisioningConfirmationConsumed)" in main_source
for log_call in re.findall(r"(?:log_|log\.)(?:printf|print|println)\([^;]*;", source,
                           flags=re.DOTALL):
    assert "password_" not in log_call

assert 'server_.on("/forget-network", HTTP_POST' in source
assert "config_->wifiNetwork(next.wifiSsid)" in source
assert r'\"remembered\"' in source
assert "function pollValidation()" in source
assert "function showSuccess()" in source
assert r'\"validating\":' in source
assert r'\"saved\":' in source
assert "ProvisioningState ProvisioningPortal::state() const" in source
assert "statusMessage_ = \"正在连接网络\";" in source
assert "statusMessage_ = \"正在连接 \" + next.wifiSsid;" not in source
assert "statusMessage_ = \"Wi-Fi 已连接\";" in source
assert "view.portalStatus = provisioningPortal.statusMessage();" in main_source
assert "view.portalState = provisioningPortal.state();" in main_source
assert "view.portalPassword = provisioningPortal.password();" in main_source
assert "renderer_.drawText(view.portalPassword" in dashboard_source
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
assert "renderer_.drawText(String(record->ssid)" in dashboard_source
assert "UiTextSize::body" in dashboard_source
signature = dashboard_source[
    dashboard_source.index("uint64_t Dashboard::signature"):
    dashboard_source.index("uint64_t Dashboard::topBarSignature")
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
sensitive_branch = save_handler[save_handler.index(
    "if (sensitiveAction != ProvisioningSensitiveAction::none)"):
    save_handler.index("armStationValidation(millis())")]
assert "sendSaveJson(202, true);" in sensitive_branch
assert "return;" in sensitive_branch
confirm_handler = source[source.index(
    "bool ProvisioningPortal::confirmSensitiveChange"):
    source.index("void ProvisioningPortal::discardSensitiveCandidate")]
assert confirm_handler.index("acceptPhysicalPress") < confirm_handler.index(
    "armStationValidation")
assert "stop();" in confirm_handler
assert "void ProvisioningPortal::beginStationValidation()" in source
assert "void ProvisioningPortal::restorePortalForRetry()" in source
assert "restorePortalForRetry();" in source
assert "WiFi.mode(WIFI_STA);" in source[source.index(
    "void ProvisioningPortal::beginStationValidation()"):
    source.index("void ProvisioningPortal::restorePortalForRetry()")]
prepare_handler = source[source.index("bool ProvisioningPortal::prepare"):
                         source.index("bool ProvisioningPortal::switchToAccessPointMode")]
start_handler = source[source.index("bool ProvisioningPortal::switchToAccessPointMode"):
                       source.index("void ProvisioningPortal::failStartupTimeout")]
assert "statusMessage_ = \"正在准备配网热点\";" in prepare_handler
assert "WiFi.mode" not in prepare_handler
assert "WiFi.softAP" not in prepare_handler
assert "startScan();" not in prepare_handler
assert "WiFi.mode(WIFI_AP)" in start_handler
assert "WiFi.softAP" in start_handler
assert start_handler.index("WiFi.mode(WIFI_AP)") < start_handler.index("WiFi.softAP")
assert "bool ProvisioningPortal::startAccessPoint()" in start_handler
assert "bool ProvisioningPortal::startServices()" in start_handler
assert "ProvisioningLogStage::radioModeStarted" in start_handler
assert "ProvisioningLogStage::accessPointStarted" in start_handler
assert "logProvisioningMemory" in start_handler
assert "statusMessage_ = \"请选择附近的 2.4 GHz 网络或手工输入\";" in start_handler
state_handler = source[source.index("ProvisioningState ProvisioningPortal::state() const"):
                       source.index("const char *ProvisioningPortal::portalState")]
assert 'statusMessage_.indexOf("仍在")' in state_handler
stop_handler = source[source.index("void ProvisioningPortal::stop()"):
                      source.index("bool ProvisioningPortal::takeConfigurationChanged")]
assert "const bool wasPrepared = prepared_;" in stop_handler
assert "wasActive || wasPrepared" in stop_handler
assert "clearProvisioningCredential();" in stop_handler

# Arduino-ESP32 String::clear() only resets its logical length. Provisioning
# credentials must be overwritten through the writable buffer before clear,
# including startup failures, normal stop, object destruction, and the start of
# a later session.
clear_helper = source[source.index("void secureClearString"):
                      source.index("String validationFailureMessage")]
assert "const size_t length = secret.length();" in clear_helper
assert "volatile char *const wipe = secret.begin();" in clear_helper
assert "wipe[index] = '\\0';" in clear_helper
assert clear_helper.index("wipe[index] = '\\0';") < clear_helper.index(
    "secret.clear();")
clear_credential = source[source.index(
    "void ProvisioningPortal::clearProvisioningCredential"):
    source.index("bool ProvisioningPortal::takeConfigurationChanged")]
assert clear_credential.index("secureClearString(password_);") < (
    clear_credential.index("credential_.close();"))
destructor = source[source.index("ProvisioningPortal::~ProvisioningPortal"):
                    source.index("bool ProvisioningPortal::prepare")]
assert "clearProvisioningCredential();" in destructor
assert "~ProvisioningPortal();" in portal_header

# A generation failure can leave partially generated bytes in the policy
# buffer. The common cleanup must run on that branch. A later session also
# scrubs any stale portal copy before generating and publishing a new value.
credential_begin = "credential_.begin(fillProvisioningRandom, nullptr)"
begin_failure = prepare_handler[prepare_handler.index(credential_begin):
                                prepare_handler.index(
                                    "password_ = credential_.password();")]
assert "clearProvisioningCredential();" in begin_failure
assert prepare_handler.index("clearProvisioningCredential();") < (
    prepare_handler.index(credential_begin))

switch_handler = source[source.index(
    "bool ProvisioningPortal::switchToAccessPointMode"):
    source.index("bool ProvisioningPortal::startAccessPoint")]
access_point_handler = source[source.index(
    "bool ProvisioningPortal::startAccessPoint"):
    source.index("bool ProvisioningPortal::startServices")]
services_handler = source[source.index(
    "bool ProvisioningPortal::startServices"):
    source.index("void ProvisioningPortal::failStartupTimeout")]
timeout_handler = source[source.index(
    "void ProvisioningPortal::failStartupTimeout"):
    source.index("void ProvisioningPortal::loop")]
assert switch_handler.count("clearProvisioningCredential();") == 3
assert access_point_handler.count("clearProvisioningCredential();") == 2
assert services_handler.count("clearProvisioningCredential();") == 1
assert timeout_handler.count("clearProvisioningCredential();") == 2
request_handler = coordinator[coordinator.index("bool ProvisioningCoordinator::request"):
                              coordinator.index("void ProvisioningCoordinator::poll")]
assert "quiesceForProvisioning" not in request_handler
assert "WiFi." not in request_handler
poll_handler = coordinator[coordinator.index("void ProvisioningCoordinator::poll"):
                           coordinator.index("void ProvisioningCoordinator::stop")]
assert "ProvisioningStartupAction::quiesceRadio" in poll_handler
assert "quiesceForProvisioning" in poll_handler
assert poll_handler.index("quiesceForProvisioning") < poll_handler.index(
    "switchToAccessPointMode"
)
assert poll_handler.index("switchToAccessPointMode") < poll_handler.index(
    "startAccessPoint"
)
assert poll_handler.index("startAccessPoint") < poll_handler.index(
    "startServices"
)
assert "readyForProvisioning" not in poll_handler
quiesce_handler = wifi_source[wifi_source.index("void WifiController::quiesceForProvisioning"):
                              wifi_source.index("void WifiController::startConnection")]
assert "esp_wifi_scan_stop" in quiesce_handler
assert "esp_wifi_disconnect" in quiesce_handler
assert "WiFi.mode(WIFI_OFF)" not in quiesce_handler
assert "WiFi.status()" not in start_handler
portal_loop = "provisioningCoordinator.poll(now);"
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
assert "portal_->loop(millis())" in coordinator
assert "nowMs = millis();" in source
assert "if (validating_ && !transitionPending_)" in source
assert "validatingSinceMs_ = millis();" in source[source.index(
    "void ProvisioningPortal::beginStationValidation()"):source.index(
    "void ProvisioningPortal::restorePortalForRetry()")]
assert "monotonicElapsedAtLeast(nowMs, validatingSinceMs_" in source
assert "monotonicElapsedOrZero(nowMs, validatingSinceMs_)" in source
assert "monotonicElapsedAtLeast(nowMs, startedMs_, kPortalLifetimeMs)" in source
assert 'strcmp(operation, "provisioning-start") == 0' in link_dispatcher
assert 'strcmp(operation, "provisioning-stop") == 0' in link_dispatcher
assert "only available over USB" in link_dispatcher
assert source.index("id='wifi-step'") < source.index("id='tencent-step'")
for handler_name, next_declaration in (
    ("scanRequest", "void ProvisioningPortal::showPortal()"),
    ("saveRequest", "void ProvisioningPortal::beginStationValidation()"),
    ("forgetRequest", "bool ProvisioningPortal::authorizeMutation()"),
):
    handler_start = source.index(f"void ProvisioningPortal::{handler_name}()")
    handler = source[handler_start:source.index(next_declaration, handler_start)]
    assert "if (!authorizeMutation()) return;" in handler
assert "server_.collectHeaders(headers, 1);" in source
change_gate = source[source.index("bool ProvisioningPortal::takeConfigurationChanged()"):
                     source.index("void ProvisioningPortal::installRoutes()")]
assert "if (active_) return false;" in change_gate
print("PASS test_provisioning_page")
