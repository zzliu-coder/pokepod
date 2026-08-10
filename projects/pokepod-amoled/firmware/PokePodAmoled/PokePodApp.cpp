#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include "AudioPipeline.h"
#include "AudioCaptureRouter.h"
#include "BleVoiceService.h"
#include "BoardConfig.h"
#include "BoardServices.h"
#include "ButtonDebouncer.h"
#include "CapsuleLibrary.h"
#include "CapsulePolicy.h"
#include "CapsuleUndoState.h"
#include "Dashboard.h"
#include "DeviceConfig.h"
#include "ProvisioningPortal.h"
#include "ProvisioningDiagnostics.h"
#include "ProvisioningCoordinator.h"
#include "PokePodLinkService.h"
#include "LinkServiceCoordinator.h"
#include "PowerPolicy.h"
#include "RaiseToWakePolicy.h"
#include "RuntimePowerManager.h"
#include "TencentWorker.h"
#include "TlsExternalMemory.h"
#include "UsbLinkBridge.h"
#include "UsbPhysicalConnectionPolicy.h"
#include "WavRecorder.h"
#include "WifiController.h"
#include "WifiUiPolicy.h"
#include "WirelessSyncIdentity.h"
#include "WirelessSyncService.h"

using namespace pokepod;

namespace {

BoardServices board;
AudioPipeline audio;
AudioCaptureRouter captureRouter;
UsbLinkBridge usb;
BleVoiceService bleVoice;
WavRecorder recorder;
CapsuleLibrary capsuleLibrary;
Dashboard dashboard;
ButtonDebouncer bootButton;
DeviceConfig deviceConfig;
WifiController wifi;
TencentWorker tencentWorker;
ProvisioningPortal provisioningPortal;
ProvisioningDiagnostics provisioningDiagnostics;
ProvisioningCoordinator provisioningCoordinator;
PokePodLinkService linkService;
LinkServiceCoordinator linkCoordinator;
WirelessSyncIdentity wirelessSyncIdentity;
WirelessSyncService wirelessSync;
RaiseToWakePolicy raiseToWake;
RuntimePowerManager runtimePower;
AutoScreenOffPolicy autoScreenOff;
LowBatteryShutdownPolicy lowBatteryShutdown;

uint8_t audioBuffer[kAudioBytesPerChunk];
TouchGestureTracker touchGesture;
bool touchWirelessHolding = false;
bool touchWirelessAttempted = false;
bool touchCapsuleSelectionAttempted = false;
bool touchVerticalScrolling = false;
bool scrollRedrawPending = false;
bool bootWirelessHolding = false;
bool bootProvisioningExitArmed = false;
bool bootScreenWakeArmed = false;
bool wirelessUiActive = false;
UiAction touchAction = UiAction::none;
uint32_t lastTouchMs = 0;
uint32_t lastDashboardMs = 0;
uint32_t lastScrollFrameMs = 0;
uint32_t lastSensorMs = 0;
uint32_t bootPressedAtMs = 0;
uint32_t lastNetworkTimeSyncRevision = 0;
bool ignoreTouchUntilRelease = false;
bool lastUsbHostConnected = false;
bool lastVbusPresent = false;
bool screenDimmed = false;
String transientMessage;
uint32_t transientUntilMs = 0;
CapsuleUndoState trashUndo;
std::vector<String> pendingPurgeIds;
PowerDecision currentPowerDecision;
bool idleRadiosPaused = false;

void noteUserActivity(uint32_t nowMs = millis()) {
  autoScreenOff.noteActivity(nowMs);
  if (idleRadiosPaused) {
    bleVoice.resumeAfterIdleSleep();
    idleRadiosPaused = false;
  }
  if (board.status().screenOn && screenDimmed) {
    board.setScreenBrightness(BoardServices::kActiveScreenBrightness);
    screenDimmed = false;
  }
}

bool automaticWakeEnabled() {
  return deviceConfig.settings().raiseToWake;
}

bool usbCableConnected() {
  const BoardStatus &status = board.status();
  return usbPhysicalConnected(usb.hostConnected(), status.pmu,
                              status.vbusPresent);
}

void setScreenState(bool enabled) {
  if (board.status().screenOn == enabled) return;
  if (!enabled) (void)board.takeTouchInterrupt();
  board.setScreenOn(enabled);
  screenDimmed = false;
  board.configureScreenOffSensors(!enabled, automaticWakeEnabled(),
                                  usb.log());
  if (enabled) {
    noteUserActivity();
    dashboard.invalidate();
  }
}

PowerInputs currentPowerInputs(uint32_t nowMs = millis()) {
  PowerInputs input;
  input.screenOn = board.status().screenOn;
  input.audioActive = audio.active() || recorder.recording() || audio.playing();
  input.bleConnected = bleVoice.radioActive();
  input.bleStreaming = bleVoice.streaming();
  input.wifiRadioOn = wifi.radioOn() || provisioningCoordinator.ownsWifi();
  // A charger supplies VBUS without opening a Mac CDC host session. Keep the
  // physical icon semantics in usbCableConnected(), while power policy uses
  // the real host session and the independent VBUS fact.
  input.usbHostConnected = usb.hostConnected();
  input.vbusPresent = board.status().vbusPresent;
  input.linkBusy = linkService.receivingBinary() ||
      linkService.maintenanceActive() || wirelessSync.linkBusy() ||
      wirelessSync.openWindow();
  input.storageBusy = recorder.recording();
  input.networkBusy = tencentWorker.working() || wirelessSync.linkBusy() ||
      wifi.phase() == WifiPhase::connecting;
  input.provisioning = provisioningCoordinator.visible();
  input.uiAnimating = dashboard.scrollActive() ||
      dashboard.pageTransitionActive();
  input.automaticWakeEnabled = automaticWakeEnabled();
  input.criticalBattery = lowBatteryShutdown.critical();
  input.idleMs = autoScreenOff.idleMs(nowMs);
  return input;
}

bool pauseIdleRadios() {
  wifi.prepareForSleep();
  const bool bleReady = bleVoice.pauseForIdleSleep();
  idleRadiosPaused = bleVoice.idlePaused();
  return bleReady && !wifi.radioOn();
}

void enterDeepSleep() {
  (void)board.takeTouchInterrupt();
  if (!runtimePower.armDeepSleepWakeSources(automaticWakeEnabled(),
                                             usb.log())) {
    noteUserActivity();
    return;
  }
  wifi.prepareForSleep();
  bleVoice.prepareForDeepSleep();
  audio.stopHardware(usb.log());
  if (board.sdReady()) SD_MMC.end();
  board.prepareForDeepSleep(
      runtimePower.snapshot().deepSleepTouchWakeArmed, usb.log());
  runtimePower.startDeepSleep(usb.log());
}

[[noreturn]] void performSafeShutdown() {
  wifi.prepareForSleep();
  bleVoice.prepareForDeepSleep();
  audio.stopHardware(usb.log());
  if (board.sdReady()) SD_MMC.end();
  board.safeShutdown();
  while (true) delay(1000);
}

String recordingId() {
  uint8_t randomBytes[16];
  for (size_t offset = 0; offset < sizeof(randomBytes); offset += 4) {
    const uint32_t random = esp_random();
    memcpy(randomBytes + offset, &random, sizeof(random));
  }
  char value[37];
  formatUuidV4(randomBytes, value);
  return String(value);
}

String deviceId() {
  char value[24];
  formatPokePodDeviceId(ESP.getEfuseMac(), value);
  return String(value);
}

void showMessage(const String &message, uint32_t durationMs = 1800) {
  transientMessage = message;
  transientUntilMs = millis() + durationMs;
}

void armTrashUndo(const std::vector<String> &ids) {
  std::vector<std::string> stableIds;
  stableIds.reserve(ids.size());
  for (const String &id : ids) stableIds.emplace_back(id.c_str());
  trashUndo.arm(stableIds, millis());
  showMessage("已删除 · 点此撤销", CapsuleUndoState::kDurationMs);
}

void restoreRecentTrash() {
  const uint32_t now = millis();
  std::vector<std::string> failedIds;
  const std::vector<std::string> pending = trashUndo.pendingIds();
  for (const std::string &id : pending) {
    if (!capsuleLibrary.restore(id.c_str())) failedIds.push_back(id);
  }
  const CapsuleUndoResult result = trashUndo.finishAttempt(failedIds);
  dashboard.invalidate();
  if (result.failed() == 0) {
    showMessage(String("已恢复 ") + result.restored + " 条");
  } else {
    const String message = String("已恢复 ") + result.restored +
        " 条，失败 " + result.failed() + " 条 · 再试";
    const uint32_t remaining = trashUndo.remainingMs(now);
    showMessage(message, remaining == 0 ? 1800 : remaining);
  }
}

void drawDashboard() {
  if (!board.status().screenOn) return;
  const uint32_t now = millis();
  DashboardView view;
  view.board = &board.status();
  view.library = &capsuleLibrary;
  view.settings = &deviceConfig.settings();
  view.audioReady = audio.ready();
  view.usbReady = usb.ready();
  view.usbConnected = usbCableConnected();
  view.bleVoiceConnected = bleVoice.connected();
  view.bleVoiceReady = bleVoice.appReady();
  view.bleVoiceBonded = bleVoice.bonded();
  view.bleVoicePairing = bleVoice.pairingMode(now);
  view.bleVoicePasskey = bleVoice.passkey();
  view.bleVoiceMtu = bleVoice.mtu();
  const BleVoiceQualitySnapshot bleQuality = bleVoice.quality();
  view.bleVoiceNotifyFailures = bleQuality.notifyFailures;
  view.bleVoiceQueueOverflows = bleQuality.queueOverflows;
  view.bleVoiceReadyTimeouts = bleQuality.readyTimeouts;
  view.bleVoiceStopAckTimeouts = bleQuality.stopAckTimeouts;
  view.bleVoiceStreamTimeouts = bleQuality.streamTimeouts;
  view.wifiSyncOpen = wirelessSync.openWindow();
  view.wifiSyncSecureReady = wirelessSync.secureReady();
  view.wifiSyncPaired = wirelessSync.paired();
  view.wifiSyncNetworkConnected = wirelessSync.networkConnected();
  view.wifiSyncListener = wirelessSync.listenerActive();
  view.wifiSyncBonjour = wirelessSync.bonjourActive();
  view.wifiSyncClient = wirelessSync.clientConnected();
  view.wifiSyncAuthenticated = wirelessSync.authenticated();
  view.wifiSyncBusy = wirelessSync.linkBusy();
  view.wifiSyncCompleted = wirelessSync.completed();
  view.wifiSyncRemainingSeconds = wirelessSync.remainingSeconds(now);
  view.wifiSyncLastCompletedAtMs = wirelessSync.lastCompletedAtMs();
  view.wifiSyncLastError = wirelessSync.lastError();
  view.wirelessHolding = wirelessUiActive;
  view.recording = recorder.recording();
  view.transcribing = tencentWorker.working();
  view.playing = audio.playing();
  view.provisioning = provisioningCoordinator.visible();
  view.undoAvailable = trashUndo.available(now);
  view.recordingMs = recorder.durationMs();
  view.audioPeak = audio.consumePeakWindow();
  audio.copyEnvelope(view.audioEnvelope, PeakWindow::kEnvelopeSamples);
  view.wifiPhase = provisioningCoordinator.visible()
      ? WifiPhase::provisioning : wifi.phase();
  view.wifiRssi = wifi.rssi();
  view.portalSsid = provisioningPortal.ssid();
  view.portalPassword = provisioningPortal.password();
  view.portalStatus = provisioningPortal.statusMessage();
  view.portalState = provisioningPortal.state();
  view.provisioningDiagnostics = &provisioningDiagnostics;
  if (deadlinePending(now, transientUntilMs)) view.message = transientMessage;
  dashboard.draw(view);
}

bool startWirelessHold() {
  if (!bleVoice.appReady()) {
    showMessage(bleVoice.connected() ? "蓝牙连接质量不足" : "等待 Mac 应用");
    drawDashboard();
    return false;
  }
  if (audio.playing()) audio.stopPlayback(usb.log());
  if (!captureRouter.available() || !audio.startCapture(usb.log())) {
    showMessage("无线麦克风暂时不可用");
    drawDashboard();
    return false;
  }
  uint32_t sessionId = esp_random();
  if (sessionId == 0) sessionId = 1;
  if (!bleVoice.startSession(sessionId, millis(), captureRouter)) {
    audio.stopHardware(usb.log());
    showMessage("无线麦克风暂时不可用");
    drawDashboard();
    return false;
  }
  wirelessUiActive = true;
  noteUserActivity();
  transientMessage = "";
  transientUntilMs = 0;
  drawDashboard();
  return true;
}

bool stopWirelessHold() {
  bleVoice.endSession();
  wirelessUiActive = false;
  noteUserActivity();
  transientMessage = "";
  transientUntilMs = 0;
  drawDashboard();
  return true;
}

void toggleRecording() {
  if (recorder.recording()) {
    const bool ok = recorder.stop(usb.log());
    captureRouter.release(AudioCaptureOwner::localCapsule);
    audio.stopHardware(usb.log());
    if (ok) capsuleLibrary.scan();
    showMessage(ok ? "胶囊已进入转写队列" : "录音提交失败");
  } else if (tencentWorker.working()) {
    showMessage("当前胶囊正在转写");
  } else if (!board.sdReady()) {
    showMessage("请插入 microSD 卡");
  } else if (!audio.ready()) {
    showMessage("麦克风尚未就绪");
  } else {
    if (audio.playing()) audio.stopPlayback(usb.log());
    tencentWorker.wake();
    const bool acquired = captureRouter.acquire(AudioCaptureOwner::localCapsule);
    const bool ok = acquired && audio.startCapture(usb.log()) &&
        recorder.start(usb.log(), recordingId(), board.utcNow());
    if (ok) {
      audio.resetPeakWindow();
      transientMessage = "";
      transientUntilMs = 0;
    } else {
      captureRouter.release(AudioCaptureOwner::localCapsule);
      audio.stopHardware(usb.log());
      showMessage("录音启动失败");
    }
  }
  noteUserActivity();
  drawDashboard();
}

void emitStatus() {
  const BoardStatus &s = board.status();
  const BleVoiceQualitySnapshot quality = bleVoice.quality();
  const RuntimePowerSnapshot &power = runtimePower.snapshot();
  const AudioFrontEndMetrics &frontEnd = recorder.audioMetrics();
  usb.log().printf(
      "{\"event\":\"status\",\"variant\":\"%s\",\"display\":%s,\"touch\":%s,\"sd\":%s,\"audio\":%s,\"audio_active\":%s,\"usb\":%s,\"host_connected\":%s,\"ble_voice_connected\":%s,\"ble_voice_ready\":%s,\"ble_voice_mtu\":%u,\"ble_voice_streaming\":%s,\"ble_voice_notify_attempts\":%lu,\"ble_voice_notify_accepted\":%lu,\"ble_voice_notify_failures\":%lu,\"ble_voice_queue_overflows\":%lu,\"ble_voice_session_failures\":%lu,\"ble_voice_ready_timeouts\":%lu,\"ble_voice_stop_ack_timeouts\":%lu,\"ble_voice_stream_timeouts\":%lu,\"ble_voice_last_error_code\":%u,\"audio_read_bytes\":%llu,\"audio_read_failures\":%lu,\"audio_peak\":%u,\"audio_frontend_channel\":\"%s\",\"audio_frontend_left_peak\":%u,\"audio_frontend_right_peak\":%u,\"audio_frontend_output_peak\":%u,\"audio_frontend_noise_floor\":%u,\"audio_frontend_suppressed_samples\":%lu,\"audio_frontend_limited_samples\":%lu,\"audio_frontend_max_gain_q12\":%lu,\"recording\":%s,\"duration_ms\":%lu,\"battery\":%d,\"charging\":%s,\"vbus\":%s,\"wifi\":\"%s\",\"wifi_rssi\":%ld,\"wifi_radio_on\":%s,\"wifi_power_save\":%s,\"pending_capsules\":%u,\"tencent_configured\":%s,\"transcribing\":%s,\"power_mode\":\"%s\",\"cpu_mhz\":%u,\"light_sleep_count\":%lu,\"light_sleep_us\":%llu,\"deep_sleep_wake_count\":%lu,\"woke_from_deep_sleep\":%s,\"deep_sleep_touch_wake\":%s,\"critical_battery\":%s,\"idle_ms\":%lu,\"last_wake_cause\":%u,\"reset_reason\":%u,\"internal_heap_free\":%u,\"internal_heap_largest\":%u,\"psram_free\":%u,\"automatic_pm_supported\":%s,\"ble_modem_sleep_supported\":%s,\"provisioning_startup_phase\":\"%s\",\"provisioning_diagnostic_count\":%u}\n",
      variantName(s.variant), s.display ? "true" : "false", s.touch ? "true" : "false",
      s.sdCard ? "true" : "false", audio.ready() ? "true" : "false",
      audio.active() ? "true" : "false",
      usb.ready() ? "true" : "false", usb.hostConnected() ? "true" : "false",
      bleVoice.connected() ? "true" : "false",
      bleVoice.appReady() ? "true" : "false", bleVoice.mtu(),
      bleVoice.streaming() ? "true" : "false",
      static_cast<unsigned long>(quality.notifyAttempts),
      static_cast<unsigned long>(quality.notifyAccepted),
      static_cast<unsigned long>(quality.notifyFailures),
      static_cast<unsigned long>(quality.queueOverflows),
      static_cast<unsigned long>(quality.sessionFailures),
      static_cast<unsigned long>(quality.readyTimeouts),
      static_cast<unsigned long>(quality.stopAckTimeouts),
      static_cast<unsigned long>(quality.streamTimeouts),
      static_cast<unsigned>(quality.lastErrorCode),
      static_cast<unsigned long long>(audio.bytesRead()),
      static_cast<unsigned long>(audio.readFailures()), audio.peakSample(),
      audioInputChannelName(frontEnd.selectedChannel), frontEnd.leftPeak,
      frontEnd.rightPeak, frontEnd.outputPeak,
      frontEnd.estimatedNoiseFloor,
      static_cast<unsigned long>(frontEnd.suppressedSamples),
      static_cast<unsigned long>(frontEnd.limitedSamples),
      static_cast<unsigned long>(frontEnd.maximumGainQ12),
      recorder.recording() ? "true" : "false",
      static_cast<unsigned long>(recorder.durationMs()), s.batteryPercent,
      s.charging ? "true" : "false", s.vbusPresent ? "true" : "false",
      wifi.phaseName(), static_cast<long>(wifi.rssi()),
      wifi.radioOn() ? "true" : "false",
      wifi.powerSaveEnabled() ? "true" : "false",
      static_cast<unsigned>(capsuleLibrary.pendingCount()),
      deviceConfig.hasTencent() ? "true" : "false",
      tencentWorker.working() ? "true" : "false",
      powerModeName(power.mode), power.cpuMhz,
      static_cast<unsigned long>(power.lightSleepCount),
      static_cast<unsigned long long>(power.lightSleepUs),
      static_cast<unsigned long>(power.deepSleepWakeCount),
      power.wokeFromDeepSleep ? "true" : "false",
      power.deepSleepTouchWakeArmed ? "true" : "false",
      lowBatteryShutdown.critical() ? "true" : "false",
      static_cast<unsigned long>(autoScreenOff.idleMs(millis())),
      static_cast<unsigned>(power.lastWakeCause),
      static_cast<unsigned>(esp_reset_reason()),
      static_cast<unsigned>(heap_caps_get_free_size(
          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      static_cast<unsigned>(heap_caps_get_largest_free_block(
          MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
      static_cast<unsigned>(ESP.getFreePsram()),
      power.automaticPmSupported ? "true" : "false",
      power.bleModemSleepSupported ? "true" : "false",
      provisioningCoordinator.phaseName(),
      static_cast<unsigned>(provisioningDiagnostics.count()));
  usb.log().printf(
      "{\"event\":\"wifi_sync_status\",\"window\":%s,\"phase\":\"%s\",\"secure\":%s,\"listener\":%s,\"bonjour\":%s,\"client\":%s,\"authenticated\":%s,\"remaining_seconds\":%lu,\"last_error\":\"%s\"}\n",
      wirelessSync.openWindow() ? "true" : "false",
      wirelessSyncWindowPhaseName(wirelessSync.phase()),
      wirelessSync.secureReady() ? "true" : "false",
      wirelessSync.listenerActive() ? "true" : "false",
      wirelessSync.bonjourActive() ? "true" : "false",
      wirelessSync.clientConnected() ? "true" : "false",
      wirelessSync.authenticated() ? "true" : "false",
      static_cast<unsigned long>(wirelessSync.remainingSeconds(millis())),
      wirelessSync.lastError());
}

void pollTouch() {
  const uint32_t now = millis();
  int16_t x = 0;
  int16_t y = 0;
  if (!board.status().screenOn) {
    const bool touchInterrupt = board.takeTouchInterrupt();
    if (automaticWakeEnabled() && touchInterrupt && board.readTouch(x, y)) {
      ignoreTouchUntilRelease = true;
      setScreenState(true);
    }
    return;
  }
  if (dashboard.pageTransitionActive()) {
    if (board.readTouch(x, y)) ignoreTouchUntilRelease = true;
    return;
  }
  const bool touched = board.readTouch(x, y);
  if (ignoreTouchUntilRelease) {
    if (!touched) ignoreTouchUntilRelease = false;
    return;
  }
  if (touched && !touchGesture.active) {
    noteUserActivity(now);
    touchGesture.begin(x, y, now);
    touchWirelessAttempted = false;
    touchCapsuleSelectionAttempted = false;
    touchVerticalScrolling = false;
    touchAction = dashboard.actionAt(x, y, bleVoice.appReady());
  } else if (touched) {
    touchGesture.update(x, y);
    if (!touchVerticalScrolling && !touchWirelessHolding &&
        !touchCapsuleSelectionAttempted &&
        touchGesture.verticalSwipe()) {
      touchVerticalScrolling = dashboard.beginVerticalScroll(
          touchGesture.startY, touchGesture.startedAtMs, capsuleLibrary);
    }
    if (touchVerticalScrolling) {
      if (dashboard.updateVerticalScroll(y, now, capsuleLibrary)) {
        noteUserActivity(now);
        scrollRedrawPending = true;
      }
      return;
    }
    if (touchAction == UiAction::wechatVoice &&
        !touchWirelessAttempted &&
        touchGesture.wirelessHoldReady(now)) {
      touchWirelessAttempted = true;
      touchWirelessHolding = startWirelessHold();
    } else if (touchAction == UiAction::openCapsule &&
               !touchCapsuleSelectionAttempted &&
               !dashboard.capsuleSelectionMode() &&
               touchGesture.tapEligible() &&
               now - touchGesture.startedAtMs >=
                   CapsuleBrowserState::kLongPressMs) {
      touchCapsuleSelectionAttempted = true;
      if (!dashboard.beginCapsuleSelectionAt(touchGesture.startY,
                                             capsuleLibrary)) {
        showMessage("转写中或版本只读，暂时不能选择");
      }
      drawDashboard();
    }
  } else if (!touched) {
    if (!touchGesture.active) return;
    const int16_t deltaX = touchGesture.deltaX();
    const int16_t startX = touchGesture.startX;
    const int16_t startY = touchGesture.startY;
    const bool horizontalSwipe = touchGesture.horizontalSwipe();
    const bool verticalSwipe = touchGesture.verticalSwipe();
    const bool tapEligible = touchGesture.tapEligible();
    touchGesture.reset();
    if (touchVerticalScrolling) {
      touchVerticalScrolling = false;
      dashboard.endVerticalScroll(now);
      noteUserActivity(now);
      scrollRedrawPending = false;
      lastScrollFrameMs = now;
      drawDashboard();
      return;
    }
    if (touchWirelessHolding) {
      touchWirelessHolding = false;
      stopWirelessHold();
      return;
    }
    if (touchCapsuleSelectionAttempted) return;
    if (horizontalSwipe) {
      if (provisioningCoordinator.visible() &&
          isBackEdgeSwipe(startX, deltaX)) {
        if (dashboard.state().screen() != UiScreen::provisioningLog) {
          provisioningCoordinator.stop();
        }
        dashboard.back();
      } else {
        dashboard.swipeHorizontal(deltaX, recorder.recording(), startX);
      }
      drawDashboard();
      return;
    }
    if (verticalSwipe) return;
    if (!tapEligible) return;
    noteUserActivity(now);
    const UiAction action = touchAction;
    if (action == UiAction::undoTrash) {
      restoreRecentTrash();
      drawDashboard();
    } else if (action == UiAction::capsuleRecord) toggleRecording();
    else if (action == UiAction::wechatVoice) return;
    else if (action == UiAction::openCapsule) {
      if (dashboard.capsuleSelectionMode()) {
        if (!dashboard.toggleCapsuleSelectionAt(startY, capsuleLibrary)) {
          showMessage("转写中或版本只读，暂时不能选择");
        }
      } else {
        dashboard.openCapsuleAt(startY, capsuleLibrary);
      }
      drawDashboard();
    } else if (action == UiAction::back) {
      if (provisioningCoordinator.visible() &&
          dashboard.state().screen() != UiScreen::provisioningLog) {
        provisioningCoordinator.stop();
      }
      dashboard.back();
      drawDashboard();
    } else if (action == UiAction::openProvisioningLog) {
      dashboard.openProvisioningLog();
      drawDashboard();
    } else if (action == UiAction::wifiToggle) {
      if (wifiUiSwitchOn(wifi.phase())) {
        if (wirelessSync.openWindow()) wirelessSync.close();
        if (deviceConfig.setWifiEnabled(false, usb.log())) {
          wifi.configurationChanged();
          showMessage("Wi-Fi 已关闭");
        }
      } else if (!deviceConfig.hasWifi()) {
        showMessage("请先完成手机配网");
      } else {
        bool enabled = deviceConfig.settings().wifiEnabled;
        if (!enabled) {
          enabled = deviceConfig.setWifiEnabled(true, usb.log());
        }
        if (enabled) {
          wifi.requestConnection();
          tencentWorker.wake();
          showMessage("正在连接 Wi-Fi");
        }
      }
      dashboard.invalidate();
      drawDashboard();
    } else if (action == UiAction::openBluetoothPairing) {
      dashboard.openBluetoothPairing();
      drawDashboard();
    } else if (action == UiAction::toggleBluetoothPairing) {
      if (bleVoice.pairingMode(now)) {
        bleVoice.cancelPairingMode();
        showMessage("已取消配对");
      } else {
        bleVoice.enterPairingMode(now);
        char pairMessage[48];
        snprintf(pairMessage, sizeof(pairMessage), "配对码 %06lu · 长按忘记",
                 static_cast<unsigned long>(bleVoice.passkey()));
        showMessage(String(pairMessage), 5000);
      }
      dashboard.invalidate();
      drawDashboard();
    } else if (action == UiAction::forgetBluetoothMac) {
      if (bleVoice.bonded()) {
        bleVoice.forgetMac();
        showMessage("已忘记 Mac");
      } else {
        showMessage("当前没有已配对 Mac");
      }
      dashboard.invalidate();
      drawDashboard();
    } else if (action == UiAction::openComputerSync) {
      if (computerSyncEntryDecision(wirelessSync.openWindow()) ==
          ComputerSyncEntryDecision::openAndNavigate) {
        wirelessSync.open(now);
      }
      dashboard.openComputerSync();
      dashboard.invalidate();
      drawDashboard();
    } else if (action == UiAction::closeComputerSync) {
      if (wirelessSync.openWindow()) wirelessSync.close();
      dashboard.back();
      showMessage("电脑同步已关闭");
      dashboard.invalidate();
      drawDashboard();
    } else if (action == UiAction::openProvisioning) {
      if (wirelessSync.openWindow()) wirelessSync.close();
      if (provisioningCoordinator.request(now)) {
        showMessage("正在准备配网热点");
      } else {
        showMessage("配网启动请求失败");
      }
      dashboard.invalidate();
      drawDashboard();
    } else if (action == UiAction::raiseToWakeToggle) {
      const bool enabled = !automaticWakeEnabled();
      if (deviceConfig.setRaiseToWake(enabled, usb.log())) {
        showMessage(enabled ? "自动亮屏已开启" : "自动亮屏已关闭");
      }
      dashboard.invalidate();
      drawDashboard();
    } else if (action == UiAction::openCapsuleScope) {
      dashboard.openScopePicker();
      drawDashboard();
    } else if (capsuleScopeIndexForAction(action) >= 0) {
      capsuleLibrary.setScope(static_cast<CapsuleScope>(
          capsuleScopeIndexForAction(action)));
      dashboard.scopeChanged();
      drawDashboard();
    } else if (action == UiAction::openDetailMore) {
      dashboard.openDetailMore();
      drawDashboard();
    } else if (action == UiAction::requestPurge) {
      pendingPurgeIds.clear();
      if (dashboard.capsuleSelectionMode()) {
        pendingPurgeIds = dashboard.selectedCapsuleIds(capsuleLibrary);
      } else {
        const CapsuleSummary *selected = dashboard.selected(capsuleLibrary);
        if (selected != nullptr) pendingPurgeIds.push_back(selected->id);
      }
      bool safe = !pendingPurgeIds.empty();
      for (const String &id : pendingPurgeIds) {
        const CapsuleSummary *record = capsuleLibrary.find(id);
        if (record == nullptr || !record->trashed || record->readOnly ||
            record->status == CapsuleStatus::transcribing) {
          safe = false;
          break;
        }
      }
      if (safe) {
        dashboard.openPurgeConfirm(pendingPurgeIds.size());
      } else {
        pendingPurgeIds.clear();
        showMessage("版本过新或状态忙，请在 Mac 处理");
      }
      drawDashboard();
    } else if (action == UiAction::confirmPurge) {
      const CapsuleBatchResult result = capsuleLibrary.purge(pendingPurgeIds);
      const size_t requested = pendingPurgeIds.size();
      pendingPurgeIds.clear();
      dashboard.closeOverlays();
      dashboard.clearCapsuleSelection();
      if (result.ok) {
        if (dashboard.state().screen() == UiScreen::capsuleDetail) {
          dashboard.back();
        }
        showMessage(String("已永久删除 ") + result.changed + " 条");
      } else if (result.rolledBackFully) {
        showMessage("永久删除失败，已完整回滚");
      } else if (result.rollbackFailed > 0) {
        showMessage(String("删除回滚失败 ") + result.rollbackFailed +
                    " 条，请到 Mac 处理");
      } else {
        showMessage(requested == 0 ? "没有可删除的胶囊"
                                   : "永久删除未完成");
      }
      dashboard.invalidate();
      drawDashboard();
    } else if (action == UiAction::closeOverlay) {
      pendingPurgeIds.clear();
      dashboard.closeOverlays();
      drawDashboard();
    } else if (action == UiAction::bulkFavorite ||
               action == UiAction::bulkArchive ||
               action == UiAction::bulkTrash) {
      const std::vector<String> ids =
          dashboard.selectedCapsuleIds(capsuleLibrary);
      const CapsuleScope scope = capsuleLibrary.scope();
      const CapsuleBatchAction batchAction =
          action == UiAction::bulkFavorite
              ? CapsuleBatchAction::favorite
              : (action == UiAction::bulkArchive
                     ? CapsuleBatchAction::archiveOrRestore
                     : CapsuleBatchAction::trashOrRestore);
      const CapsuleBatchResult result = capsuleLibrary.batch(
          ids, batchAction, board.utcNow());
      dashboard.clearCapsuleSelection();
      if (!result.ok) {
        if (result.rolledBackFully) {
          showMessage("批量失败，已完整回滚");
        } else if (result.rollbackFailed > 0) {
          showMessage(String("回滚失败 ") + result.rollbackFailed +
                      " 条，请到 Mac 处理");
        } else {
          showMessage("批量操作未完成");
        }
      } else if (action == UiAction::bulkTrash &&
                 scope != CapsuleScope::trash) {
        armTrashUndo(ids);
      } else {
        showMessage(String("已处理 ") + result.changed + " 条");
      }
      drawDashboard();
    } else {
      const CapsuleSummary *selected = dashboard.selected(capsuleLibrary);
      if (selected == nullptr) return;
      const String id = selected->id;
      if (selected->readOnly &&
          (action == UiAction::favorite || action == UiAction::archive ||
           action == UiAction::trash || action == UiAction::retry ||
           action == UiAction::play)) {
        showMessage("版本过新，请在 Mac 处理");
        drawDashboard();
        return;
      }
      if (action == UiAction::favorite) {
        capsuleLibrary.toggleFavorite(id);
        dashboard.invalidate();
      } else if (action == UiAction::archive) {
        const bool wasTrashed = selected->trashed;
        const bool wasArchived = selected->archived;
        const bool ok = wasTrashed
            ? capsuleLibrary.restore(id)
            : (wasArchived ? capsuleLibrary.unarchive(id)
                           : capsuleLibrary.archive(id));
        if (ok) {
          dashboard.back();
          showMessage(wasTrashed ? "已恢复" :
                      (wasArchived ? "已移回收件箱" : "已归档"));
        } else {
          showMessage("操作失败");
        }
      } else if (action == UiAction::trash) {
        if (capsuleLibrary.trash(id, board.utcNow())) {
          dashboard.closeOverlays();
          dashboard.back();
          std::vector<String> ids;
          ids.push_back(id);
          armTrashUndo(ids);
        } else {
          showMessage("删除失败");
        }
      } else if (action == UiAction::retry) {
        if (selected->status != CapsuleStatus::failed &&
            !(selected->status == CapsuleStatus::queued &&
              !selected->error.isEmpty())) return;
        if (capsuleLibrary.requeue(id)) {
          dashboard.closeOverlays();
          tencentWorker.wake();
          showMessage("已重新加入转写队列");
        } else {
          showMessage("重新转写失败");
        }
      } else if (action == UiAction::play) {
        if (audio.playing()) {
          audio.stopPlayback(usb.log());
          showMessage("已停止播放");
        } else if (!captureRouter.available() || recorder.recording() ||
                   tencentWorker.working()) {
          showMessage("麦克风使用中，暂时无法播放");
        } else if (!safeCapsuleFileName(selected->audioFile.c_str()) ||
                   !audio.startPlayback(
                       SD_MMC, selected->directory + "/" + selected->audioFile,
                       usb.log())) {
          showMessage("音频播放失败");
        } else {
          showMessage("正在播放");
        }
      }
      drawDashboard();
    }
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  pinMode(kBootButtonPin, INPUT_PULLUP);

  board.begin(Serial);
  beginTlsExternalMemory(Serial);
  provisioningDiagnostics.begin(
      Serial, static_cast<uint16_t>(esp_reset_reason()));
  audio.begin(Serial);
  const bool usbStarted = usb.begin(board.status().variant);
  const bool bleStarted = bleVoice.begin(deviceId(), usb.log());
  deviceConfig.begin(usb.log());
  const bool syncIdentityStarted =
      wirelessSyncIdentity.begin(ESP.getEfuseMac(), usb.log());
  if (board.sdReady() && recorder.begin(SD_MMC, usb.log())) {
    recorder.recoverInterrupted(usb.log(), board.utcNow());
    capsuleLibrary.begin(SD_MMC, usb.log());
    tencentWorker.begin(SD_MMC, capsuleLibrary, deviceConfig, usb.log());
  }
  wifi.begin(deviceConfig, usb.log());
  provisioningCoordinator.begin(provisioningPortal, wifi, deviceConfig,
                                provisioningDiagnostics, usb.log());
  runtimePower.begin(usb.log());
  linkService.begin(usb.stream(), SD_MMC, board, audio, captureRouter,
                    usb, bleVoice,
                    dashboard,
                    capsuleLibrary, recorder,
                    deviceConfig, wifi, tencentWorker,
                    provisioningDiagnostics, runtimePower, usb.log(),
                    &linkCoordinator, LinkTransport::usb, &wirelessSync,
                    nullptr, &provisioningCoordinator);
  const bool wifiSyncStarted = wirelessSync.begin(
      SD_MMC, board, audio, captureRouter, usb, bleVoice, dashboard,
      capsuleLibrary, recorder, deviceConfig, wifi, tencentWorker,
      provisioningDiagnostics, runtimePower, wirelessSyncIdentity,
      linkCoordinator, usb.log());
  dashboard.begin(board.display(), board.sdReady() ? &SD_MMC : nullptr);
  if (provisioningDiagnostics.recoveredInterruptedSession()) {
    showMessage("上次配网被重启中断 · 见诊断", 5000);
  } else {
    showMessage(usbStarted && bleStarted && syncIdentityStarted &&
                        wifiSyncStarted
                    ? "PokePod 已就绪"
                    : (usbStarted && bleStarted
                           ? "无线同步安全服务未就绪"
                           : "连接服务启动失败"),
                3000);
  }
  drawDashboard();
  autoScreenOff.begin(millis());
  lastUsbHostConnected = usb.hostConnected();
  lastVbusPresent = board.status().vbusPresent;
  emitStatus();
}

void loop() {
  const uint32_t now = millis();
  const bool usbHostConnected = usb.hostConnected();
  const bool usbHostSessionClosed = usb.takeHostSessionClosed();
  if ((lastUsbHostConnected && !usbHostConnected) || usbHostSessionClosed) {
    usb.discardHostSessionBuffers();
    linkService.disconnect();
  }
  lastUsbHostConnected = usbHostConnected;
  if (trashUndo.expire(now)) {
    dashboard.invalidate();
  }

  // A CDC upload can otherwise overrun TinyUSB while a full-screen AMOLED
  // redraw or an SD/network task owns the main loop. Once a binary request has
  // started, drain it before doing any optional UI or sensor work.
  wirelessSync.enforceDeadline(now);
  if (linkService.receivingBinary()) {
    linkService.poll(now);
    return;
  }
  if (wirelessSync.receivingBinary()) {
    wirelessSync.poll(now, wifi.connected());
    return;
  }
  if (bootButton.update(digitalRead(kBootButtonPin) == LOW, now)) {
    noteUserActivity(now);
    if (!board.status().screenOn) {
      bootScreenWakeArmed = true;
      setScreenState(true);
      return;
    }
    if (bootButton.releasedEdge() && bootScreenWakeArmed) {
      bootScreenWakeArmed = false;
      return;
    }
    if (bootButton.pressedEdge()) {
      if (provisioningCoordinator.visible()) {
        bootProvisioningExitArmed = true;
        return;
      }
      bootPressedAtMs = now;
    } else if (bootButton.releasedEdge()) {
      if (bootProvisioningExitArmed) {
        bootProvisioningExitArmed = false;
        if (provisioningCoordinator.visible()) provisioningCoordinator.stop();
        dashboard.back();
        dashboard.invalidate();
        showMessage("已退出手机配网");
        drawDashboard();
        return;
      }
      if (bootWirelessHolding) {
        bootWirelessHolding = false;
        stopWirelessHold();
        return;
      }
      if (bleVoice.appReady() && !recorder.recording()) {
        showMessage("请按住说话");
        drawDashboard();
      } else {
        toggleRecording();
      }
    }
  }
  if (bootButton.pressed() && bleVoice.appReady() &&
      !bootWirelessHolding && !recorder.recording() &&
      now - bootPressedAtMs >= ui::kWirelessHoldDelayMs) {
    bootWirelessHolding = startWirelessHold();
  }

  if (audio.playing()) {
    audio.pumpPlayback(usb.log());
  } else if (audio.active()) {
    size_t bytes = audio.read(audioBuffer, sizeof(audioBuffer));
    if (bytes > 0) {
      if (bleVoice.acceptingAudio() &&
          !bleVoice.appendAudio(audioBuffer, bytes, now)) {
        wirelessUiActive = false;
        showMessage("无线语音已中断");
      }
      if (recorder.recording()) {
        const bool wasRecording = recorder.recording();
        recorder.append(audioBuffer, bytes, usb.log());
        if (wasRecording && !recorder.recording()) {
          captureRouter.release(AudioCaptureOwner::localCapsule);
          audio.stopHardware(usb.log());
          capsuleLibrary.scan();
        }
      }
    }
  }
  bleVoice.poll(now);
  if (wirelessUiActive && !bleVoice.streaming()) {
    wirelessUiActive = false;
    dashboard.invalidate();
  }
  if (captureRouter.available() && !audio.playing() && audio.active()) {
    audio.stopHardware(usb.log());
  }

  wirelessSync.enforceDeadline(now);
  linkService.poll(now);
  if (linkService.receivingBinary()) {
    return;
  }

  provisioningCoordinator.poll(now);
  if (provisioningCoordinator.takeConfigurationChanged()) {
    tencentWorker.wake();
    dashboard.invalidate();
  }
  const bool networkWork = !lowBatteryShutdown.critical() &&
      capsuleLibrary.pendingCount() > 0 &&
      deviceConfig.hasTencent() && !tencentWorker.waitingForWake();
  wifi.loop(now, recorder.recording(), networkWork,
            board.status().charging, provisioningCoordinator.ownsWifi(),
            wirelessSync.wifiDemand());
  wirelessSync.poll(now, wifi.connected());
  const uint32_t timeSyncRevision = wifi.networkTimeSyncRevision();
  if (timeSyncRevision != lastNetworkTimeSyncRevision) {
    lastNetworkTimeSyncRevision = timeSyncRevision;
    const time_t synchronizedEpoch = time(nullptr);
    const bool rtcUpdated = board.setUtcEpoch(synchronizedEpoch);
    usb.log().printf(
        "{\"event\":\"network_time_applied\",\"revision\":%lu,\"generation\":%lu,\"rtc_updated\":%s}\n",
        static_cast<unsigned long>(timeSyncRevision),
        static_cast<unsigned long>(wifi.connectionGeneration()),
        rtcUpdated ? "true" : "false");
  }
  tencentWorker.loop(now, wifi.connected(), wifi.timeReady(),
                     transcriptionDispatchBusy(recorder.recording(),
                                               linkService.maintenanceActive() ||
                                                   wirelessSync.linkBusy()) ||
                         lowBatteryShutdown.critical(),
                     board.status().charging);
  currentPowerDecision = runtimePower.apply(currentPowerInputs(now), usb.log());
  if (currentPowerDecision.requestIdleRadioPause) {
    (void)pauseIdleRadios();
    currentPowerDecision = runtimePower.apply(currentPowerInputs(now),
                                               usb.log());
  }
  if (currentPowerDecision.requestSafeShutdown) performSafeShutdown();
  if (currentPowerDecision.requestDeepSleep && !bleVoice.connected() &&
      !wifi.radioOn()) {
    enterDeepSleep();
  }
  if (now - lastTouchMs >= currentPowerDecision.touchPollMs) {
    lastTouchMs = now;
    pollTouch();
  }
  if (dashboard.advanceVerticalScroll(now, capsuleLibrary)) {
    scrollRedrawPending = true;
  }
  if (scrollRedrawPending &&
      now - lastScrollFrameMs >= ui::kScrollFrameIntervalMs) {
    scrollRedrawPending = false;
    lastScrollFrameMs = now;
    lastDashboardMs = now;
    drawDashboard();
  }
  if (dashboard.pageTransitionActive() &&
      now - lastScrollFrameMs >= ui::kScrollFrameIntervalMs) {
    lastScrollFrameMs = now;
    if (dashboard.advancePageTransition(now)) lastDashboardMs = now;
  }

  const uint32_t sensorIntervalMs = currentPowerDecision.sensorPollMs;
  if (now - lastSensorMs >= sensorIntervalMs) {
    lastSensorMs = now;
    board.refreshSensors();
    const BoardStatus &status = board.status();
    (void)lowBatteryShutdown.update(status.batteryPercent,
                                    status.vbusPresent);
    if (status.vbusPresent != lastVbusPresent) {
      lastVbusPresent = status.vbusPresent;
      dashboard.invalidate();
    }
    bleVoice.setBatteryPercent(status.batteryPercent);
    if (!status.screenOn && automaticWakeEnabled() &&
        board.pollMotionWake()) {
      setScreenState(true);
    } else if (raiseToWake.update(now, automaticWakeEnabled(),
                                  status.screenOn, status.accelerationX,
                                  status.accelerationY,
                                  status.accelerationZ)) {
      setScreenState(true);
    }
    const PowerKeyEvent powerKey = board.pollPowerKey();
    if (powerKey == PowerKeyEvent::shortPress) {
      noteUserActivity(now);
      setScreenState(!board.status().screenOn);
    } else if (powerKey == PowerKeyEvent::longPress) {
      if (recorder.recording()) {
        recorder.stop(usb.log());
        captureRouter.release(AudioCaptureOwner::localCapsule);
        audio.stopHardware(usb.log());
      }
      if (bleVoice.streaming()) stopWirelessHold();
      if (audio.playing()) audio.stopPlayback(usb.log());
      performSafeShutdown();
    }
  }
  const bool keepScreenAwake = recorder.recording() || wirelessUiActive ||
      audio.playing() || provisioningCoordinator.visible() ||
      touchVerticalScrolling || dashboard.scrollActive() ||
      dashboard.pageTransitionActive();
  if (!screenDimmed &&
      autoScreenOff.shouldDim(now, board.status().screenOn,
                              keepScreenAwake)) {
    board.setScreenBrightness(BoardServices::kDimScreenBrightness);
    screenDimmed = true;
  }
  if (autoScreenOff.shouldTurnOff(now, board.status().screenOn,
                                 keepScreenAwake)) {
    setScreenState(false);
    currentPowerDecision = runtimePower.apply(currentPowerInputs(now),
                                               usb.log());
  }
  const uint32_t dashboardIntervalMs =
      (recorder.recording() || wirelessUiActive)
          ? ui::kRecordingFrameIntervalMs : 1000;
  if (now - lastDashboardMs >= dashboardIntervalMs) {
    lastDashboardMs = now;
    drawDashboard();
  }
  if (currentPowerDecision.allowLightSleep) {
    const PowerInputs verifiedInputs = currentPowerInputs(now);
    if (runtimePower.enterLightSleep(verifiedInputs, currentPowerDecision,
                                     usb.log()) &&
        runtimePower.snapshot().lastWakeCause == ESP_SLEEP_WAKEUP_GPIO) {
      setScreenState(true);
      noteUserActivity();
    }
  }
}
