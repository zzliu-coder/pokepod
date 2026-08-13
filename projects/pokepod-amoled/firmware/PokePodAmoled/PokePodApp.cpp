#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>

#include "AudioPipeline.h"
#include "AudioCaptureRuntime.h"
#include "AudioCaptureDispatcher.h"
#include "AudioCaptureRouter.h"
#include "BleVoiceService.h"
#include "BoardConfig.h"
#include "BoardServices.h"
#include "ButtonDebouncer.h"
#include "CapsuleLibrary.h"
#include "CapsuleOperationService.h"
#include "CapsulePolicy.h"
#include "CapsuleUndoState.h"
#include "CapabilityRegistry.h"
#include "Dashboard.h"
#include "DeviceConfig.h"
#include "ProvisioningPortal.h"
#include "ProvisioningDiagnostics.h"
#include "ProvisioningCoordinator.h"
#include "PokePodLinkService.h"
#include "LinkServiceCoordinator.h"
#include "LocalRecordingStart.h"
#include "PowerPolicy.h"
#include "PowerDiagnostics.h"
#include "RaiseToWakePolicy.h"
#include "RuntimePowerManager.h"
#include "ServiceQuiescencePolicy.h"
#include "StorageCoordinator.h"
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

class SdMmcRecordingCapacitySource final : public RecordingCapacitySource {
 public:
  RecordingSpaceSnapshot query() override {
    const uint64_t totalBytes = SD_MMC.totalBytes();
    const uint64_t usedBytes = SD_MMC.usedBytes();
    return {totalBytes, usedBytes, totalBytes != 0};
  }

  uint64_t monotonicMicros() override {
    return static_cast<uint64_t>(esp_timer_get_time());
  }
};

SdMmcRecordingCapacitySource recordingCapacitySource;
BoardServices board;
AudioPipeline audio;
AudioCaptureRouter captureRouter;
AudioCaptureRuntime captureRuntime;
AudioCaptureDispatcher captureDispatcher;
UsbLinkBridge usb;
BleVoiceService bleVoice;
WavRecorder recorder;
CapsuleLibrary capsuleLibrary;
CapsuleOperationService capsuleOperations;
Dashboard dashboard;
ButtonDebouncer bootButton;
DeviceConfig deviceConfig;
WifiController wifi;
TencentWorker tencentWorker;
ProvisioningPortal provisioningPortal;
ProvisioningDiagnostics provisioningDiagnostics;
PowerDiagnostics powerDiagnostics;
ProvisioningCoordinator provisioningCoordinator;
PokePodLinkService linkService;
LinkServiceCoordinator linkCoordinator;
WirelessSyncIdentity wirelessSyncIdentity;
WirelessSyncService wirelessSync;
RaiseToWakePolicy raiseToWake;
RuntimePowerManager runtimePower;
AutoScreenOffPolicy autoScreenOff;
LowBatteryShutdownPolicy lowBatteryShutdown;
SafeShutdownQuiescePolicy safeShutdownQuiesce;
CapabilityRegistry capabilities;
LocalRecordingStartState localRecordingStart;

class LocalRecordingStartGate final : public CapsuleTransactionGate {
 public:
  bool permits(uint32_t) override {
    return localRecordingStart.gatePermitted();
  }
};

LocalRecordingStartGate localRecordingStartGate;

TouchGestureTracker touchGesture;
bool touchWirelessHolding = false;
bool touchWirelessAttempted = false;
bool touchCapsuleSelectionAttempted = false;
bool touchVerticalScrolling = false;
bool scrollRedrawPending = false;
bool bootWirelessHolding = false;
bool bootProvisioningExitArmed = false;
bool bootProvisioningConfirmationConsumed = false;
bool bootScreenWakeArmed = false;
bool wirelessUiActive = false;
UiAction touchAction = UiAction::none;
uint32_t lastTouchMs = 0;
uint32_t lastDashboardMs = 0;
uint32_t lastScrollFrameMs = 0;
uint32_t lastSensorMs = 0;
AudioCaptureFrontEndSnapshot lastLocalCaptureMetrics;
uint32_t bootPressedAtMs = 0;
uint32_t lastNetworkTimeSyncRevision = 0;
uint32_t lastCapsuleLibraryRevision = 0;
bool ignoreTouchUntilRelease = false;
bool lastUsbHostConnected = false;
bool lastVbusPresent = false;
bool screenDimmed = false;
String transientMessage;
uint32_t transientUntilMs = 0;
CapsuleUndoState trashUndo;
std::vector<String> pendingPurgeIds;
std::vector<String> pendingLocalOperationIds;
PowerDecision currentPowerDecision;
bool idleRadiosPaused = false;
uint32_t nextSafeShutdownAttemptMs = 0;

enum class PendingCaptureStop : uint8_t {
  none = 0,
  wirelessVoice,
  localCapsule,
};

PendingCaptureStop pendingCaptureStop = PendingCaptureStop::none;
RecorderStopReason pendingRecorderStopReason = RecorderStopReason::none;
bool pendingCaptureForceAbort = false;
bool pendingCaptureNotifyUser = false;
bool pendingRecorderResultNotify = false;
bool pendingRecorderFinalize = false;
bool recorderHardwareReady = false;
bool recorderRecoveryFailureReported = false;

enum class StorageBootPhase : uint8_t {
  localRecovery,
  recorder,
  library,
  transcription,
  usbLink,
  wirelessLink,
  ready,
};

enum class LocalOperationPresentation : uint8_t {
  none,
  archive,
  unarchive,
  trash,
  restore,
  undoTrash,
  purge,
  bulk,
};

StorageBootPhase storageBootPhase = StorageBootPhase::localRecovery;
LocalOperationPresentation localOperationPresentation =
    LocalOperationPresentation::none;
bool storageBootAvailable = false;
bool bootUsbStarted = false;
bool bootCaptureTaskStarted = false;
bool bootSyncIdentityStarted = false;
bool bootRecorderStarted = false;
bool bootCapsuleLibraryStarted = false;
bool bootCapsuleLibraryBeginAttempted = false;
bool bootTencentWorkerStarted = false;
bool bootWifiStarted = false;
bool bootUsbLinkStarted = false;
bool bootWifiSyncStarted = false;

ServiceQuiescenceFacts serviceQuiescenceFacts(bool asrQuiesced) {
  return {
      asrQuiesced,
      captureRuntime.running(),
      pendingCaptureStop != PendingCaptureStop::none,
      localRecordingStart.active() || recorder.operationActive() || pendingRecorderFinalize,
      audio.playbackCleanupPending(),
      !StorageCoordinator::instance().idle(),
  };
}

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
  input.audioActive = audio.active() || localRecordingStart.active() ||
      recorder.operationActive() ||
      audio.playing() || captureRuntime.running() ||
      audio.playbackCleanupPending();
  const bool linkLeaseActive = linkService.receivingBinary() ||
      linkService.maintenanceActive() || wirelessSync.linkBusy() ||
      wirelessSync.openWindow() || capsuleLibrary.startupActive() ||
      capsuleLibrary.scanActive() ||
      capsuleLibrary.scanRequested() || capsuleOperations.sleepBlocker();
  const PowerFacts facts = {
      usb.tinyUsbMounted(),
      usb.cdcSessionActive(),
      board.status().vbusPresent,
      board.status().charging,
      linkLeaseActive,
      bleVoice.radioActive(),
      bleVoice.streaming(),
      wifi.radioOn() || provisioningCoordinator.ownsWifi(),
      board.lowPowerWakeSourcesReady(),
      StorageCoordinator::instance().mutationActive(),
      StorageCoordinator::instance().readActive(),
  };
  input = powerInputsWithFacts(input, facts);
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

void enterDeepSleep(const PowerInputs &inputs) {
  if (!tencentWorker.quiesce(millis(), 2000,
                             TencentCancelReason::shutdown)) {
    usb.log().println(
        "{\"event\":\"deep_sleep_deferred\",\"reason\":\"asr_busy\"}");
    noteUserActivity();
    return;
  }
  (void)captureRuntime.pollFinalize(usb.log());
  (void)audio.pollPlaybackCleanup(usb.log());
  if (!servicesQuiesced(serviceQuiescenceFacts(true))) {
    usb.log().println(
        "{\"event\":\"deep_sleep_deferred\",\"reason\":\"storage_or_audio_busy\"}");
    return;
  }
  (void)board.takeTouchInterrupt();
  if (!runtimePower.armDeepSleepWakeSources(automaticWakeEnabled(),
                                             board.lowPowerWakeSourcesReady(),
                                             usb.log())) {
    powerDiagnostics.recordDeepSleepArmError(
        millis(), runtimePower.snapshot().lastError, inputs,
        board.status().batteryPercent, usb.log());
    noteUserActivity();
    return;
  }
  powerDiagnostics.recordDeepSleepIntent(
      millis(), inputs, runtimePower.snapshot().deepSleepWakeMask,
      board.status().batteryPercent, usb.log());
  wifi.prepareForSleep();
  bleVoice.prepareForDeepSleep();
  audio.stopHardware(usb.log());
  if (board.sdReady()) SD_MMC.end();
  board.prepareForDeepSleep(
      runtimePower.snapshot().deepSleepTouchWakeArmed, usb.log());
  runtimePower.startDeepSleep(usb.log());
}

void requestSafeShutdown(uint32_t nowMs = millis()) {
  localRecordingStart.requestCancel();
  safeShutdownQuiesce.request();
  if (nextSafeShutdownAttemptMs == 0) nextSafeShutdownAttemptMs = nowMs;
}

bool advanceSafeShutdown(uint32_t nowMs) {
  if (!safeShutdownQuiesce.pending() ||
      static_cast<int32_t>(nowMs - nextSafeShutdownAttemptMs) < 0) {
    return false;
  }
  const bool asrQuiesced = tencentWorker.quiesce(
      nowMs, 2000, TencentCancelReason::shutdown);
  (void)captureRuntime.pollFinalize(usb.log());
  (void)audio.pollPlaybackCleanup(usb.log());
  const bool localQuiesced = servicesQuiesced(
      serviceQuiescenceFacts(asrQuiesced));
  usb.log().printf(
      "{\"event\":\"shutdown_asr_quiesce\",\"ok\":%s}\n",
      asrQuiesced ? "true" : "false");
  const SafeShutdownProgress progress =
      safeShutdownQuiesce.update(localQuiesced);
  if (progress != SafeShutdownProgress::ready) {
    nextSafeShutdownAttemptMs = millis() + 100;
    usb.log().println(
        "{\"event\":\"safe_shutdown_deferred\",\"reason\":\"service_or_storage_busy\",\"storage_mounted\":true}");
    return false;
  }
  nextSafeShutdownAttemptMs = 0;
  const PowerInputs inputs = currentPowerInputs();
  powerDiagnostics.recordSafeShutdown(
      millis(), inputs, board.status().batteryPercent, usb.log());
  wifi.prepareForSleep();
  bleVoice.prepareForDeepSleep();
  audio.stopHardware(usb.log());
  if (board.sdReady()) SD_MMC.end();
  board.safeShutdown(usb.log());
  return true;
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

bool submitLocalCapsuleOperation(
    CapsuleOperationAction action, const std::vector<String> &ids,
    LocalOperationPresentation presentation,
    const String &changedAt = String()) {
  if (!capsuleOperations.submit(action, ids, changedAt)) {
    showMessage(capsuleOperations.mutationCapabilityBlocked()
                    ? "本地操作恢复失败，请连接 Mac"
                    : "本地操作忙，请稍后再试");
    return false;
  }
  pendingLocalOperationIds = ids;
  localOperationPresentation = presentation;
  showMessage("正在安全处理…", 3000);
  dashboard.invalidate();
  return true;
}

void restoreRecentTrash() {
  const std::vector<std::string> pending = trashUndo.pendingIds();
  std::vector<String> ids;
  ids.reserve(pending.size());
  for (const std::string &id : pending) ids.emplace_back(id.c_str());
  (void)submitLocalCapsuleOperation(
      CapsuleOperationAction::restore, ids,
      LocalOperationPresentation::undoTrash);
}

void consumeLocalOperationOutcome() {
  CapsuleOperationOutcome outcome;
  if (!capsuleOperations.takeOutcome(outcome)) return;
  const LocalOperationPresentation presentation = localOperationPresentation;
  localOperationPresentation = LocalOperationPresentation::none;
  if (outcome.committed) {
    if (presentation == LocalOperationPresentation::undoTrash) {
      const std::vector<std::string> noFailures;
      const CapsuleUndoResult restored = trashUndo.finishAttempt(noFailures);
      showMessage(String("已恢复 ") + restored.restored + " 条");
    } else if (presentation == LocalOperationPresentation::purge) {
      showMessage(String("已永久删除 ") + outcome.changed + " 条");
    } else if (presentation == LocalOperationPresentation::trash) {
      armTrashUndo(pendingLocalOperationIds);
    } else if (presentation == LocalOperationPresentation::archive) {
      showMessage("已归档");
    } else if (presentation == LocalOperationPresentation::unarchive) {
      showMessage("已移回原目录");
    } else if (presentation == LocalOperationPresentation::restore) {
      showMessage("已恢复");
    } else {
      showMessage(String("已处理 ") + outcome.changed + " 条");
    }
  } else if (outcome.rollbackFailed || outcome.authorityPreserved) {
    showMessage("操作中断，已保留恢复记录，请连接 Mac", 5000);
  } else {
    showMessage("操作失败，已恢复");
  }
  pendingLocalOperationIds.clear();
  dashboard.closeOverlays();
  dashboard.clearCapsuleSelection();
  dashboard.invalidate();
}

void drawDashboard() {
  if (!board.status().screenOn) return;
  const uint32_t now = millis();
  DashboardView view;
  view.board = &board.status();
  view.capsuleLibraryReady =
      capabilities.ready(DeviceCapability::capsuleLibrary);
  view.library = view.capsuleLibraryReady ? &capsuleLibrary : nullptr;
  view.settings = &deviceConfig.settings();
  view.audioReady = audio.ready();
  view.localCapsulesReady = capabilities.allows(kRecordingCapabilities);
  view.recorderReady = capabilities.ready(DeviceCapability::recording);
  view.transcriptionReady =
      capabilities.ready(DeviceCapability::transcription);
  view.bleVoiceServiceReady = capabilities.ready(DeviceCapability::bleVoice);
  view.linkReady = capabilities.ready(DeviceCapability::link);
  view.wifiServiceReady = capabilities.ready(DeviceCapability::wifi);
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

bool requestCaptureStop(PendingCaptureStop owner, RecorderStopReason reason,
                        bool forceAbort, bool notifyUser);

bool startWirelessHold() {
  if (!capabilities.allows(kBleVoiceCapabilities)) {
    showMessage("无线语音服务未就绪");
    drawDashboard();
    return false;
  }
  if (!bleVoice.appReady()) {
    showMessage(bleVoice.connected() ? "蓝牙连接质量不足" : "等待 Mac 应用");
    drawDashboard();
    return false;
  }
  if (audio.playing()) audio.stopPlayback(usb.log());
  if (!captureRouter.acquire(AudioCaptureOwner::wirelessVoice)) {
    showMessage("无线麦克风暂时不可用");
    drawDashboard();
    return false;
  }
  uint32_t sessionId = esp_random();
  if (sessionId == 0) sessionId = 1;
  if (!captureRuntime.start(audio, sessionId, usb.log())) {
    captureRouter.release(AudioCaptureOwner::wirelessVoice);
    showMessage("无线麦克风暂时不可用");
    drawDashboard();
    return false;
  }
  if (!bleVoice.startSession(sessionId, millis(), captureRouter)) {
    (void)requestCaptureStop(PendingCaptureStop::wirelessVoice,
                             RecorderStopReason::none, true, false);
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

AudioCaptureDispatchResult drainCapturedAudio(uint32_t nowMs) {
  return captureDispatcher.drain(captureRuntime, captureRouter, audio, recorder,
                                 bleVoice, usb.log(), nowMs);
}

void observeCaptureMetrics() {
  const AudioCaptureFrontEndSnapshot snapshot =
      captureRuntime.frontEndSnapshot();
  if (snapshot.sessionId == 0 || snapshot.generation == 0) return;
  // Recorder finalization runs on the storage task. Publish the final metrics
  // before stop() crosses that ownership boundary, then keep the snapshot
  // immutable until the terminal outcome is consumed.
  if (recorder.recording()) {
    recorder.observeAudioMetrics(snapshot.sessionId, snapshot.generation,
                                 snapshot.asMetrics());
    if (recorder.ownedBy(RecorderOperationOwner::localApp)) {
      lastLocalCaptureMetrics = snapshot;
    }
  }
}

bool consumeRecorderTerminal(bool notifyUser);

void finishLocalRecordingStartFailure(bool notifyUser) {
  pendingRecorderFinalize = recorder.operationActive();
  pendingRecorderResultNotify = pendingRecorderResultNotify || notifyUser;
  if (pendingRecorderFinalize) return;
  captureRouter.release(AudioCaptureOwner::localCapsule);
  const bool terminalConsumed =
      consumeRecorderTerminal(pendingRecorderResultNotify);
  pendingRecorderResultNotify = false;
  if (notifyUser && !terminalConsumed) showMessage("录音启动失败");
}

void advanceLocalRecordingStart() {
  if (!localRecordingStart.active()) return;
  const RecorderStartPollResult result = recorder.pollStart(
      usb.log(), millis(), &localRecordingStartGate);
  uint32_t captureSessionId = 0;
  const LocalRecordingStartAction action =
      localRecordingStart.observe(result, captureSessionId);
  if (action == LocalRecordingStartAction::idle ||
      action == LocalRecordingStartAction::waiting) {
    return;
  }
  if (action == LocalRecordingStartAction::startCapture &&
      captureRuntime.start(audio, captureSessionId, usb.log())) {
    audio.resetPeakWindow();
    transientMessage = "";
    transientUntilMs = 0;
    dashboard.invalidate();
    return;
  }
  if (recorder.recording()) recorder.abortCapture(usb.log());
  finishLocalRecordingStartFailure(true);
  dashboard.invalidate();
}

bool finishPendingCaptureStop() {
  if (pendingCaptureStop == PendingCaptureStop::none ||
      captureRuntime.running()) {
    return pendingCaptureStop == PendingCaptureStop::none;
  }
  const AudioCaptureDispatchResult dispatch = drainCapturedAudio(millis());
  observeCaptureMetrics();
  const bool complete = dispatch.ok && !captureRuntime.incomplete() &&
      !pendingCaptureForceAbort;
  const PendingCaptureStop owner = pendingCaptureStop;
  const bool notifyUser = pendingCaptureNotifyUser;
  const RecorderStopReason reason = pendingRecorderStopReason;
  pendingCaptureStop = PendingCaptureStop::none;
  pendingCaptureForceAbort = false;
  pendingCaptureNotifyUser = false;
  pendingRecorderStopReason = RecorderStopReason::none;

  if (owner == PendingCaptureStop::wirelessVoice) {
    if (complete) bleVoice.endSession();
    else bleVoice.abortSession(VoiceSessionError::notifyFailed);
    captureRouter.release(AudioCaptureOwner::wirelessVoice);
    wirelessUiActive = false;
    dashboard.invalidate();
    return complete;
  }

  if (recorder.recording()) {
    if (complete) recorder.stop(usb.log(), reason);
    else recorder.abortCapture(usb.log());
  }
  pendingRecorderFinalize = recorder.operationActive();
  pendingRecorderResultNotify = pendingRecorderResultNotify || notifyUser;
  if (!pendingRecorderFinalize) {
    captureRouter.release(AudioCaptureOwner::localCapsule);
    (void)consumeRecorderTerminal(pendingRecorderResultNotify);
    pendingRecorderResultNotify = false;
  }
  dashboard.invalidate();
  return complete;
}

bool requestCaptureStop(PendingCaptureStop owner, RecorderStopReason reason,
                        bool forceAbort, bool notifyUser) {
  if (pendingCaptureStop != PendingCaptureStop::none &&
      pendingCaptureStop != owner) {
    return false;
  }
  pendingCaptureStop = owner;
  pendingRecorderStopReason = reason;
  pendingCaptureForceAbort = pendingCaptureForceAbort || forceAbort;
  pendingCaptureNotifyUser = pendingCaptureNotifyUser || notifyUser;
  const bool stopped = captureRuntime.stop(usb.log());
  if (stopped) return finishPendingCaptureStop();
  return false;
}

void pollDeferredServiceCleanup() {
  // Publish the realtime owner's coherent diagnostics before Link/UI status
  // handling. This also feeds Link-owned recordings without duplicating DSP.
  observeCaptureMetrics();
  advanceLocalRecordingStart();
  if (recorder.recoveryPending()) {
    (void)recorder.pollFinalize(usb.log(), millis(), nullptr);
  }
  if (recorder.takeRecoveryReady()) {
    capabilities.record(DeviceCapability::recording, recorderHardwareReady);
    dashboard.invalidate();
  }
  if (recorder.recoveryFailed() && !recorderRecoveryFailureReported) {
    recorderRecoveryFailureReported = true;
    capabilities.record(DeviceCapability::recording, false);
    usb.log().println(
        "{\"event\":\"recording_capability_degraded\",\"reason\":\"recovery_failed\"}");
    dashboard.invalidate();
  }
  // Local capture only enqueues PCM on the UI loop. The persistent recorder
  // storage task owns WAV writes, flushes, checkpoints and finalize I/O; this
  // poll publishes time/cancellation state and never performs physical I/O.
  if (recorder.recording() &&
      recorder.ownedBy(RecorderOperationOwner::localApp)) {
    (void)recorder.pollFinalize(usb.log(), millis(), nullptr);
  }
  const bool captureFinalized = captureRuntime.pollFinalize(usb.log());
  if (captureFinalized && pendingCaptureStop != PendingCaptureStop::none) {
    (void)finishPendingCaptureStop();
  }
  if (pendingRecorderFinalize &&
      recorder.ownedBy(RecorderOperationOwner::localApp)) {
    (void)recorder.pollFinalize(usb.log(), millis(), nullptr);
  }
  if (pendingRecorderFinalize && !recorder.operationActive()) {
    pendingRecorderFinalize = false;
    captureRouter.release(AudioCaptureOwner::localCapsule);
    const bool notifyUser = pendingRecorderResultNotify;
    pendingRecorderResultNotify = false;
    (void)consumeRecorderTerminal(notifyUser);
  }
  (void)audio.pollPlaybackCleanup(usb.log());
}

bool stopWirelessHold() {
  const bool captureStopped = requestCaptureStop(
      PendingCaptureStop::wirelessVoice, RecorderStopReason::none,
      false, false);
  noteUserActivity();
  transientMessage = "";
  transientUntilMs = 0;
  drawDashboard();
  return captureStopped;
}

bool stopLocalCapture(RecorderStopReason reason) {
  if (!recorder.ownedBy(RecorderOperationOwner::localApp)) return false;
  return requestCaptureStop(PendingCaptureStop::localCapsule, reason,
                            false, true);
}

bool consumeRecorderTerminal(bool notifyUser) {
  RecorderOutcome outcome;
  if (!recorder.takeTerminalResult(outcome)) return false;
  const bool completed = outcome.success();
  bool indexed = completed;
  bool refreshQueued = false;
  if (completed) {
    if (!capsuleLibrary.scanActive() && !capsuleLibrary.scanRequested()) {
      indexed = capsuleLibrary.includeInboxCapsule(recorder.capsuleId());
    } else {
      indexed = false;
    }
    if (!indexed) refreshQueued = capsuleLibrary.requestScan();
  }
  if (notifyUser) {
    const char *message = "录音失败，内容未提交";
    if (completed) {
      message = indexed ? "胶囊已进入转写队列"
                        : (refreshQueued ? "胶囊已保存，列表刷新中"
                                         : "胶囊已保存，列表刷新失败");
    } else if (outcome.failureStage ==
               RecorderFailureStage::insufficientSpace) {
      message = "存储空间不足";
    } else if (outcome.failureStage == RecorderFailureStage::capacityUnknown ||
               outcome.failureStage == RecorderFailureStage::capacityInvalid) {
      message = "无法读取存储容量";
    } else if (outcome.failureStage == RecorderFailureStage::storageTooSlow) {
      message = "存储卡性能不足";
    } else if (outcome.failureStage == RecorderFailureStage::storageBusy) {
      message = outcome.terminal == RecorderTerminal::cancelled
          ? "录音启动已取消" : "存储服务忙，请稍后再试";
    }
    showMessage(message);
  }
  const AudioFrontEndMetrics captureMetrics =
      lastLocalCaptureMetrics.asMetrics();
  usb.log().printf(
      "{\"event\":\"recording_result_consumed\",\"completed\":%s,\"terminal\":%u,\"stage\":\"%s\",\"bytes\":%lu,\"capture_session_id\":%lu,\"capture_metrics_generation\":%lu,\"audio_frontend_channel\":\"%s\",\"audio_frontend_left_peak\":%u,\"audio_frontend_right_peak\":%u,\"audio_frontend_output_peak\":%u,\"audio_frontend_noise_floor\":%u,\"audio_frontend_suppressed_samples\":%lu,\"audio_frontend_limited_samples\":%lu,\"audio_frontend_max_gain_q12\":%lu}\n",
      completed ? "true" : "false", static_cast<unsigned>(outcome.terminal),
      recorderFailureStageName(outcome.failureStage),
      static_cast<unsigned long>(outcome.dataBytes),
      static_cast<unsigned long>(lastLocalCaptureMetrics.sessionId),
      static_cast<unsigned long>(lastLocalCaptureMetrics.generation),
      audioInputChannelName(captureMetrics.selectedChannel),
      captureMetrics.leftPeak, captureMetrics.rightPeak,
      captureMetrics.outputPeak, captureMetrics.estimatedNoiseFloor,
      static_cast<unsigned long>(captureMetrics.suppressedSamples),
      static_cast<unsigned long>(captureMetrics.limitedSamples),
      static_cast<unsigned long>(captureMetrics.maximumGainQ12));
  return true;
}

void toggleRecording() {
  if (localRecordingStart.active()) {
    localRecordingStart.requestCancel();
    showMessage("正在取消录音…");
  } else if (recorder.recording() &&
      recorder.ownedBy(RecorderOperationOwner::localApp)) {
    stopLocalCapture(RecorderStopReason::user);
  } else if (recorder.operationActive() || pendingRecorderFinalize) {
    showMessage("上一段录音仍在保存");
  } else if (tencentWorker.working()) {
    showMessage("当前胶囊正在转写");
  } else if (!capabilities.allows(kRecordingCapabilities)) {
    showMessage(board.sdReady() ? "录音服务未就绪" : "请插入 microSD 卡");
  } else if (!board.sdReady()) {
    showMessage("请插入 microSD 卡");
  } else if (!audio.ready()) {
    showMessage("麦克风尚未就绪");
  } else {
    if (audio.playing()) audio.stopPlayback(usb.log());
    tencentWorker.wake();
    const bool acquired = captureRouter.acquire(AudioCaptureOwner::localCapsule);
    uint32_t captureSessionId = esp_random();
    if (captureSessionId == 0) captureSessionId = 1;
    lastLocalCaptureMetrics = {};
    const bool stateReady = acquired &&
        localRecordingStart.begin(captureSessionId);
    const bool requested = stateReady &&
        recorder.requestStart(usb.log(), recordingId(), board.utcNow(),
                              RecorderOperationOwner::localApp);
    if (requested) {
      showMessage("正在检查存储…", 3000);
    } else {
      localRecordingStart.reset();
      finishLocalRecordingStartFailure(true);
    }
  }
  noteUserActivity();
  drawDashboard();
}

void emitStatus() {
  const BoardStatus &s = board.status();
  const BleVoiceQualitySnapshot quality = bleVoice.quality();
  const RuntimePowerSnapshot &power = runtimePower.snapshot();
  const AudioCaptureFrontEndSnapshot captureSnapshot =
      captureRuntime.frontEndSnapshot();
  const AudioFrontEndMetrics frontEnd = captureSnapshot.asMetrics();
  usb.log().printf(
      "{\"event\":\"status\",\"variant\":\"%s\",\"display\":%s,\"touch\":%s,\"sd\":%s,\"audio\":%s,\"audio_active\":%s,\"usb\":%s,\"host_connected\":%s,\"ble_voice_connected\":%s,\"ble_voice_ready\":%s,\"ble_voice_mtu\":%u,\"ble_voice_streaming\":%s,\"ble_voice_notify_attempts\":%lu,\"ble_voice_notify_accepted\":%lu,\"ble_voice_notify_failures\":%lu,\"ble_voice_queue_overflows\":%lu,\"ble_voice_session_failures\":%lu,\"ble_voice_ready_timeouts\":%lu,\"ble_voice_stop_ack_timeouts\":%lu,\"ble_voice_stream_timeouts\":%lu,\"ble_voice_last_error_code\":%u,\"audio_read_bytes\":%llu,\"audio_read_failures\":%lu,\"audio_peak\":%u,\"audio_capture_session_id\":%lu,\"audio_capture_metrics_generation\":%lu,\"audio_capture_active\":%s,\"audio_frontend_channel\":\"%s\",\"audio_frontend_left_peak\":%u,\"audio_frontend_right_peak\":%u,\"audio_frontend_output_peak\":%u,\"audio_frontend_noise_floor\":%u,\"audio_frontend_suppressed_samples\":%lu,\"audio_frontend_limited_samples\":%lu,\"audio_frontend_max_gain_q12\":%lu,\"recording\":%s,\"duration_ms\":%lu,\"battery\":%d,\"charging\":%s,\"vbus\":%s,\"wifi\":\"%s\",\"wifi_rssi\":%ld,\"wifi_radio_on\":%s,\"wifi_power_save\":%s,\"pending_capsules\":%u,\"tencent_configured\":%s,\"transcribing\":%s,\"power_mode\":\"%s\",\"cpu_mhz\":%u,\"light_sleep_count\":%lu,\"light_sleep_us\":%llu,\"deep_sleep_wake_count\":%lu,\"woke_from_deep_sleep\":%s,\"deep_sleep_touch_wake\":%s,\"critical_battery\":%s,\"idle_ms\":%lu,\"last_wake_cause\":%u,\"reset_reason\":%u,\"internal_heap_free\":%u,\"internal_heap_largest\":%u,\"psram_free\":%u,\"automatic_pm_supported\":%s,\"ble_modem_sleep_supported\":%s,\"provisioning_startup_phase\":\"%s\",\"provisioning_diagnostic_count\":%u}\n",
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
      static_cast<unsigned long>(captureSnapshot.sessionId),
      static_cast<unsigned long>(captureSnapshot.generation),
      captureSnapshot.active ? "true" : "false",
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
      const PowerInputs inputs = currentPowerInputs(now);
      powerDiagnostics.recordAutomaticScreenWake(
          now, AutomaticScreenWakeSource::touchInterrupt, inputs,
          board.status().batteryPercent, usb.log());
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
        touchGesture.verticalSwipe() &&
        capabilities.allows(kCapsuleBrowsingCapabilities)) {
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
               capabilities.allows(kCapsuleBrowsingCapabilities) &&
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
    if (uiActionRequiresCapsuleLibrary(action) &&
        !capabilities.allows(kCapsuleBrowsingCapabilities)) {
      showMessage("本地胶囊不可用");
      drawDashboard();
      return;
    }
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
      if (!capabilities.allows(kWifiCapabilities)) {
        showMessage("Wi-Fi 服务未就绪");
        drawDashboard();
        return;
      }
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
      if (!capabilities.ready(DeviceCapability::bleVoice)) {
        showMessage("蓝牙服务未就绪");
        drawDashboard();
        return;
      }
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
      if (!capabilities.allows(kComputerSyncCapabilities)) {
        showMessage("电脑同步服务未就绪");
        drawDashboard();
        return;
      }
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
      if (!capabilities.allows(kWifiCapabilities)) {
        showMessage("Wi-Fi 服务未就绪");
        drawDashboard();
        return;
      }
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
      if (submitLocalCapsuleOperation(
              CapsuleOperationAction::purge, pendingPurgeIds,
              LocalOperationPresentation::purge)) {
        pendingPurgeIds.clear();
        dashboard.closeOverlays();
        dashboard.clearCapsuleSelection();
        if (dashboard.state().screen() == UiScreen::capsuleDetail) {
          dashboard.back();
        }
      }
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
      if (action == UiAction::bulkFavorite) {
        const CapsuleBatchResult result = capsuleLibrary.batch(
            ids, CapsuleBatchAction::favorite, board.utcNow());
        dashboard.clearCapsuleSelection();
        showMessage(result.ok ? String("已处理 ") + result.changed + " 条"
                              : "批量收藏未完成");
      } else {
        CapsuleOperationAction operation = CapsuleOperationAction::archive;
        LocalOperationPresentation presentation =
            LocalOperationPresentation::bulk;
        if (action == UiAction::bulkArchive) {
          operation = scope == CapsuleScope::trash
              ? CapsuleOperationAction::restore
              : (scope == CapsuleScope::archive
                     ? CapsuleOperationAction::unarchive
                     : CapsuleOperationAction::archive);
        } else {
          operation = scope == CapsuleScope::trash
              ? CapsuleOperationAction::restore
              : CapsuleOperationAction::trash;
          presentation = scope == CapsuleScope::trash
              ? LocalOperationPresentation::bulk
              : LocalOperationPresentation::trash;
        }
        if (submitLocalCapsuleOperation(
                operation, ids, presentation, board.utcNow())) {
          dashboard.clearCapsuleSelection();
        }
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
        std::vector<String> ids{id};
        const CapsuleOperationAction operation = wasTrashed
            ? CapsuleOperationAction::restore
            : (wasArchived ? CapsuleOperationAction::unarchive
                           : CapsuleOperationAction::archive);
        const LocalOperationPresentation presentation = wasTrashed
            ? LocalOperationPresentation::restore
            : (wasArchived ? LocalOperationPresentation::unarchive
                           : LocalOperationPresentation::archive);
        if (submitLocalCapsuleOperation(operation, ids, presentation,
                                        board.utcNow())) {
          dashboard.back();
        }
      } else if (action == UiAction::trash) {
        std::vector<String> ids{id};
        if (submitLocalCapsuleOperation(
                CapsuleOperationAction::trash, ids,
                LocalOperationPresentation::trash, board.utcNow())) {
          dashboard.closeOverlays();
          dashboard.back();
        }
      } else if (action == UiAction::retry) {
        if (!capabilities.allows(kTranscriptionCapabilities)) {
          showMessage("转写服务未就绪");
          drawDashboard();
          return;
        }
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
        } else if (!capabilities.allows(kPlaybackCapabilities)) {
          showMessage("播放服务未就绪");
        } else if (!captureRouter.available() || recorder.recording()) {
          showMessage("麦克风使用中，暂时无法播放");
        } else if (tencentWorker.working()) {
          showMessage("正在转写，完成后可播放");
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

bool advanceStorageBoot(uint32_t nowMs) {
  switch (storageBootPhase) {
    case StorageBootPhase::localRecovery:
      capsuleOperations.poll(nowMs);
      if (capsuleOperations.recoveryActive()) return false;
      storageBootPhase = StorageBootPhase::recorder;
      return false;
    case StorageBootPhase::recorder:
      bootRecorderStarted = board.sdReady() &&
          recorder.begin(SD_MMC, recordingCapacitySource, usb.log());
      recorderHardwareReady =
          bootRecorderStarted && bootCaptureTaskStarted;
      storageBootPhase = StorageBootPhase::library;
      return false;
    case StorageBootPhase::library:
      if (!bootCapsuleLibraryBeginAttempted) {
        bootCapsuleLibraryBeginAttempted = true;
        bootCapsuleLibraryStarted = board.sdReady() && capsuleLibrary.begin(
            SD_MMC, usb.log(),
            !capsuleOperations.mutationCapabilityBlocked());
        if (!bootCapsuleLibraryStarted) {
          capabilities.record(DeviceCapability::capsuleLibrary, false);
          storageBootPhase = StorageBootPhase::transcription;
        }
        return false;
      }
      if (capsuleLibrary.startupActive()) {
        (void)capsuleLibrary.pollStartup(nowMs);
        return false;
      }
      bootCapsuleLibraryStarted = capsuleLibrary.startupReady();
      capabilities.record(DeviceCapability::capsuleLibrary,
                          bootCapsuleLibraryStarted);
      lastCapsuleLibraryRevision = capsuleLibrary.revision();
      storageBootPhase = StorageBootPhase::transcription;
      return false;
    case StorageBootPhase::transcription:
      bootTencentWorkerStarted = bootCapsuleLibraryStarted &&
          tencentWorker.begin(SD_MMC, capsuleLibrary, deviceConfig, usb.log());
      capabilities.record(DeviceCapability::transcription,
                          bootTencentWorkerStarted);
      storageBootPhase = StorageBootPhase::usbLink;
      return false;
    case StorageBootPhase::usbLink:
      bootUsbLinkStarted = linkService.begin(
          usb.stream(), SD_MMC, board, audio, captureRouter, usb, bleVoice,
          dashboard, capsuleLibrary, recorder, deviceConfig, wifi,
          tencentWorker, provisioningDiagnostics, powerDiagnostics,
          runtimePower, usb.log(), &linkCoordinator,
          LinkTransport::usb, &wirelessSync,
                    nullptr, &provisioningCoordinator, nullptr,
          &captureRuntime, &captureDispatcher, &capabilities);
      storageBootPhase = StorageBootPhase::wirelessLink;
      return false;
    case StorageBootPhase::wirelessLink:
      bootWifiSyncStarted = wirelessSync.begin(
          SD_MMC, board, audio, captureRouter, usb, bleVoice, dashboard,
          capsuleLibrary, recorder, deviceConfig, wifi, tencentWorker,
          provisioningDiagnostics, powerDiagnostics, runtimePower,
          wirelessSyncIdentity, linkCoordinator, usb.log(), &captureRuntime,
          &captureDispatcher, &capabilities);
      capabilities.record(
          DeviceCapability::link,
          bootUsbStarted && bootUsbLinkStarted && bootSyncIdentityStarted &&
              bootWifiSyncStarted);
      storageBootPhase = StorageBootPhase::ready;
      break;
    case StorageBootPhase::ready:
      return true;
  }
  const StartupCapabilityPresentation startup =
      startupCapabilityPresentation(capabilities);
  usb.log().printf(
      "{\"event\":\"boot_capabilities\",\"observed_mask\":%u,"
      "\"ready_mask\":%u,\"missing_mask\":%u,\"all_ready\":%s,"
      "\"capsule_library\":%s,\"recording\":%s,"
      "\"transcription\":%s,\"local_operation_phase\":\"%s\","
      "\"startup_mode\":\"%s\"}\n",
      static_cast<unsigned>(capabilities.observedMask()),
      static_cast<unsigned>(capabilities.readyMask()),
      static_cast<unsigned>(capabilities.missingMask()),
      capabilities.allReady() ? "true" : "false",
      bootCapsuleLibraryStarted ? "true" : "false",
      capabilities.ready(DeviceCapability::recording) ? "true" : "false",
      bootTencentWorkerStarted ? "true" : "false",
      capsuleOperations.phaseName(), startupCapabilityModeName(startup.mode));
  showMessage(startup.message, 4000);
  dashboard.invalidate();
  drawDashboard();
  emitStatus();
  return true;
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  pinMode(kBootButtonPin, INPUT_PULLUP);

  board.begin(Serial);
  const BoardStatus &bootBoard = board.status();
  capabilities.record(DeviceCapability::display, bootBoard.display);
  capabilities.record(DeviceCapability::touch, bootBoard.touch);
  capabilities.record(DeviceCapability::storage, bootBoard.sdCard);
  capabilities.record(DeviceCapability::rtc, bootBoard.rtc);
  capabilities.record(DeviceCapability::imu, bootBoard.imu);
  capabilities.record(DeviceCapability::pmu, bootBoard.pmu);
  beginTlsExternalMemory(Serial);
  provisioningDiagnostics.begin(
      Serial, static_cast<uint16_t>(esp_reset_reason()));
  const bool audioStarted = audio.begin(board.status().variant, Serial);
  capabilities.record(DeviceCapability::audio, audioStarted);
  bootCaptureTaskStarted = audioStarted &&
      captureRuntime.begin(board.status().variant, Serial);
  bootUsbStarted = usb.begin(board.status().variant);
  runtimePower.begin(usb.log());
  const RuntimePowerSnapshot &bootPower = runtimePower.snapshot();
  powerDiagnostics.begin(
      usb.log(), static_cast<uint16_t>(esp_reset_reason()),
      bootPower.lastWakeCause, bootPower.wakeCauses,
      bootPower.ext1WakeMask, bootPower.automaticPmSupported,
      bootPower.bleModemSleepSupported, board.status().batteryPercent);
  const bool bleStarted = bleVoice.begin(deviceId(), usb.log());
  capabilities.record(DeviceCapability::bleVoice,
                      bleStarted && bootCaptureTaskStarted);
  deviceConfig.begin(usb.log());
  bootSyncIdentityStarted =
      wirelessSyncIdentity.begin(ESP.getEfuseMac(), usb.log());
  storageBootAvailable = board.sdReady() &&
      capsuleOperations.begin(SD_MMC, usb.log());
  if (storageBootAvailable) capsuleOperations.attachCatalog(capsuleLibrary);
  storageBootPhase = storageBootAvailable
      ? StorageBootPhase::localRecovery : StorageBootPhase::recorder;
  capabilities.record(DeviceCapability::capsuleLibrary, false);
  recorderHardwareReady = false;
  capabilities.record(DeviceCapability::recording, false);
  capabilities.record(DeviceCapability::transcription, false);
  bootWifiStarted = wifi.begin(deviceConfig, usb.log());
  capabilities.record(DeviceCapability::wifi, bootWifiStarted);
  provisioningCoordinator.begin(provisioningPortal, wifi, deviceConfig,
                                provisioningDiagnostics, usb.log());
  const StartupCapabilityPresentation startup =
      startupCapabilityPresentation(capabilities);
  dashboard.begin(board.display(), board.sdReady() ? &SD_MMC : nullptr);
  if (provisioningDiagnostics.recoveredInterruptedSession()) {
    showMessage("上次配网被重启中断 · 见诊断", 5000);
  } else {
    showMessage(storageBootAvailable ? "正在恢复本地胶囊…" : startup.message,
                4000);
  }
  drawDashboard();
  autoScreenOff.begin(millis());
  lastUsbHostConnected = usb.hostConnected();
  lastVbusPresent = board.status().vbusPresent;
  emitStatus();
}

void loop() {
  const uint32_t now = millis();
  // Storage boot is a real application phase. One cooperative recovery step
  // runs per turn; the initial library scan, ASR queue, Link and every local
  // mutation remain unopened until this phase reaches a terminal boundary.
  if (!advanceStorageBoot(now)) {
    if (now - lastDashboardMs >= 1000) {
      lastDashboardMs = now;
      drawDashboard();
    }
    return;
  }
  capsuleOperations.poll(now);
  consumeLocalOperationOutcome();
  // These polls precede every transport/UI early return. Physical File close
  // and late capture finalization therefore always make bounded progress.
  pollDeferredServiceCleanup();
  if (safeShutdownQuiesce.pending()) (void)advanceSafeShutdown(now);
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
        if (provisioningCoordinator.sensitiveConfirmationPending()) {
          bootProvisioningExitArmed = false;
          bootProvisioningConfirmationConsumed = true;
          const bool confirmed =
              provisioningCoordinator.confirmSensitiveChange(now);
          dashboard.invalidate();
          showMessage(confirmed ? "已确认腾讯密钥操作"
                                : "实体确认超时，请再次保存");
          drawDashboard();
          return;
        }
        bootProvisioningExitArmed = true;
        return;
      }
      bootPressedAtMs = now;
    } else if (bootButton.releasedEdge()) {
      if (bootProvisioningConfirmationConsumed) {
        bootProvisioningConfirmationConsumed = false;
        return;
      }
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
  } else if (captureRuntime.running()) {
    const bool wasRecording = recorder.recording();
    const AudioCaptureDispatchResult dispatch = drainCapturedAudio(now);
    if (!dispatch.ok) {
      if (dispatch.failedRecorderOwner == RecorderOperationOwner::localApp) {
        (void)requestCaptureStop(PendingCaptureStop::localCapsule,
                                 RecorderStopReason::none, true, true);
        showMessage("录音已中断");
      } else if (dispatch.voiceDeliveryFailure ||
                 captureRouter.wirelessStreaming()) {
        (void)requestCaptureStop(PendingCaptureStop::wirelessVoice,
                                 RecorderStopReason::none, true, false);
        showMessage("无线语音已中断");
      }
      // Link-owned recorder failures are durable facts inside WavRecorder.
      // The owning LinkRecordingSession observes them in its own poll and
      // performs protocol response, rollback and resource release.
    }
    if (recorder.ownedBy(RecorderOperationOwner::localApp) &&
        recorder.stopRequested() &&
        pendingCaptureStop == PendingCaptureStop::none) {
      (void)requestCaptureStop(PendingCaptureStop::localCapsule,
                               recorder.requestedStopReason(), false, true);
      drawDashboard();
    } else if (recorder.ownedBy(RecorderOperationOwner::localApp) &&
               wasRecording && !recorder.recording() &&
               pendingCaptureStop == PendingCaptureStop::none) {
      (void)requestCaptureStop(PendingCaptureStop::localCapsule,
                               RecorderStopReason::none, true, true);
      drawDashboard();
    }
  }
  bleVoice.poll(now);
  if (wirelessUiActive && !bleVoice.streaming()) {
    (void)requestCaptureStop(PendingCaptureStop::wirelessVoice,
                             RecorderStopReason::none, true, false);
  }
  if (captureRouter.available() && !captureRuntime.running() &&
      !audio.playing() && audio.active()) {
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
  wifi.loop(now, recorder.operationActive(), networkWork,
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
  // One application turn performs at most one scan action. Starting is also
  // an action, so the first filesystem slice happens on the next turn. An ASR
  // job retains priority until its result has committed.
  if (!tencentWorker.working()) capsuleLibrary.pollScan();
  const uint32_t capsuleRevision = capsuleLibrary.revision();
  if (capsuleRevision != lastCapsuleLibraryRevision) {
    lastCapsuleLibraryRevision = capsuleRevision;
    dashboard.invalidate();
  }
  tencentWorker.loop(now, wifi.connected(), wifi.timeReady(),
                     transcriptionDispatchBusy(recorder.operationActive(),
                                               linkService.maintenanceActive() ||
                                                   wirelessSync.linkBusy() ||
                                                   capsuleLibrary.scanActive() ||
                                                   capsuleOperations.busy()) ||
                         lowBatteryShutdown.critical(),
                     board.status().charging);
  PowerInputs finalPowerInputs = currentPowerInputs(now);
  currentPowerDecision = runtimePower.apply(finalPowerInputs, usb.log());
  if (currentPowerDecision.requestIdleRadioPause) {
    (void)pauseIdleRadios();
    finalPowerInputs = currentPowerInputs(now);
    currentPowerDecision = runtimePower.apply(finalPowerInputs, usb.log());
  }
  powerDiagnostics.observe(now, finalPowerInputs, currentPowerDecision,
                           board.status().batteryPercent, usb.log());
  if (currentPowerDecision.requestSafeShutdown) requestSafeShutdown(now);
  if (safeShutdownQuiesce.pending()) (void)advanceSafeShutdown(now);
  if (!safeShutdownQuiesce.pending() &&
      currentPowerDecision.requestDeepSleep && !bleVoice.connected() &&
      !wifi.radioOn()) {
    enterDeepSleep(finalPowerInputs);
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
      const PowerInputs inputs = currentPowerInputs(now);
      powerDiagnostics.recordAutomaticScreenWake(
          now, AutomaticScreenWakeSource::motionInterrupt, inputs,
          status.batteryPercent, usb.log());
      setScreenState(true);
    } else if (raiseToWake.update(now, automaticWakeEnabled(),
                                  status.screenOn, status.accelerationX,
                                  status.accelerationY,
                                  status.accelerationZ)) {
      const PowerInputs inputs = currentPowerInputs(now);
      powerDiagnostics.recordAutomaticScreenWake(
          now, AutomaticScreenWakeSource::raiseToWakePolicy, inputs,
          status.batteryPercent, usb.log());
      setScreenState(true);
    }
    const PowerKeyEvent powerKey = board.pollPowerKey();
    if (powerKey == PowerKeyEvent::shortPress) {
      noteUserActivity(now);
      setScreenState(!board.status().screenOn);
    } else if (powerKey == PowerKeyEvent::longPress) {
      if (recorder.recording() &&
          recorder.ownedBy(RecorderOperationOwner::localApp)) {
        stopLocalCapture(RecorderStopReason::user);
        consumeRecorderTerminal(false);
      }
      if (bleVoice.streaming()) stopWirelessHold();
      if (audio.playing()) audio.stopPlayback(usb.log());
      requestSafeShutdown(now);
      (void)advanceSafeShutdown(now);
    }
  }
  const bool keepScreenAwake = localRecordingStart.active() ||
      recorder.operationActive() || wirelessUiActive ||
      audio.playing() || provisioningCoordinator.visible() ||
      capsuleOperations.busy() ||
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
      (localRecordingStart.active() || recorder.operationActive() ||
       wirelessUiActive)
          ? ui::kRecordingFrameIntervalMs : 1000;
  if (now - lastDashboardMs >= dashboardIntervalMs) {
    lastDashboardMs = now;
    drawDashboard();
  }
  if (currentPowerDecision.allowLightSleep) {
    const PowerInputs verifiedInputs = currentPowerInputs(now);
    const uint32_t attemptsBefore =
        runtimePower.snapshot().lightSleepAttempts;
    const uint32_t failuresBefore =
        runtimePower.snapshot().lightSleepFailures;
    const bool slept = runtimePower.enterLightSleep(
        verifiedInputs, currentPowerDecision, usb.log());
    const RuntimePowerSnapshot &afterSleep = runtimePower.snapshot();
    if (slept) {
      powerDiagnostics.recordLightSleepWake(
          millis(), afterSleep.lastLightSleepMs, afterSleep.lastWakeCause,
          board.status().batteryPercent, usb.log());
    } else if (afterSleep.lightSleepAttempts != attemptsBefore &&
               afterSleep.lightSleepFailures != failuresBefore) {
      powerDiagnostics.recordLightSleepError(
          millis(), afterSleep.lastError, verifiedInputs,
          board.status().batteryPercent, usb.log());
    }
    if (slept && afterSleep.lastWakeCause == ESP_SLEEP_WAKEUP_GPIO) {
      setScreenState(true);
      noteUserActivity();
    }
  }
}
