#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_heap_caps.h>
#include <esp_mac.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <new>

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
#include "DeviceRebootCoordinator.h"
#include "ProvisioningPortal.h"
#include "ProvisioningDiagnostics.h"
#include "ProvisioningCoordinator.h"
#include "PokePodLinkService.h"
#include "LinkServiceCoordinator.h"
#include "LocalRecordingStart.h"
#include "PowerPolicy.h"
#include "PowerDiagnostics.h"
#include "PsramDegradedPolicy.h"
#include "RaiseToWakePolicy.h"
#include "RuntimePowerManager.h"
#include "RuntimeDiagnostics.h"
#include "ServiceQuiescencePolicy.h"
#include "StorageCoordinator.h"
#include "TencentWorker.h"
#include "TlsExternalMemory.h"
#include "UsbLinkBridge.h"
#include "UsbLinkSessionReconcile.h"
#include "UsbPhysicalConnectionPolicy.h"
#include "WavRecorder.h"
#include "WifiController.h"
#include "WifiUiPolicy.h"
#include "WirelessSyncIdentity.h"
#include "WirelessSyncService.h"
#include "WirelessCaptureStartPolicy.h"

using namespace pokepod;

namespace {

static_assert(!psramMayFallbackToInternal(),
              "large services must fail closed when PSRAM is unavailable");

// These services carry large fixed workspaces but only run from cooperative
// application/storage tasks. Keep their objects out of DMA-capable internal
// RAM so the Wi-Fi and BLE drivers retain enough contiguous internal heap to
// coexist. The board requires OPI PSRAM; a failed allocation is a degraded
// capability state and never falls back into internal RAM.
template <typename T>
class PsramService {
 public:
  bool allocate(const char *name, Print &log) {
    if (instance_ != nullptr) return external_;
    void *memory = heap_caps_malloc(
        sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    external_ = memory != nullptr;
    if (memory == nullptr) {
      log.printf(
          "{\"event\":\"service_allocation\",\"service\":\"%s\","
          "\"ok\":false,\"degraded\":true,\"memory\":\"psram\","
          "\"bytes\":%u}\n",
          name, static_cast<unsigned>(sizeof(T)));
      return false;
    }
    instance_ = new (memory) T();
    log.printf(
        "{\"event\":\"service_allocation\",\"service\":\"%s\","
        "\"ok\":true,\"bytes\":%u,\"memory\":\"psram\"}\n",
        name, static_cast<unsigned>(sizeof(T)));
    return true;
  }

  T *operator->() { return instance_; }
  const T *operator->() const { return instance_; }
  T &get() { return *instance_; }
  const T &get() const { return *instance_; }
  bool ready() const { return instance_ != nullptr; }
  bool external() const { return external_; }

 private:
  T *instance_ = nullptr;
  bool external_ = false;
};

class SdMmcRecordingCapacitySource final : public RecordingCapacitySource {
 public:
  explicit SdMmcRecordingCapacitySource(const BoardServices &board)
      : board_(board) {}
  RecordingSpaceSnapshot query() override {
    const uint64_t totalBytes = SD_MMC.totalBytes();
    const uint64_t usedBytes = SD_MMC.usedBytes();
    return {totalBytes, usedBytes, totalBytes != 0};
  }

  uint64_t monotonicMicros() override {
    return static_cast<uint64_t>(esp_timer_get_time());
  }

  uint32_t mountGeneration() const override {
    return board_.sdMountGeneration();
  }

 private:
  const BoardServices &board_;
};

BoardServices board;
SdMmcRecordingCapacitySource recordingCapacitySource(board);
AudioPipeline audio;
AudioCaptureRouter captureRouter;
AudioCaptureRuntime captureRuntime;
AudioCaptureDispatcher captureDispatcher;
UsbLinkBridge usb;
BleVoiceService bleVoice;
WavRecorder recorder;
PsramService<CapsuleLibrary> capsuleLibrary;
PsramService<CapsuleOperationService> capsuleOperations;
Dashboard dashboard;
ButtonDebouncer bootButton;
BootGesturePolicy bootGesturePolicy;
DeviceConfig deviceConfig;
WifiController wifi;
TencentWorker tencentWorker;
ProvisioningPortal provisioningPortal;
ProvisioningDiagnostics provisioningDiagnostics;
PowerDiagnostics powerDiagnostics;
RuntimeDiagnostics runtimeDiagnostics;
ProvisioningCoordinator provisioningCoordinator;
PsramService<PokePodLinkService> linkService;
LinkServiceCoordinator linkCoordinator;
DeviceRebootCoordinator deviceReboot;
WirelessSyncIdentity wirelessSyncIdentity;
PsramService<WirelessSyncService> wirelessSync;
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
bool wirelessUiActive = false;
WirelessCaptureStartPolicy wirelessCaptureStart;
AudioSessionTelemetry wirelessSessionTelemetry;
AudioSessionTelemetryProducer wirelessTelemetryProducer;
bool wirelessTelemetryActive = false;
UiAction touchAction = UiAction::none;
uint32_t lastTouchMs = 0;
uint32_t lastDashboardMs = 0;
uint32_t lastScrollFrameMs = 0;
uint32_t lastSensorMs = 0;
AudioCaptureFrontEndSnapshot lastLocalCaptureMetrics;
uint32_t captureTelemetrySampledSessionId = 0;
uint32_t captureTelemetryLastStackSampleMs = 0;
uint32_t captureTelemetryStackHighWaterWords = 0;
bool captureTelemetryStackSampled = false;
uint32_t lastNetworkTimeSyncRevision = 0;
uint32_t lastCapsuleLibraryRevision = 0;
bool ignoreTouchUntilRelease = false;
bool lastUsbHostConnected = false;
uint32_t lastUsbSessionGeneration = 0;
bool lastVbusPresent = false;
bool screenDimmed = false;
String transientMessage;
UiNoticeKind transientMessageKind = UiNoticeKind::info;
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
bool psramDegradedBoot = false;

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
  const bool linkLeaseActive = linkService->receivingBinary() ||
      linkService->maintenanceActive() || wirelessSync->linkBusy() ||
      wirelessSync->openWindow() || capsuleLibrary->startupActive() ||
      capsuleLibrary->scanActive() ||
      capsuleLibrary->scanRequested() || capsuleOperations->sleepBlocker();
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
  input.networkBusy = tencentWorker.working() || wirelessSync->linkBusy() ||
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
  if (!bleVoice.quiescedForSleep() || wifi.radioOn()) {
    usb.log().println(
        "{\"event\":\"deep_sleep_deferred\",\"reason\":\"radio_not_quiesced\"}");
    return;
  }
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
  // Re-read callback-owned BLE facts immediately before controller teardown.
  // This prevents a caller or a late callback from bypassing the outer gate.
  if (!bleVoice.quiescedForSleep() || wifi.radioOn()) {
    usb.log().println(
        "{\"event\":\"deep_sleep_deferred\",\"reason\":\"radio_reactivated\"}");
    return;
  }
  bleVoice.prepareForDeepSleep();
  audio.stopHardware(usb.log());
  board.endSdMount();
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
  board.endSdMount();
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

void showMessage(const String &message, UiNoticeKind kind,
                 uint32_t durationMs = 1800) {
  transientMessage = message;
  transientMessageKind = kind;
  transientUntilMs = millis() + durationMs;
}

String localCapsuleStatusMessage() {
  if (!board.sdReady()) return "SD 卡不可用";
  if (capsuleOperations->mutationCapabilityBlocked()) {
    return "恢复记录失败 · 连接 Mac";
  }
  if (storageBootPhase == StorageBootPhase::localRecovery) {
    return "正在恢复本地胶囊";
  }
  if (storageBootPhase == StorageBootPhase::recorder) {
    return "正在启动录音存储";
  }
  if (storageBootPhase == StorageBootPhase::library ||
      capsuleLibrary->startupActive()) {
    return "正在恢复胶囊目录";
  }
  if (capsuleLibrary->startupBlocked()) return "胶囊目录恢复失败";
  if (!capabilities.ready(DeviceCapability::capsuleLibrary)) {
    return "胶囊目录不可用";
  }
  if (!capabilities.ready(DeviceCapability::recording)) {
    return "录音存储不可用";
  }
  return "本地胶囊不可用";
}

void armTrashUndo(const std::vector<String> &ids) {
  std::vector<std::string> stableIds;
  stableIds.reserve(ids.size());
  for (const String &id : ids) stableIds.emplace_back(id.c_str());
  trashUndo.arm(stableIds, millis());
  showMessage("已删除 · 点此撤销", UiNoticeKind::success,
              CapsuleUndoState::kDurationMs);
}

bool submitLocalCapsuleOperation(
    CapsuleOperationAction action, const std::vector<String> &ids,
    LocalOperationPresentation presentation,
    const String &changedAt = String()) {
  if (!capsuleOperations->submit(action, ids, changedAt)) {
    showMessage(capsuleOperations->mutationCapabilityBlocked()
                    ? "本地操作恢复失败，请连接 Mac"
                    : "本地操作忙，请稍后再试",
                capsuleOperations->mutationCapabilityBlocked()
                    ? UiNoticeKind::error
                    : UiNoticeKind::warning);
    return false;
  }
  pendingLocalOperationIds = ids;
  localOperationPresentation = presentation;
  showMessage("正在安全处理…", UiNoticeKind::progress, 3000);
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
  if (!capsuleOperations->takeOutcome(outcome)) return;
  const LocalOperationPresentation presentation = localOperationPresentation;
  localOperationPresentation = LocalOperationPresentation::none;
  if (outcome.committed) {
    if (presentation == LocalOperationPresentation::undoTrash) {
      const std::vector<std::string> noFailures;
      const CapsuleUndoResult restored = trashUndo.finishAttempt(noFailures);
      showMessage(String("已恢复 ") + restored.restored + " 条",
                  UiNoticeKind::success);
    } else if (presentation == LocalOperationPresentation::purge) {
      showMessage(String("已永久删除 ") + outcome.changed + " 条",
                  UiNoticeKind::success);
    } else if (presentation == LocalOperationPresentation::trash) {
      armTrashUndo(pendingLocalOperationIds);
    } else if (presentation == LocalOperationPresentation::archive) {
      showMessage("已归档", UiNoticeKind::success);
    } else if (presentation == LocalOperationPresentation::unarchive) {
      showMessage("已移回原目录", UiNoticeKind::success);
    } else if (presentation == LocalOperationPresentation::restore) {
      showMessage("已恢复", UiNoticeKind::success);
    } else {
      showMessage(String("已处理 ") + outcome.changed + " 条",
                  UiNoticeKind::success);
    }
  } else if (outcome.rollbackFailed || outcome.authorityPreserved) {
    showMessage("操作中断，已保留恢复记录，请连接 Mac",
                UiNoticeKind::error, 5000);
  } else {
    showMessage("操作失败，已恢复", UiNoticeKind::error);
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
  view.settings = &deviceConfig.settings();
  view.portalPassword = nullptr;
  if (psramDegradedBoot) {
    view.localCapsuleStatus = "PSRAM REQUIRED";
    view.audioReady = false;
    view.localCapsulesReady = false;
    view.recorderReady = false;
    view.transcriptionReady = false;
    view.bleVoiceServiceReady = false;
    view.linkReady = false;
    view.wifiServiceReady = false;
    view.usbReady = usb.ready();
    view.usbConnected = usbCableConnected();
    view.provisioningDiagnostics = &provisioningDiagnostics;
    if (deadlinePending(now, transientUntilMs)) {
      view.message = transientMessage;
      view.messageKind = transientMessageKind;
    }
    dashboard.draw(view);
    return;
  }
  view.capsuleLibraryReady =
      capabilities.ready(DeviceCapability::capsuleLibrary);
  view.library = view.capsuleLibraryReady ? &capsuleLibrary.get() : nullptr;
  view.audioReady = audio.ready();
  view.localCapsulesReady = capabilities.allows(kRecordingCapabilities);
  if (!view.localCapsulesReady) {
    view.localCapsuleStatus = localCapsuleStatusMessage();
  }
  view.recorderReady = capabilities.ready(DeviceCapability::recording);
  view.transcriptionReady =
      capabilities.ready(DeviceCapability::transcription);
  view.bleVoiceServiceReady = capabilities.ready(DeviceCapability::bleVoice);
  view.shutdownPending = safeShutdownQuiesce.pending();
  view.bluetoothEnabled = bleVoice.userEnabled();
  view.bleVoiceDisablePending = bleVoice.disablePending();
  view.linkReady = capabilities.ready(DeviceCapability::link);
  view.wifiServiceReady = capabilities.ready(DeviceCapability::wifi);
  view.usbReady = usb.ready();
  view.usbConnected = usbCableConnected();
  view.bleVoiceConnected = bleVoice.connected();
  view.bleVoiceReady = bleVoice.appReady();
  view.bleVoiceHandshakeDisconnectPending =
      bleVoice.appHandshakeDisconnectPending();
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
  view.wifiSyncOpen = wirelessSync->openWindow();
  view.wifiSyncSecureReady = wirelessSync->secureReady();
  view.wifiSyncPaired = wirelessSync->paired();
  view.wifiSyncNetworkConnected = wirelessSync->networkConnected();
  view.wifiSyncListener = wirelessSync->listenerActive();
  view.wifiSyncBonjour = wirelessSync->bonjourActive();
  view.wifiSyncClient = wirelessSync->clientConnected();
  view.wifiSyncAuthenticated = wirelessSync->authenticated();
  view.wifiSyncBusy = wirelessSync->linkBusy();
  view.wifiSyncCompleted = wirelessSync->completed();
  view.wifiSyncRemainingSeconds = wirelessSync->remainingSeconds(now);
  view.wifiSyncLastCompletedAtMs = wirelessSync->lastCompletedAtMs();
  view.wifiSyncLastError = wirelessSync->lastError();
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
  // Dashboard consumes this borrow synchronously below; it never owns or
  // retains credential bytes after drawDashboard() returns.
  view.portalPassword =
      view.provisioning ? &provisioningPortal.password() : nullptr;
  view.portalStatus = provisioningPortal.statusMessage();
  view.portalState = provisioningPortal.state();
  view.provisioningDiagnostics = &provisioningDiagnostics;
  if (deadlinePending(now, transientUntilMs)) {
    view.message = transientMessage;
    view.messageKind = transientMessageKind;
  }
  dashboard.draw(view);
}

bool requestCaptureStop(PendingCaptureStop owner, RecorderStopReason reason,
                        bool forceAbort, bool notifyUser);

void recordWirelessRuntime(RuntimeDiagnosticStage stage,
                           RuntimeDiagnosticOutcome outcome,
                           uint32_t detail0, uint32_t detail1) {
  (void)runtimeDiagnostics.record(RuntimeDiagnosticSubsystem::wirelessVoice,
                                  stage, outcome, detail0, detail1, usb.log());
}

bool startWirelessHold() {
  const uint32_t readiness =
      (bleVoice.connected() ? 1U : 0U) |
      (bleVoice.mtuReady() ? 2U : 0U) |
      (bleVoice.appReady() ? 4U : 0U) |
      (bleVoice.userEnabled() ? 8U : 0U);
  recordWirelessRuntime(RuntimeDiagnosticStage::wirelessAttempt,
                        RuntimeDiagnosticOutcome::started, readiness,
                        static_cast<uint32_t>(bleVoice.sessionState()));
  if (storageBootPhase != StorageBootPhase::ready) {
    recordWirelessRuntime(RuntimeDiagnosticStage::wirelessFailure,
                          RuntimeDiagnosticOutcome::failure, 1,
                          static_cast<uint32_t>(storageBootPhase));
    showMessage("本地服务启动中，请稍候", UiNoticeKind::progress);
    drawDashboard();
    return false;
  }
  if (!bleVoice.userEnabled()) {
    recordWirelessRuntime(RuntimeDiagnosticStage::wirelessFailure,
                          RuntimeDiagnosticOutcome::failure, 2, readiness);
    showMessage("蓝牙已关闭", UiNoticeKind::warning);
    drawDashboard();
    return false;
  }
  if (!capabilities.allows(kBleVoiceCapabilities)) {
    recordWirelessRuntime(RuntimeDiagnosticStage::wirelessFailure,
                          RuntimeDiagnosticOutcome::failure, 3,
                          capabilities.readyMask());
    showMessage("无线语音服务未就绪", UiNoticeKind::warning);
    drawDashboard();
    return false;
  }
  if (!bleVoice.appReady()) {
    recordWirelessRuntime(RuntimeDiagnosticStage::wirelessFailure,
                          RuntimeDiagnosticOutcome::failure, 4,
                          (static_cast<uint32_t>(bleVoice.mtu()) << 16) |
                              static_cast<uint32_t>(bleVoice.sessionError()));
    showMessage(bleVoice.connected()
                    ? (bleVoice.mtuReady() ? "等待 Mac 应用"
                                           : "等待蓝牙 MTU")
                    : "等待 Mac 应用",
                UiNoticeKind::warning);
    drawDashboard();
    return false;
  }
  if (tencentWorker.working()) {
    recordWirelessRuntime(RuntimeDiagnosticStage::wirelessFailure,
                          RuntimeDiagnosticOutcome::failure, 6, readiness);
    showMessage("当前胶囊正在转写", UiNoticeKind::warning);
    drawDashboard();
    return false;
  }
  if (audio.playing()) audio.stopPlayback(usb.log());
  wifi.pauseForAudioCapture(usb.log());
  recordWirelessRuntime(RuntimeDiagnosticStage::wirelessRouterAcquire,
                        RuntimeDiagnosticOutcome::started,
                        static_cast<uint32_t>(captureRouter.owner()), 0);
  if (!captureRouter.acquire(AudioCaptureOwner::wirelessVoice)) {
    recordWirelessRuntime(RuntimeDiagnosticStage::wirelessRouterAcquire,
                          RuntimeDiagnosticOutcome::failure,
                          static_cast<uint32_t>(captureRouter.owner()), 0);
    showMessage("无线麦克风暂时不可用", UiNoticeKind::warning);
    drawDashboard();
    return false;
  }
  uint32_t sessionId = esp_random();
  if (sessionId == 0) sessionId = 1;
  recordWirelessRuntime(RuntimeDiagnosticStage::wirelessCaptureStart,
                        RuntimeDiagnosticOutcome::started, sessionId, 0);
  wirelessSessionTelemetry.reset();
  wirelessTelemetryProducer =
      wirelessSessionTelemetry.bindCaptureSession(sessionId);
  wirelessTelemetryActive = wirelessTelemetryProducer.valid();
  if (!captureRuntime.start(audio, sessionId, usb.log())) {
    wirelessTelemetryActive = false;
    wirelessTelemetryProducer = {};
    captureRouter.release(AudioCaptureOwner::wirelessVoice);
    recordWirelessRuntime(RuntimeDiagnosticStage::wirelessCaptureStart,
                          RuntimeDiagnosticOutcome::failure, sessionId,
                          static_cast<uint32_t>(audio.readFailures()));
    showMessage("无线麦克风暂时不可用", UiNoticeKind::warning);
    drawDashboard();
    return false;
  }
  if (!wirelessCaptureStart.begin(sessionId, millis())) {
    (void)requestCaptureStop(PendingCaptureStop::wirelessVoice,
                             RecorderStopReason::none, true, false);
    showMessage("无线麦克风暂时不可用", UiNoticeKind::error);
    drawDashboard();
    return false;
  }
  recordWirelessRuntime(RuntimeDiagnosticStage::wirelessCaptureStart,
                        RuntimeDiagnosticOutcome::success, sessionId, 0);
  usb.log().printf(
      "{\"event\":\"wireless_capture_arming\",\"session_id\":%lu,"
      "\"first_frame_deadline_ms\":%lu}\n",
      static_cast<unsigned long>(sessionId),
      static_cast<unsigned long>(
          WirelessCaptureStartPolicy::kFirstFrameDeadlineMs));
  usb.log().printf(
      "{\"event\":\"wireless_capture_started\",\"session_id\":%lu}\n",
      static_cast<unsigned long>(sessionId));
  wirelessUiActive = true;
  noteUserActivity();
  transientMessage = "";
  transientUntilMs = 0;
  drawDashboard();
  return true;
}

void advanceWirelessCaptureStart(uint32_t nowMs) {
  if (!wirelessCaptureStart.active()) return;
  const AudioCaptureServiceMetrics metrics = captureRuntime.metrics();
  const WirelessCaptureStartAction action =
      wirelessCaptureStart.observe(nowMs, metrics);
  if (action == WirelessCaptureStartAction::waiting) return;
  const uint32_t sessionId = wirelessCaptureStart.sessionId();
  if (action == WirelessCaptureStartAction::abortCapture) {
    wirelessCaptureStart.finish();
    recordWirelessRuntime(
        RuntimeDiagnosticStage::wirelessFailure,
        RuntimeDiagnosticOutcome::failure, 5,
        (static_cast<uint32_t>(metrics.firstFailure) << 24) |
            ((metrics.earlyZeroReads & 0xfffU) << 12) |
            (metrics.warmupZeroReads & 0xfffU));
    (void)requestCaptureStop(PendingCaptureStop::wirelessVoice,
                             RecorderStopReason::none, true, false);
    showMessage("麦克风未就绪", UiNoticeKind::error);
    dashboard.invalidate();
    return;
  }
  recordWirelessRuntime(RuntimeDiagnosticStage::wirelessSessionStart,
                        RuntimeDiagnosticOutcome::started, sessionId,
                        static_cast<uint32_t>(bleVoice.mtu()));
  if (!bleVoice.startSession(sessionId, nowMs, captureRouter)) {
    wirelessCaptureStart.finish();
    recordWirelessRuntime(RuntimeDiagnosticStage::wirelessSessionStart,
                          RuntimeDiagnosticOutcome::failure, sessionId,
                          static_cast<uint32_t>(bleVoice.sessionError()));
    (void)requestCaptureStop(PendingCaptureStop::wirelessVoice,
                             RecorderStopReason::none, true, false);
    showMessage("无线会话启动失败", UiNoticeKind::error);
    dashboard.invalidate();
    return;
  }
  wirelessCaptureStart.finish();
  recordWirelessRuntime(RuntimeDiagnosticStage::wirelessSessionStart,
                        RuntimeDiagnosticOutcome::success, sessionId,
                        static_cast<uint32_t>(bleVoice.sessionState()));
  recordWirelessRuntime(RuntimeDiagnosticStage::wirelessReady,
                        RuntimeDiagnosticOutcome::success, sessionId,
                        (static_cast<uint32_t>(bleVoice.mtu()) << 16) |
                            static_cast<uint32_t>(bleVoice.appReady()));
  usb.log().printf(
      "{\"event\":\"wireless_first_frame_ready\",\"session_id\":%lu,"
      "\"queued_frames\":%lu,\"warmup_zero_reads\":%lu}\n",
      static_cast<unsigned long>(sessionId),
      static_cast<unsigned long>(metrics.ring.currentFrames),
      static_cast<unsigned long>(metrics.warmupZeroReads));
}

AudioCaptureDispatchResult drainCapturedAudio(uint32_t nowMs) {
  return captureDispatcher.drain(captureRuntime, captureRouter, audio, recorder,
                                 bleVoice, usb.log(), nowMs);
}

void observeCaptureMetrics() {
  const AudioCaptureFrontEndSnapshot snapshot =
      captureRuntime.frontEndSnapshot();
  if (snapshot.sessionId == 0 || snapshot.generation == 0) return;
  if (captureTelemetrySampledSessionId != snapshot.sessionId) {
    captureTelemetrySampledSessionId = snapshot.sessionId;
    captureTelemetryLastStackSampleMs = 0;
    captureTelemetryStackHighWaterWords = 0;
    captureTelemetryStackSampled = false;
  }
  const bool terminalOrActiveCapture = captureRuntime.running() ||
      pendingCaptureStop != PendingCaptureStop::none;
  if ((recorder.operationActive() || wirelessTelemetryActive) &&
      terminalOrActiveCapture) {
    const uint32_t nowMs = millis();
    const bool terminalSample = !captureRuntime.running() &&
        pendingCaptureStop != PendingCaptureStop::none;
    const bool periodicSample = !captureTelemetryStackSampled ||
        static_cast<uint32_t>(nowMs - captureTelemetryLastStackSampleMs) >=
            1000U;
    if (terminalSample || periodicSample) {
      captureTelemetryStackHighWaterWords = static_cast<uint32_t>(
          captureRuntime.taskStackHighWater());
      captureTelemetryLastStackSampleMs = nowMs;
      captureTelemetryStackSampled = true;
    }
    const AudioCaptureServiceMetrics captureMetrics = captureRuntime.metrics();
    const AudioCaptureDispatcherMetrics dispatcherMetrics =
        captureDispatcher.metrics();
    if (recorder.operationActive()) {
      recorder.observeCaptureTelemetry(
          captureMetrics, dispatcherMetrics, captureTelemetryStackHighWaterWords);
    }
    if (wirelessTelemetryActive) {
      wirelessSessionTelemetry.observeCapture(
          wirelessTelemetryProducer, captureMetrics, dispatcherMetrics,
          captureTelemetryStackHighWaterWords);
    }
  }
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

void publishWirelessTelemetryTerminal() {
  if (!wirelessTelemetryActive) return;
  wirelessSessionTelemetry.freeze();
  const AudioSessionTelemetrySnapshot snapshot =
      wirelessSessionTelemetry.snapshot();
  runtimeDiagnostics.publishAudioSessionSnapshot(snapshot);
  usb.log().printf(
      "{\"event\":\"audio_session_terminal\",\"owner\":\"wireless_voice\","
      "\"session_id\":%lu,\"generation\":%lu,\"frozen\":%s,"
      "\"incomplete\":%s,\"first_failure\":\"%s\","
      "\"read_calls\":%lu,\"timeouts\":%lu,\"zero_reads\":%lu,"
      "\"early_zero_reads\":%lu,\"ring_drops\":%lu,"
      "\"dispatch_ble_failures\":%lu,\"sequence_gaps\":%lu,"
      "\"i2s_longest_read_us\":%lu}\n",
      static_cast<unsigned long>(snapshot.sessionId),
      static_cast<unsigned long>(snapshot.generation),
      snapshot.frozen ? "true" : "false", snapshot.incomplete() ? "true" : "false",
      audioCaptureFailureCodeName(snapshot.firstFailure),
      static_cast<unsigned long>(snapshot.readCalls),
      static_cast<unsigned long>(snapshot.i2sTimeouts),
      static_cast<unsigned long>(snapshot.zeroByteReads),
      static_cast<unsigned long>(snapshot.earlyZeroReads),
      static_cast<unsigned long>(snapshot.captureRingDroppedFrames),
      static_cast<unsigned long>(snapshot.dispatchBleDeliveryFailures),
      static_cast<unsigned long>(snapshot.sequenceGaps),
      static_cast<unsigned long>(snapshot.i2sLongestReadUs));
  wirelessTelemetryActive = false;
  wirelessTelemetryProducer = {};
}

bool consumeRecorderTerminal(bool notifyUser);

void finishLocalRecordingStartFailure(bool notifyUser, bool ownsRouter) {
  const bool ownsRecorder =
      LocalRecordingOwnershipPolicy::mayManageFailure(
          recorder.ownedBy(RecorderOperationOwner::localApp));
  pendingRecorderFinalize = ownsRecorder && recorder.operationActive();
  pendingRecorderResultNotify = pendingRecorderResultNotify || notifyUser;
  if (pendingRecorderFinalize) return;
  if (LocalRecordingOwnershipPolicy::mayReleaseRouter(ownsRouter)) {
    captureRouter.release(AudioCaptureOwner::localCapsule);
  }
  const bool terminalConsumed = ownsRecorder &&
      consumeRecorderTerminal(pendingRecorderResultNotify);
  pendingRecorderResultNotify = false;
  if (notifyUser && !terminalConsumed) {
    showMessage("录音启动失败", UiNoticeKind::error);
  }
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
  finishLocalRecordingStartFailure(true, true);
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
    wirelessCaptureStart.finish();
    const AudioCaptureServiceMetrics captureMetrics = captureRuntime.metrics();
    publishWirelessTelemetryTerminal();
    recordWirelessRuntime(RuntimeDiagnosticStage::wirelessStop,
                          complete ? RuntimeDiagnosticOutcome::success
                                   : RuntimeDiagnosticOutcome::failure,
                          captureMetrics.readCalls,
                          ((captureMetrics.timeouts & 0xffffU) << 16) |
                              (captureMetrics.ring.droppedFrames & 0xffffU));
    usb.log().printf(
        "{\"event\":\"wireless_capture_terminal\",\"session_id\":%lu,"
        "\"complete\":%s,\"dispatch_ok\":%s,\"read_calls\":%lu,"
        "\"short_reads\":%lu,\"timeouts\":%lu,\"early_zero\":%lu,"
        "\"source_failures\":%lu,\"longest_read_us\":%lu,"
        "\"ring_high_water\":%lu,\"ring_drops\":%lu,"
        "\"consumed_frames\":%lu}\n",
        static_cast<unsigned long>(captureMetrics.ring.sessionId),
        complete ? "true" : "false", dispatch.ok ? "true" : "false",
        static_cast<unsigned long>(captureMetrics.readCalls),
        static_cast<unsigned long>(captureMetrics.shortReads),
        static_cast<unsigned long>(captureMetrics.timeouts),
        static_cast<unsigned long>(captureMetrics.earlyZeroReads),
        static_cast<unsigned long>(captureMetrics.sourceFailures),
        static_cast<unsigned long>(captureMetrics.longestReadUs),
        static_cast<unsigned long>(captureMetrics.ring.highWaterFrames),
        static_cast<unsigned long>(captureMetrics.ring.droppedFrames),
        static_cast<unsigned long>(dispatch.consumedFrames));
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
  wirelessCaptureStart.finish();
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
    if (!capsuleLibrary->scanActive() && !capsuleLibrary->scanRequested()) {
      indexed = capsuleLibrary->includeInboxCapsule(recorder.capsuleId());
    } else {
      indexed = false;
    }
    if (!indexed) refreshQueued = capsuleLibrary->requestScan();
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
    const UiNoticeKind kind = completed ? UiNoticeKind::success
                                        : UiNoticeKind::error;
    showMessage(message, kind);
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
    showMessage("正在取消录音…", UiNoticeKind::progress);
  } else if (recorder.recording() &&
      recorder.ownedBy(RecorderOperationOwner::localApp)) {
    stopLocalCapture(RecorderStopReason::user);
  } else if (recorder.operationActive() || pendingRecorderFinalize) {
    showMessage("上一段录音仍在保存", UiNoticeKind::warning);
  } else if (tencentWorker.working()) {
    showMessage("当前胶囊正在转写", UiNoticeKind::warning);
  } else if (!capabilities.allows(kRecordingCapabilities)) {
    showMessage(board.sdReady() ? "录音服务未就绪" : "请插入 microSD 卡",
                UiNoticeKind::warning);
  } else if (!board.sdReady()) {
    showMessage("请插入 microSD 卡", UiNoticeKind::warning);
  } else if (!audio.ready()) {
    showMessage("麦克风尚未就绪", UiNoticeKind::warning);
  } else {
    if (audio.playing()) audio.stopPlayback(usb.log());
    tencentWorker.wake();
    const bool mayAdmit = LocalRecordingOwnershipPolicy::mayAdmit(
        captureRouter.available(), recorder.operationActive(),
        recorder.terminalResult().pending());
    const bool acquired = mayAdmit &&
        captureRouter.acquire(AudioCaptureOwner::localCapsule);
    uint32_t captureSessionId = esp_random();
    if (captureSessionId == 0) captureSessionId = 1;
    lastLocalCaptureMetrics = {};
    captureTelemetrySampledSessionId = captureSessionId;
    captureTelemetryLastStackSampleMs = 0;
    captureTelemetryStackHighWaterWords = 0;
    captureTelemetryStackSampled = false;
    if (acquired) wifi.pauseForAudioCapture(usb.log());
    const bool capturePrepared = acquired &&
        captureRuntime.prepare(audio, usb.log());
    const bool stateReady = capturePrepared &&
        localRecordingStart.begin(captureSessionId);
    const bool requested = stateReady &&
        recorder.requestStart(usb.log(), recordingId(), board.utcNow(),
                              RecorderOperationOwner::localApp);
    if (requested) {
      showMessage("正在检查存储…", UiNoticeKind::progress, 3000);
    } else {
      localRecordingStart.reset();
      finishLocalRecordingStartFailure(true, acquired);
    }
  }
  noteUserActivity();
  drawDashboard();
}

BootGestureContext bootGestureContext() {
  BootGestureContext context;
  context.screenOn = board.status().screenOn;
  context.provisioning = provisioningCoordinator.visible();
  context.sensitiveConfirmationPending =
      provisioningCoordinator.sensitiveConfirmationPending();
  context.wirelessHolding = bootWirelessHolding;
  context.localRecording =
      recorder.ownedBy(RecorderOperationOwner::localApp) &&
      (recorder.recording() || localRecordingStart.active());
  context.bluetoothEnabled = bleVoice.userEnabled();
  context.wirelessAppReady = bleVoice.appReady();
  return context;
}

void applyBootGestureAction(BootGestureAction action, uint32_t nowMs) {
  switch (action) {
    case BootGestureAction::wakeScreen:
      noteUserActivity(nowMs);
      setScreenState(true);
      return;
    case BootGestureAction::confirmProvisioning: {
      const bool confirmed =
          provisioningCoordinator.confirmSensitiveChange(nowMs);
      dashboard.invalidate();
      showMessage(confirmed ? "已确认腾讯密钥操作"
                            : "实体确认超时，请再次保存",
                  confirmed ? UiNoticeKind::success : UiNoticeKind::error);
      return;
    }
    case BootGestureAction::exitProvisioning:
      if (provisioningCoordinator.visible()) {
        provisioningCoordinator.stop(ProvisioningStopReason::bootButton);
      }
      dashboard.back();
      dashboard.invalidate();
      showMessage("已退出手机配网", UiNoticeKind::success);
      return;
    case BootGestureAction::stopWirelessVoice:
      bootWirelessHolding = false;
      (void)stopWirelessHold();
      return;
    case BootGestureAction::startWirelessVoice:
      bootWirelessHolding = startWirelessHold();
      return;
    case BootGestureAction::wirelessUnavailable:
      showMessage("等待 Mac 应用", UiNoticeKind::warning);
      dashboard.invalidate();
      return;
    case BootGestureAction::voiceReadyShortPress:
      showMessage("请按住说话", UiNoticeKind::warning);
      dashboard.invalidate();
      return;
    case BootGestureAction::bluetoothDisabled:
      showMessage("蓝牙已关闭", UiNoticeKind::warning);
      dashboard.invalidate();
      return;
    case BootGestureAction::stopLocalRecording:
      (void)stopLocalCapture(RecorderStopReason::user);
      return;
    case BootGestureAction::startLocalRecording:
      toggleRecording();
      return;
    case BootGestureAction::armProvisioningExit:
    case BootGestureAction::none:
      return;
  }
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
      static_cast<unsigned>(capsuleLibrary->pendingCount()),
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
      wirelessSync->openWindow() ? "true" : "false",
      wirelessSyncWindowPhaseName(wirelessSync->phase()),
      wirelessSync->secureReady() ? "true" : "false",
      wirelessSync->listenerActive() ? "true" : "false",
      wirelessSync->bonjourActive() ? "true" : "false",
      wirelessSync->clientConnected() ? "true" : "false",
      wirelessSync->authenticated() ? "true" : "false",
      static_cast<unsigned long>(wirelessSync->remainingSeconds(millis())),
      wirelessSync->lastError());
  const AudioCaptureServiceMetrics captureMetrics = captureRuntime.metrics();
  const AudioCaptureDispatcherMetrics dispatchMetrics =
      captureDispatcher.metrics();
  usb.log().printf(
      "{\"event\":\"audio_capture_status\",\"session_id\":%lu,"
      "\"read_calls\":%lu,\"short_reads\":%lu,\"timeouts\":%lu,"
      "\"zero_reads\":%lu,\"early_zero\":%lu,"
      "\"source_overruns\":%lu,\"source_failures\":%lu,"
      "\"longest_read_us\":%lu,\"ring_current\":%lu,"
      "\"ring_high_water\":%lu,\"ring_drops\":%lu,"
      "\"dispatch_consumed\":%lu,\"dispatch_voice_failures\":%lu,"
      "\"dispatch_routing_failures\":%lu}\n",
      static_cast<unsigned long>(captureMetrics.ring.sessionId),
      static_cast<unsigned long>(captureMetrics.readCalls),
      static_cast<unsigned long>(captureMetrics.shortReads),
      static_cast<unsigned long>(captureMetrics.timeouts),
      static_cast<unsigned long>(captureMetrics.zeroByteReads),
      static_cast<unsigned long>(captureMetrics.earlyZeroReads),
      static_cast<unsigned long>(captureMetrics.sourceOverruns),
      static_cast<unsigned long>(captureMetrics.sourceFailures),
      static_cast<unsigned long>(captureMetrics.longestReadUs),
      static_cast<unsigned long>(captureMetrics.ring.currentFrames),
      static_cast<unsigned long>(captureMetrics.ring.highWaterFrames),
      static_cast<unsigned long>(captureMetrics.ring.droppedFrames),
      static_cast<unsigned long>(dispatchMetrics.consumedFrames),
      static_cast<unsigned long>(dispatchMetrics.voiceDeliveryFailures),
      static_cast<unsigned long>(dispatchMetrics.routingFailures));
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
    touchAction = dashboard.actionAt(x, y, bleVoice.userEnabled());
  } else if (touched) {
    touchGesture.update(x, y);
    if (!touchVerticalScrolling && !touchWirelessHolding &&
        !touchCapsuleSelectionAttempted &&
        touchGesture.verticalSwipe() &&
        capabilities.allows(kCapsuleBrowsingCapabilities)) {
      touchVerticalScrolling = dashboard.beginVerticalScroll(
          touchGesture.startY, touchGesture.startedAtMs,
          capsuleLibrary.get());
    }
    if (touchVerticalScrolling) {
      if (dashboard.updateVerticalScroll(y, now, capsuleLibrary.get())) {
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
                                             capsuleLibrary.get())) {
        showMessage("转写中或版本只读，暂时不能选择", UiNoticeKind::warning);
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
          provisioningCoordinator.stop(ProvisioningStopReason::touchBack);
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
      showMessage(localCapsuleStatusMessage(), UiNoticeKind::warning, 3000);
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
        if (!dashboard.toggleCapsuleSelectionAt(
                startY, capsuleLibrary.get())) {
          showMessage("转写中或版本只读，暂时不能选择",
                      UiNoticeKind::warning);
        }
      } else {
        dashboard.openCapsuleAt(startY, capsuleLibrary.get());
      }
      drawDashboard();
    } else if (action == UiAction::back) {
      if (provisioningCoordinator.visible() &&
          dashboard.state().screen() != UiScreen::provisioningLog) {
        provisioningCoordinator.stop(ProvisioningStopReason::touchBack);
      }
      dashboard.back();
      drawDashboard();
    } else if (action == UiAction::openProvisioningLog) {
      dashboard.openProvisioningLog();
      drawDashboard();
    } else if (action == UiAction::wifiToggle) {
      if (!capabilities.allows(kWifiCapabilities)) {
        showMessage("Wi-Fi 服务未就绪", UiNoticeKind::warning);
        drawDashboard();
        return;
      }
      if (wifiUiSwitchOn(wifi.phase())) {
        if (wirelessSync->openWindow()) wirelessSync->close();
        if (deviceConfig.setWifiEnabled(false, usb.log())) {
          wifi.configurationChanged();
          showMessage("Wi-Fi 已关闭", UiNoticeKind::success);
        }
      } else if (!deviceConfig.hasWifi()) {
        showMessage("请先完成手机配网", UiNoticeKind::warning);
      } else {
        bool enabled = deviceConfig.settings().wifiEnabled;
        if (!enabled) {
          enabled = deviceConfig.setWifiEnabled(true, usb.log());
        }
        if (enabled) {
          wifi.requestConnection();
          tencentWorker.wake();
          showMessage("正在连接 Wi-Fi", UiNoticeKind::progress);
        }
      }
      dashboard.invalidate();
      drawDashboard();
    } else if (action == UiAction::bluetoothToggle) {
      if (!capabilities.ready(DeviceCapability::bleVoice)) {
        showMessage("蓝牙服务未就绪", UiNoticeKind::warning);
        drawDashboard();
        return;
      }
      const bool enabled = !bleVoice.userEnabled();
      // Persisted user intent is the source of truth. Runtime state changes
      // only after the complete atomic blob has committed.
      if (!deviceConfig.setBluetoothEnabled(enabled, usb.log())) {
        showMessage("蓝牙设置保存失败", UiNoticeKind::error);
        drawDashboard();
        return;
      }
      if (enabled) {
        bleVoice.requestEnable();
        const bool pending = bleVoice.disablePending();
        showMessage(pending ? "蓝牙将在语音结束后开启" : "蓝牙已开启",
                    pending ? UiNoticeKind::progress : UiNoticeKind::success);
      } else {
        bleVoice.requestDisable(now);
        const bool pending = bleVoice.disablePending();
        showMessage(pending ? "蓝牙正在安全关闭" : "蓝牙已关闭",
                    pending ? UiNoticeKind::progress : UiNoticeKind::success);
      }
      dashboard.invalidate();
      drawDashboard();
    } else if (action == UiAction::openBluetoothPairing) {
      if (!capabilities.ready(DeviceCapability::bleVoice)) {
        showMessage("蓝牙服务未就绪", UiNoticeKind::warning);
        drawDashboard();
        return;
      }
      dashboard.openBluetoothPairing();
      drawDashboard();
    } else if (action == UiAction::toggleBluetoothPairing) {
      if (!bleVoice.userEnabled() ||
          bleVoice.disablePending()) {
        showMessage("请先开启蓝牙", UiNoticeKind::warning);
        drawDashboard();
        return;
      }
      if (bleVoice.pairingMode(now)) {
        bleVoice.cancelPairingMode();
      showMessage("已取消配对", UiNoticeKind::info);
      } else {
        bleVoice.enterPairingMode(now);
        if (bleVoice.pairingMode(now)) {
          char pairMessage[48];
          snprintf(pairMessage, sizeof(pairMessage), "配对码 %06lu · 长按忘记",
                   static_cast<unsigned long>(bleVoice.passkey()));
          showMessage(String(pairMessage), UiNoticeKind::info, 5000);
        } else {
          // An existing physical connection must close before the pairing
          // attempt owns its new passkey. Never snapshot the previous code.
          showMessage("正在断开当前连接…", UiNoticeKind::progress, 3000);
        }
      }
      dashboard.invalidate();
      drawDashboard();
    } else if (action == UiAction::forgetBluetoothMac) {
      if (captureRouter.owner() == AudioCaptureOwner::wirelessVoice) {
        showMessage("语音输入中，请先松开", UiNoticeKind::warning);
        drawDashboard();
        return;
      }
      if (bleVoice.bonded()) {
        bleVoice.forgetMac();
        showMessage("已忘记 Mac", UiNoticeKind::success);
      } else {
        showMessage("当前没有已配对 Mac", UiNoticeKind::warning);
      }
      dashboard.invalidate();
      drawDashboard();
    } else if (action == UiAction::openComputerSync) {
      if (!capabilities.allows(kComputerSyncCapabilities)) {
        showMessage("电脑同步服务未就绪", UiNoticeKind::warning);
        drawDashboard();
        return;
      }
      if (computerSyncEntryDecision(wirelessSync->openWindow()) ==
          ComputerSyncEntryDecision::openAndNavigate) {
        wirelessSync->open(now);
      }
      dashboard.openComputerSync();
      dashboard.invalidate();
      drawDashboard();
    } else if (action == UiAction::closeComputerSync) {
      if (wirelessSync->openWindow()) wirelessSync->close();
      dashboard.back();
      showMessage("电脑同步已关闭", UiNoticeKind::success);
      dashboard.invalidate();
      drawDashboard();
    } else if (action == UiAction::openProvisioning) {
      if (storageBootPhase != StorageBootPhase::ready) {
        showMessage("本地服务启动中，请稍候", UiNoticeKind::progress);
        drawDashboard();
        return;
      }
      if (!capabilities.allows(kWifiCapabilities)) {
        showMessage("Wi-Fi 服务未就绪", UiNoticeKind::warning);
        drawDashboard();
        return;
      }
      if (wirelessSync->openWindow()) wirelessSync->close();
      if (provisioningCoordinator.request(now)) {
        showMessage("正在准备配网热点", UiNoticeKind::progress);
      } else {
        showMessage("配网启动请求失败", UiNoticeKind::error);
      }
      dashboard.invalidate();
      drawDashboard();
    } else if (action == UiAction::openShutdownConfirm) {
      if (safeShutdownQuiesce.pending()) {
        showMessage("正在安全关机", UiNoticeKind::progress, 2000);
      } else {
        dashboard.openShutdownConfirm();
        if (recorder.recording() || captureRuntime.running()) {
          showMessage("确认后会先保存当前录音", UiNoticeKind::warning, 3000);
        }
      }
      dashboard.invalidate();
      drawDashboard();
    } else if (action == UiAction::confirmShutdown) {
      dashboard.closeOverlays();
      requestSafeShutdown(now);
      showMessage("正在保存并关机", UiNoticeKind::progress, 3000);
      dashboard.invalidate();
      drawDashboard();
    } else if (action == UiAction::raiseToWakeToggle) {
      const bool enabled = !automaticWakeEnabled();
      if (deviceConfig.setRaiseToWake(enabled, usb.log())) {
        showMessage(enabled ? "自动亮屏已开启" : "自动亮屏已关闭",
                    UiNoticeKind::success);
      }
      dashboard.invalidate();
      drawDashboard();
    } else if (action == UiAction::openCapsuleScope) {
      dashboard.openScopePicker();
      drawDashboard();
    } else if (capsuleScopeIndexForAction(action) >= 0) {
      capsuleLibrary->setScope(static_cast<CapsuleScope>(
          capsuleScopeIndexForAction(action)));
      dashboard.scopeChanged();
      drawDashboard();
    } else if (action == UiAction::openDetailMore) {
      dashboard.openDetailMore();
      drawDashboard();
    } else if (action == UiAction::requestPurge) {
      pendingPurgeIds.clear();
      if (dashboard.capsuleSelectionMode()) {
        pendingPurgeIds = dashboard.selectedCapsuleIds(capsuleLibrary.get());
      } else {
        const CapsuleSummary *selected = dashboard.selected(
            capsuleLibrary.get());
        if (selected != nullptr) pendingPurgeIds.push_back(selected->id);
      }
      bool safe = !pendingPurgeIds.empty();
      for (const String &id : pendingPurgeIds) {
        const CapsuleSummary *record = capsuleLibrary->find(id);
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
        showMessage("版本过新或状态忙，请在 Mac 处理", UiNoticeKind::warning);
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
          dashboard.selectedCapsuleIds(capsuleLibrary.get());
      const CapsuleScope scope = capsuleLibrary->scope();
      if (action == UiAction::bulkFavorite) {
        const CapsuleBatchResult result = capsuleLibrary->batch(
            ids, CapsuleBatchAction::favorite, board.utcNow());
        dashboard.clearCapsuleSelection();
        showMessage(result.ok ? String("已处理 ") + result.changed + " 条"
                              : "批量收藏未完成",
                    result.ok ? UiNoticeKind::success : UiNoticeKind::error);
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
      const CapsuleSummary *selected = dashboard.selected(
          capsuleLibrary.get());
      if (selected == nullptr) return;
      const String id = selected->id;
      if (selected->readOnly &&
          (action == UiAction::favorite || action == UiAction::archive ||
           action == UiAction::trash || action == UiAction::retry ||
           action == UiAction::play)) {
        showMessage("版本过新，请在 Mac 处理", UiNoticeKind::warning);
        drawDashboard();
        return;
      }
      if (action == UiAction::favorite) {
        capsuleLibrary->toggleFavorite(id);
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
        showMessage("转写服务未就绪", UiNoticeKind::warning);
          drawDashboard();
          return;
        }
        if (selected->status != CapsuleStatus::failed &&
            !(selected->status == CapsuleStatus::queued &&
              !selected->error.isEmpty())) return;
        if (capsuleLibrary->requeue(id)) {
          dashboard.closeOverlays();
          tencentWorker.wake();
          showMessage("已重新加入转写队列", UiNoticeKind::success);
        } else {
          showMessage("重新转写失败", UiNoticeKind::error);
        }
      } else if (action == UiAction::play) {
        if (audio.playing()) {
          audio.stopPlayback(usb.log());
          showMessage("已停止播放", UiNoticeKind::success);
        } else if (!capabilities.allows(kPlaybackCapabilities)) {
          showMessage("播放服务未就绪", UiNoticeKind::warning);
        } else if (!captureRouter.available() || recorder.recording()) {
          showMessage("麦克风使用中，暂时无法播放", UiNoticeKind::warning);
        } else if (tencentWorker.working()) {
          showMessage("正在转写，完成后可播放", UiNoticeKind::progress);
        } else if (!safeCapsuleFileName(selected->audioFile.c_str()) ||
                   !audio.startPlayback(
                       SD_MMC, selected->directory + "/" + selected->audioFile,
                       usb.log())) {
          showMessage("音频播放失败", UiNoticeKind::error);
        } else {
          showMessage("正在播放", UiNoticeKind::progress);
        }
      }
      drawDashboard();
    }
  }
}

bool advanceStorageBoot(uint32_t nowMs) {
  switch (storageBootPhase) {
    case StorageBootPhase::localRecovery:
      capsuleOperations->poll(nowMs);
      if (capsuleOperations->recoveryActive()) return false;
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
        bootCapsuleLibraryStarted = board.sdReady() && capsuleLibrary->begin(
            SD_MMC, usb.log(),
            !capsuleOperations->mutationCapabilityBlocked());
        if (!bootCapsuleLibraryStarted) {
          capabilities.record(DeviceCapability::capsuleLibrary, false);
          storageBootPhase = StorageBootPhase::transcription;
        }
        return false;
      }
      if (capsuleLibrary->startupActive()) {
        (void)capsuleLibrary->pollStartup(nowMs);
        return false;
      }
      bootCapsuleLibraryStarted = capsuleLibrary->startupReady();
      capabilities.record(DeviceCapability::capsuleLibrary,
                          bootCapsuleLibraryStarted);
      lastCapsuleLibraryRevision = capsuleLibrary->revision();
      storageBootPhase = StorageBootPhase::transcription;
      return false;
    case StorageBootPhase::transcription:
      bootTencentWorkerStarted = bootCapsuleLibraryStarted &&
          tencentWorker.begin(SD_MMC, capsuleLibrary.get(), deviceConfig,
                              usb.log());
      capabilities.record(DeviceCapability::transcription,
                          bootTencentWorkerStarted);
      storageBootPhase = StorageBootPhase::usbLink;
      return false;
    case StorageBootPhase::usbLink:
      // Link owns durable capsule storage. Do not start it against an
      // unmounted SD card: a failed begin() leaves deferred cleanup state that
      // would otherwise be polled ahead of touch/provisioning every turn.
      bootUsbLinkStarted = board.sdReady() && linkService->begin(
          usb.stream(), SD_MMC, board, audio, captureRouter, usb, bleVoice,
          dashboard, capsuleLibrary.get(), recorder, deviceConfig, wifi,
          tencentWorker, provisioningDiagnostics, powerDiagnostics,
          runtimePower, usb.log(), &linkCoordinator,
          LinkTransport::usb, &wirelessSync.get(),
                    nullptr, &provisioningCoordinator, nullptr,
          &captureRuntime, &captureDispatcher, &capabilities, &deviceReboot,
          &runtimeDiagnostics, startWirelessHold, stopWirelessHold);
      storageBootPhase = StorageBootPhase::wirelessLink;
      return false;
    case StorageBootPhase::wirelessLink:
      bootWifiSyncStarted = board.sdReady() && wirelessSync->begin(
          SD_MMC, board, audio, captureRouter, usb, bleVoice, dashboard,
          capsuleLibrary.get(), recorder, deviceConfig, wifi, tencentWorker,
          provisioningDiagnostics, powerDiagnostics, runtimePower,
          wirelessSyncIdentity, linkCoordinator, deviceReboot, usb.log(),
          &captureRuntime, &captureDispatcher, &capabilities);
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
      capsuleOperations->phaseName(), startupCapabilityModeName(startup.mode));
  const UiNoticeKind startupKind =
      startup.mode == StartupCapabilityMode::ready
          ? UiNoticeKind::success
          : (startup.mode == StartupCapabilityMode::unavailable
                 ? UiNoticeKind::error
                 : UiNoticeKind::warning);
  showMessage(startup.message, startupKind, 4000);
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
  const bool capsuleLibraryAllocated =
      capsuleLibrary.allocate("capsule_library", Serial);
  const bool capsuleOperationsAllocated =
      capsuleOperations.allocate("capsule_operations", Serial);
  const bool linkServiceAllocated = linkService.allocate("usb_link", Serial);
  const bool wirelessSyncAllocated =
      wirelessSync.allocate("wireless_sync", Serial);
  const bool serviceObjectsAllocated = capsuleLibraryAllocated &&
      capsuleOperationsAllocated && linkServiceAllocated &&
      wirelessSyncAllocated;
  if (!serviceObjectsAllocated) {
    psramDegradedBoot = true;
    Serial.println(
        "{\"event\":\"boot_degraded\",\"reason\":\"psram_required\","
        "\"large_services_started\":false}");
    capabilities.record(DeviceCapability::display, board.status().display);
    capabilities.record(DeviceCapability::touch, board.status().touch);
    capabilities.record(DeviceCapability::storage, false);
    (void)usb.begin(board.status().variant);
    (void)deviceConfig.begin(usb.log());
    (void)provisioningDiagnostics.begin(
        usb.log(), static_cast<uint16_t>(esp_reset_reason()));
    (void)runtimeDiagnostics.begin(
        usb.log(), static_cast<uint16_t>(esp_reset_reason()));
    dashboard.begin(board.display(), nullptr);
    showMessage("PSRAM REQUIRED · CHECK MEMORY", UiNoticeKind::error, 60000);
    drawDashboard();
    return;
  }
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
  runtimeDiagnostics.begin(usb.log(),
                          static_cast<uint16_t>(esp_reset_reason()));
  recorder.bindRuntimeDiagnostics(runtimeDiagnostics);
  runtimePower.begin(usb.log());
  const RuntimePowerSnapshot &bootPower = runtimePower.snapshot();
  powerDiagnostics.begin(
      usb.log(), static_cast<uint16_t>(esp_reset_reason()),
      bootPower.lastWakeCause, bootPower.wakeCauses,
      bootPower.ext1WakeMask, bootPower.automaticPmSupported,
      bootPower.bleModemSleepSupported, board.status().batteryPercent);
  const bool configStarted = deviceConfig.begin(usb.log());
  const bool bleStarted = bleVoice.begin(
      deviceId(), configStarted ? deviceConfig.settings().bluetoothEnabled
                                : false,
      usb.log());
  capabilities.record(DeviceCapability::bleVoice,
                      bleStarted && bootCaptureTaskStarted);
  bootSyncIdentityStarted =
      wirelessSyncIdentity.begin(ESP.getEfuseMac(), usb.log());
  storageBootAvailable = board.sdReady() &&
      capsuleOperations->begin(SD_MMC, usb.log());
  if (storageBootAvailable) {
    capsuleOperations->attachCatalog(capsuleLibrary.get());
  }
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
  provisioningCoordinator.bindRuntimeDiagnostics(runtimeDiagnostics);
  provisioningCoordinator.bindBleVoice(bleVoice);
  const StartupCapabilityPresentation startup =
      startupCapabilityPresentation(capabilities);
  const UiNoticeKind startupKind =
      startup.mode == StartupCapabilityMode::ready
          ? UiNoticeKind::success
          : (startup.mode == StartupCapabilityMode::unavailable
                 ? UiNoticeKind::error
                 : UiNoticeKind::warning);
  dashboard.begin(board.display(), board.sdReady() ? &SD_MMC : nullptr);
  if (provisioningDiagnostics.recoveredInterruptedSession()) {
    showMessage("上次配网被重启中断 · 见诊断", UiNoticeKind::warning, 5000);
  } else {
    showMessage(storageBootAvailable ? "正在恢复本地胶囊…" : startup.message,
                storageBootAvailable ? UiNoticeKind::progress : startupKind,
                4000);
  }
  drawDashboard();
  autoScreenOff.begin(millis());
  lastUsbHostConnected = usb.hostConnected();
  lastUsbSessionGeneration = usb.hostSessionSnapshot().generation;
  lastVbusPresent = board.status().vbusPresent;
  emitStatus();
  // Arduino feeds the subscribed loop task immediately before every loop()
  // call. Any synchronous storage, Wi-Fi, BLE or Link stall longer than the
  // configured five seconds therefore produces a panic coredump and reboot
  // instead of leaving a lit but permanently unresponsive device.
  enableLoopWDT();
  Serial.println("{\"event\":\"loop_task_watchdog_enabled\",\"timeout_s\":5}");
}

void loop() {
  const uint32_t now = millis();
  if (psramDegradedBoot) {
    const PowerKeyEvent powerKey = board.pollPowerKey();
    if (powerKey == PowerKeyEvent::longPress) {
      Serial.println(
          "{\"event\":\"psram_degraded_shutdown\",\"safe\":true}");
      board.safeShutdown(Serial);
    }
    if (now - lastDashboardMs >= 1000) {
      lastDashboardMs = now;
      drawDashboard();
    }
    delay(10);
    return;
  }
  // Storage boot is a real application phase. One cooperative recovery step
  // runs per turn; the initial library scan, ASR queue, Link and every local
  // mutation remain unopened until this phase reaches a terminal boundary.
  if (!advanceStorageBoot(now)) {
    // Storage recovery owns its durable authority, but it must never own the
    // whole product loop. Keep BLE, touch and provisioning responsive while
    // the bounded recovery/scan advances one primitive per turn.
    bleVoice.poll(now);
    if (now - lastTouchMs >= 16) {
      lastTouchMs = now;
      pollTouch();
    }
    provisioningCoordinator.poll(now);
    if (provisioningCoordinator.takeRestartRequired()) {
      (void)deviceReboot.requestLocal(now);
    }
    if (provisioningCoordinator.takeConfigurationChanged()) {
      dashboard.invalidate();
    }
    wifi.loop(now, false, false, board.status().charging,
              provisioningCoordinator.ownsWifi(), false);
    if (now - lastDashboardMs >= 1000) {
      lastDashboardMs = now;
      drawDashboard();
    }
    return;
  }
  capsuleOperations->poll(now);
  consumeLocalOperationOutcome();
  // Link reboot is a device-lifecycle request, not a transport operation.
  // Advance it before any transport-specific early return so a client that
  // closes USB/Wi-Fi immediately after reboot OK cannot cancel the intent.
  if (deviceReboot.pending()) {
    if (captureRuntime.running() && pendingCaptureStop == PendingCaptureStop::none) {
      const PendingCaptureStop ownerStop =
          captureRouter.owner() == AudioCaptureOwner::wirelessVoice
              ? PendingCaptureStop::wirelessVoice
              : PendingCaptureStop::localCapsule;
      (void)requestCaptureStop(ownerStop, RecorderStopReason::none, true,
                               ownerStop == PendingCaptureStop::localCapsule);
    }
    if (deviceReboot.due(now) &&
        deviceReboot.phase() == DeviceRebootPhase::accepted) {
      (void)tencentWorker.beginQuiesce(
          now, 250, TencentCancelReason::shutdown);
      (void)deviceReboot.beginServiceQuiesce();
    }
    if (deviceReboot.due(now) &&
        deviceReboot.phase() == DeviceRebootPhase::waitingForServices) {
      // Do not commit a just-finished cloud result from the Link poll. Startup
      // recovery will requeue the durable transcribing capsule after restart.
      (void)tencentWorker.abandonResultForReboot();
      const TencentQuiesceStatus status = tencentWorker.pollQuiesce(now);
      if (status == TencentQuiesceStatus::waiting) {
        deviceReboot.defer(now);
      } else if (status == TencentQuiesceStatus::timedOut) {
        deviceReboot.retryServiceQuiesce(now);
      } else {
        (void)deviceReboot.markReady();
      }
    }
    const bool quiesceReady = deviceReboot.ready();
    const bool restartSafe = quiesceReady &&
        captureRouter.owner() == AudioCaptureOwner::none &&
        !captureRuntime.running() && !recorder.operationActive() &&
        !localRecordingStart.active() && !linkService->receivingBinary() &&
        !linkService->maintenanceActive() &&
        linkService->deviceLifecycleRestartReady() &&
        wirelessSync->deviceLifecycleRestartReady() &&
        !wirelessSync->linkBusy() &&
        !capsuleLibrary->scanActive() && !capsuleOperations->busy() &&
        !tencentWorker.working() && StorageCoordinator::instance().idle();
    if (restartSafe) {
      usb.log().println("{\"event\":\"link_reboot_execute\"}");
      deviceReboot.acknowledgeRestart();
#if defined(ARDUINO_ARCH_ESP32)
      ESP.restart();
#endif
      return;
    }
    if (quiesceReady) deviceReboot.defer(now);
  }
  // These polls precede every transport/UI early return. Physical File close
  // and late capture finalization therefore always make bounded progress.
  pollDeferredServiceCleanup();
  if (safeShutdownQuiesce.pending()) (void)advanceSafeShutdown(now);
  const bool usbHostConnected = usb.hostConnected();
  const UsbCdcSessionSnapshot usbSession = usb.hostSessionSnapshot();
  uint32_t closedUsbSessionGeneration = 0;
  const bool usbHostSessionClosed =
      usb.takeHostSessionClosed(closedUsbSessionGeneration);
  const bool usbSessionAdvanced = usbSession.generation != 0 &&
      usbSession.generation != lastUsbSessionGeneration;
  const bool currentUsbSessionClosed = usbHostSessionClosed &&
      !usbSession.active &&
      closedUsbSessionGeneration == usbSession.generation;
  const bool usbPhysicallyDisconnected =
      lastUsbHostConnected && !usbHostConnected;
  const UsbLinkSessionAction usbSessionAction = usbLinkSessionAction(
      bootUsbLinkStarted, usbPhysicallyDisconnected, currentUsbSessionClosed,
      usbSessionAdvanced, usbSession.generation,
      linkService->usbHostSessionGeneration(), lastUsbSessionGeneration);
  if (usbSessionAction == UsbLinkSessionAction::disconnectAndDiscard) {
    usb.discardHostSessionBuffers();
    linkService->disconnect();
  } else if (usbSessionAction ==
             UsbLinkSessionAction::disconnectRetainingNewBytes) {
    // A fast close/reopen can happen entirely between two loop turns. Reset
    // the old Link owner, but retain bytes already received for the new DTR
    // generation. A stale close event must never discard the new request.
    linkService->disconnect();
  }
  lastUsbHostConnected = usbHostConnected;
  lastUsbSessionGeneration = usbSession.generation;
  if (trashUndo.expire(now)) {
    dashboard.invalidate();
  }

  // A CDC upload can otherwise overrun TinyUSB while a full-screen AMOLED
  // redraw or an SD/network task owns the main loop. Once a binary request has
  // started, drain it before doing any optional UI or sensor work.
  if (bootUsbLinkStarted && board.sdReady()) {
    wirelessSync->enforceDeadline(now);
  }
  if (bootUsbLinkStarted && linkService->receivingBinary()) {
    linkService->poll(now);
    return;
  }
  if (bootWifiSyncStarted && board.sdReady() && wirelessSync->receivingBinary()) {
    wirelessSync->poll(now, wifi.connected());
    return;
  }
  const bool bootChanged = bootButton.update(digitalRead(kBootButtonPin) == LOW,
                                             now);
  if (bootChanged && bootButton.pressedEdge()) {
    noteUserActivity(now);
    applyBootGestureAction(bootGesturePolicy.pressed(now, bootGestureContext()),
                           now);
  }
  if (bootButton.pressed()) {
    applyBootGestureAction(bootGesturePolicy.held(now, bootGestureContext()),
                           now);
  }
  if (bootChanged && bootButton.releasedEdge()) {
    noteUserActivity(now);
    applyBootGestureAction(
        bootGesturePolicy.released(bootGestureContext()), now);
  }

  if (captureRuntime.running() && wirelessCaptureStart.active()) {
    advanceWirelessCaptureStart(now);
  }
  if (audio.playing()) {
    audio.pumpPlayback(usb.log());
  } else if (captureRuntime.running() && !wirelessCaptureStart.active()) {
    const bool wasRecording = recorder.recording();
    const AudioCaptureDispatchResult dispatch = drainCapturedAudio(now);
    if (!dispatch.ok) {
      if (dispatch.failedRecorderOwner == RecorderOperationOwner::localApp) {
        (void)requestCaptureStop(PendingCaptureStop::localCapsule,
                                 RecorderStopReason::none, true, true);
        showMessage("录音已中断", UiNoticeKind::error);
      } else if (dispatch.voiceDeliveryFailure ||
                 captureRouter.wirelessStreaming()) {
        (void)requestCaptureStop(PendingCaptureStop::wirelessVoice,
                                 RecorderStopReason::none, true, false);
        showMessage("无线语音已中断", UiNoticeKind::error);
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
  if (bleVoice.callbackOverflowRecoveryRequired()) {
    // A valid frozen epoch reached the hard deadline without a physical
    // disconnect confirmation. Do not fabricate confirmation or reopen the
    // callback mailboxes. Finish the current capture first, then perform one
    // controlled MCU restart after all durable work has become idle; boot
    // restores the persisted Bluetooth intent.
    if (captureRuntime.running() &&
        captureRouter.owner() == AudioCaptureOwner::wirelessVoice &&
        pendingCaptureStop == PendingCaptureStop::none) {
      (void)requestCaptureStop(PendingCaptureStop::wirelessVoice,
                               RecorderStopReason::none, true, false);
    }
    const bool restartSafe =
        pendingCaptureStop == PendingCaptureStop::none &&
        captureRouter.owner() == AudioCaptureOwner::none &&
        !captureRuntime.running() && !recorder.operationActive() &&
        !localRecordingStart.active() && !linkService->receivingBinary() &&
        !linkService->maintenanceActive() && !wirelessSync->linkBusy() &&
        !capsuleLibrary->scanActive() && !capsuleOperations->busy() &&
        !tencentWorker.working() && StorageCoordinator::instance().idle();
    if (restartSafe && bleVoice.claimCallbackOverflowRecoveryRestart()) {
      usb.log().println(
          "{\"event\":\"ble_voice_callback_overflow_recovery_restart\"}");
#if defined(ARDUINO_ARCH_ESP32)
      ESP.restart();
#endif
    }
  }
  if (bleVoice.appHandshakeRecoveryRequired()) {
    // The host did not confirm the exact physical disconnect after repeated
    // requests. Keep the old epoch closed and restart only after all durable
    // work is idle; never fabricate a disconnect or restart advertising over
    // a connection the controller may still own.
    const bool restartSafe =
        pendingCaptureStop == PendingCaptureStop::none &&
        captureRouter.owner() == AudioCaptureOwner::none &&
        !captureRuntime.running() && !recorder.operationActive() &&
        !localRecordingStart.active() && !linkService->receivingBinary() &&
        !linkService->maintenanceActive() && !wirelessSync->linkBusy() &&
        !capsuleLibrary->scanActive() && !capsuleOperations->busy() &&
        !tencentWorker.working() && StorageCoordinator::instance().idle();
    if (restartSafe && bleVoice.claimAppHandshakeRecoveryRestart()) {
      usb.log().println(
          "{\"event\":\"ble_voice_handshake_recovery_restart\"}");
#if defined(ARDUINO_ARCH_ESP32)
      ESP.restart();
#endif
    }
  }
  if (bleVoice.sessionStopRequested()) {
    if (captureRouter.owner() == AudioCaptureOwner::wirelessVoice) {
      (void)requestCaptureStop(PendingCaptureStop::wirelessVoice,
                               RecorderStopReason::none, false, false);
      if (pendingCaptureStop == PendingCaptureStop::wirelessVoice ||
          captureRouter.owner() != AudioCaptureOwner::wirelessVoice) {
        bleVoice.acknowledgeSessionStopRequest();
      }
    } else {
      bleVoice.acknowledgeSessionStopRequest();
    }
  }
  if (wirelessUiActive && !wirelessCaptureStart.active() &&
      !bleVoice.streaming()) {
    (void)requestCaptureStop(PendingCaptureStop::wirelessVoice,
                             RecorderStopReason::none, true, false);
  }
  if (captureRouter.available() && !captureRuntime.running() &&
      !audio.playing() && audio.active()) {
    audio.stopHardware(usb.log());
  }

  if (bootUsbLinkStarted) {
    wirelessSync->enforceDeadline(now);
    linkService->poll(now);
  }
  if (bootUsbLinkStarted && linkService->receivingBinary()) {
    return;
  }

  // Touch has priority over captive-portal HTTP. A queued back/exit gesture is
  // consumed before the phone can start another bounded request.
  if (now - lastTouchMs >= currentPowerDecision.touchPollMs) {
    lastTouchMs = now;
    pollTouch();
  }
  provisioningCoordinator.poll(now);
  if (provisioningCoordinator.takeRestartRequired()) {
    (void)deviceReboot.requestLocal(now);
    showMessage("配网已退出，正在恢复蓝牙…", UiNoticeKind::progress, 3000);
  }
  if (provisioningCoordinator.takeConfigurationChanged()) {
    tencentWorker.wake();
    dashboard.invalidate();
  }
  const bool networkWork = !lowBatteryShutdown.critical() &&
      capsuleLibrary->pendingCount() > 0 &&
      deviceConfig.hasTencent() && !tencentWorker.waitingForWake();
  const bool audioCaptureExclusive =
      captureRouter.owner() != AudioCaptureOwner::none &&
      linkCoordinator.owner() != LinkTransport::wifi;
  wifi.loop(now, recorder.operationActive(), networkWork,
            board.status().charging, provisioningCoordinator.ownsWifi(),
            wirelessSync->wifiDemand(), audioCaptureExclusive);
  if (bootWifiSyncStarted && board.sdReady()) {
    wirelessSync->poll(now, wifi.connected());
  }
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
  if (!tencentWorker.working()) capsuleLibrary->pollScan();
  const uint32_t capsuleRevision = capsuleLibrary->revision();
  if (capsuleRevision != lastCapsuleLibraryRevision) {
    lastCapsuleLibraryRevision = capsuleRevision;
    dashboard.invalidate();
  }
  if (!deviceReboot.pending()) {
    tencentWorker.loop(now, wifi.connected(), wifi.timeReady(),
                       transcriptionDispatchBusy(recorder.operationActive(),
                                                 linkService->maintenanceActive() ||
                                                     wirelessSync->linkBusy() ||
                                                     capsuleLibrary->scanActive() ||
                                                     capsuleOperations->busy()) ||
                           lowBatteryShutdown.critical(),
                       board.status().charging);
  }
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
      currentPowerDecision.requestDeepSleep &&
      bleVoice.quiescedForSleep() &&
      !wifi.radioOn()) {
    enterDeepSleep(finalPowerInputs);
  }
  if (dashboard.advanceVerticalScroll(now, capsuleLibrary.get())) {
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
      capsuleOperations->busy() ||
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
