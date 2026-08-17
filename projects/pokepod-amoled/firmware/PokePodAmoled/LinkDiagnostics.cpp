#include "LinkDiagnostics.h"

#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include "AudioPipeline.h"
#include "BleVoiceService.h"
#include "BoardServices.h"
#include "CapabilityRegistry.h"
#include "CapsuleLibrary.h"
#include "Dashboard.h"
#include "DeviceConfig.h"
#include "LinkFrame.h"
#include "PowerDiagnostics.h"
#include "ProvisioningCoordinator.h"
#include "ProvisioningDiagnostics.h"
#include "RuntimePowerManager.h"
#include "RuntimeDiagnostics.h"
#include "TencentWorker.h"
#include "UsbLinkBridge.h"
#include "WavRecorder.h"
#include "WifiController.h"
#include "WifiFailurePolicy.h"

namespace pokepod {
namespace {

constexpr size_t kStatusDiagnosticStringBytes = 192U;
constexpr char kStatusPrefix[] = "{\"status\":\"ok\",\"version\":2";

String printed(cJSON *root) {
  char *value = cJSON_PrintUnformatted(root);
  const String result = value == nullptr ? String() : String(value);
  cJSON_free(value);
  return result;
}

String utf8Prefix(const String &value, size_t maximumBytes) {
  if (value.length() <= maximumBytes) return value;
  if (maximumBytes <= 3) return String();
  const size_t payloadBytes = maximumBytes - 3;
  size_t offset = 0;
  while (offset < value.length()) {
    const uint8_t lead = static_cast<uint8_t>(value[offset]);
    size_t characterBytes = 1;
    if ((lead & 0xE0U) == 0xC0U) characterBytes = 2;
    else if ((lead & 0xF0U) == 0xE0U) characterBytes = 3;
    else if ((lead & 0xF8U) == 0xF0U) characterBytes = 4;
    if (offset + characterBytes > value.length() ||
        offset + characterBytes > payloadBytes) {
      break;
    }
    bool continuationValid = true;
    for (size_t index = 1; index < characterBytes; ++index) {
      if ((static_cast<uint8_t>(value[offset + index]) & 0xC0U) != 0x80U) {
        continuationValid = false;
        break;
      }
    }
    if (!continuationValid) characterBytes = 1;
    offset += characterBytes;
  }
  return value.substring(0, offset) + "...";
}

void appendJsonKey(String &json, const char *key) {
  if (!json.isEmpty()) json += ',';
  json += '"';
  json += key;
  json += "\":";
}

void appendJsonBool(String &json, const char *key, bool value) {
  appendJsonKey(json, key);
  json += value ? "true" : "false";
}

void appendJsonNumber(String &json, const char *key, int64_t value) {
  appendJsonKey(json, key);
  char encoded[24];
  snprintf(encoded, sizeof(encoded), "%lld",
           static_cast<long long>(value));
  json += encoded;
}

void appendJsonString(String &json, const char *key, const String &value) {
  appendJsonKey(json, key);
  json += '"';
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    switch (character) {
      case '\\': json += "\\\\"; break;
      case '"': json += "\\\""; break;
      case '\n': json += "\\n"; break;
      case '\r': json += "\\r"; break;
      case '\t': json += "\\t"; break;
      default:
        if (static_cast<uint8_t>(character) >= 0x20) json += character;
        break;
    }
  }
  json += '"';
}

}  // namespace

void LinkDiagnostics::bind(
    BoardServices &board, AudioPipeline &audio, UsbLinkBridge &usb,
    BleVoiceService &bleVoice, Dashboard &dashboard, CapsuleLibrary &library,
    WavRecorder &recorder, DeviceConfig &config, WifiController &wifi,
    TencentWorker &tencent,
    ProvisioningDiagnostics &provisioningDiagnostics,
    PowerDiagnostics &powerDiagnostics, RuntimePowerManager &power,
    ProvisioningCoordinator *provisioningCoordinator,
    const CapabilityRegistry *capabilities,
    RuntimeDiagnostics *runtimeDiagnostics) {
  board_ = &board;
  audio_ = &audio;
  usb_ = &usb;
  bleVoice_ = &bleVoice;
  dashboard_ = &dashboard;
  library_ = &library;
  recorder_ = &recorder;
  config_ = &config;
  wifi_ = &wifi;
  tencent_ = &tencent;
  provisioningDiagnostics_ = &provisioningDiagnostics;
  powerDiagnostics_ = &powerDiagnostics;
  power_ = &power;
  provisioningCoordinator_ = provisioningCoordinator;
  capabilities_ = capabilities;
  runtimeDiagnostics_ = runtimeDiagnostics;
  // Reserve once while boot still has a large contiguous internal heap. The
  // buffer is retained for the service lifetime and reused for every status
  // response instead of allocating/freeing 3-4 KiB on each host query.
  (void)statusBuffer_.reserve(kLinkMaxControlBytes);
}

const String &LinkDiagnostics::statusJson() const {
  if (board_ == nullptr || audio_ == nullptr || usb_ == nullptr ||
      bleVoice_ == nullptr || dashboard_ == nullptr || library_ == nullptr ||
      recorder_ == nullptr || config_ == nullptr || wifi_ == nullptr ||
      tencent_ == nullptr || provisioningDiagnostics_ == nullptr ||
      powerDiagnostics_ == nullptr || power_ == nullptr) {
    statusBuffer_ = "{\"status\":\"error\",\"version\":2,"
                    "\"message\":\"diagnostics unavailable\"}";
    return statusBuffer_;
  }

  const BoardStatus &status = board_->status();
  String &extra = statusBuffer_;
  extra = kStatusPrefix;
  appendJsonBool(extra, "recording", recorder_->recording());
  appendJsonBool(extra, "transcribing", tencent_->working());
  appendJsonNumber(extra, "batteryPercent", status.batteryPercent);
  appendJsonBool(extra, "charging", status.charging);
  appendJsonString(extra, "wifi", wifi_->phaseName());
  appendJsonNumber(extra, "wifiDisconnectReason",
                   wifi_->lastDisconnectReason());
  appendJsonString(extra, "wifiDisconnectKind",
                   wifiFailureKey(wifi_->lastDisconnectReason()));
  appendJsonBool(extra, "wifiRadioOn", wifi_->radioOn());
  appendJsonBool(extra, "wifiPowerSave", wifi_->powerSaveEnabled());
  appendJsonNumber(extra, "wifiPowerSaveError", wifi_->powerSaveError());
  appendJsonNumber(extra, "pendingCapsules", library_->pendingCount());
  appendJsonNumber(extra, "asr_hash_ms", tencent_->lastHashElapsedMs());
  appendJsonNumber(extra, "asr_connect_ms",
                   tencent_->lastConnectElapsedMs());
  appendJsonNumber(extra, "asr_upload_ms", tencent_->lastUploadElapsedMs());
  appendJsonNumber(extra, "asr_total_ms", tencent_->lastTotalElapsedMs());
  appendJsonString(extra, "asr_last_code", tencent_->lastCode());
  appendJsonNumber(extra, "asr_tls_error", tencent_->lastNetworkError());
  appendJsonString(extra, "asr_tls_detail",
                   utf8Prefix(tencent_->lastNetworkErrorDetail(),
                              kStatusDiagnosticStringBytes));
  appendJsonNumber(extra, "asr_heap_free_before_tls",
                   tencent_->lastInternalHeapFreeBeforeTls());
  appendJsonNumber(extra, "asr_heap_largest_before_tls",
                   tencent_->lastInternalHeapLargestBeforeTls());
  appendJsonNumber(extra, "asr_psram_free_before_tls",
                   tencent_->lastPsramFreeBeforeTls());
  appendJsonBool(extra, "sdReady", status.sdCard);
  if (capabilities_ != nullptr) {
    appendJsonNumber(extra, "capabilityObservedMask",
                     capabilities_->observedMask());
    appendJsonNumber(extra, "capabilityReadyMask", capabilities_->readyMask());
    appendJsonBool(extra, "capsuleLibraryReady",
                   capabilities_->ready(DeviceCapability::capsuleLibrary));
    appendJsonBool(extra, "recorderReady",
                   capabilities_->ready(DeviceCapability::recording));
    appendJsonBool(extra, "asrWorkerReady",
                   capabilities_->ready(DeviceCapability::transcription));
  }
  appendJsonString(extra, "variant", variantName(status.variant));
  appendJsonBool(extra, "ioExpander", status.ioExpander);
  appendJsonBool(extra, "display", status.display);
  appendJsonBool(extra, "touch", status.touch);
  appendJsonBool(extra, "rtc", status.rtc);
  appendJsonBool(extra, "imu", status.imu);
  appendJsonBool(extra, "pmu", status.pmu);
  appendJsonBool(extra, "vbusPresent", status.vbusPresent);
  appendJsonBool(extra, "screenOn", status.screenOn);
  appendJsonBool(extra, "audio", audio_->ready());
  appendJsonBool(extra, "usb", usb_->ready());
  appendJsonBool(extra, "host_connected", usb_->hostConnected());
  appendJsonBool(extra, "bleVoiceConnected", bleVoice_->connected());
  appendJsonBool(extra, "bleVoiceReady", bleVoice_->appReady());
  appendJsonNumber(extra, "bleVoiceMtu", bleVoice_->mtu());
  const BleVoiceQualitySnapshot bleQuality = bleVoice_->quality();
  appendJsonNumber(extra, "bleVoiceNotifyAttempts", bleQuality.notifyAttempts);
  appendJsonNumber(extra, "bleVoiceNotifyAccepted", bleQuality.notifyAccepted);
  appendJsonNumber(extra, "bleVoiceNotifyFailures", bleQuality.notifyFailures);
  appendJsonNumber(extra, "bleVoiceQueueOverflows", bleQuality.queueOverflows);
  appendJsonNumber(extra, "bleVoiceSessionFailures", bleQuality.sessionFailures);
  appendJsonNumber(extra, "bleVoiceReadyTimeouts", bleQuality.readyTimeouts);
  appendJsonNumber(extra, "bleVoiceStopAckTimeouts",
                   bleQuality.stopAckTimeouts);
  appendJsonNumber(extra, "bleVoiceStreamTimeouts", bleQuality.streamTimeouts);
  appendJsonNumber(extra, "bleVoiceLastErrorCode", bleQuality.lastErrorCode);
  appendJsonNumber(extra, "audio_read_bytes", audio_->bytesRead());
  appendJsonNumber(extra, "audio_read_failures", audio_->readFailures());
  appendJsonNumber(extra, "audio_peak", audio_->peakSample());
  appendJsonString(extra, "audio_capture_last_hardware_error",
                   utf8Prefix(audio_->lastHardwareError(),
                              kStatusDiagnosticStringBytes));
  appendJsonNumber(extra, "audio_capture_heap_free_before_start",
                   audio_->captureHeapFreeBeforeStart());
  appendJsonNumber(extra, "audio_capture_heap_largest_before_start",
                   audio_->captureHeapLargestBeforeStart());
  appendJsonString(extra, "playback_last_error",
                   utf8Prefix(audio_->lastPlaybackError(),
                              kStatusDiagnosticStringBytes));
  appendJsonNumber(extra, "playback_start_failures",
                   audio_->playbackStartFailures());
  appendJsonNumber(extra, "playback_heap_largest_before_start",
                   audio_->playbackHeapLargestBeforeStart());
  appendJsonNumber(extra, "playback_file_reads",
                   audio_->playbackFileReadCount());
  appendJsonNumber(extra, "playback_pump_count",
                   audio_->playbackPumpCount());
  appendJsonNumber(extra, "playback_max_file_read_us",
                   audio_->playbackMaxFileReadUs());
  const AudioFrontEndMetrics &frontEnd = recorder_->audioMetrics();
  appendJsonString(extra, "audio_frontend_channel",
                   audioInputChannelName(frontEnd.selectedChannel));
  appendJsonNumber(extra, "audio_frontend_left_peak", frontEnd.leftPeak);
  appendJsonNumber(extra, "audio_frontend_right_peak", frontEnd.rightPeak);
  appendJsonNumber(extra, "audio_frontend_output_peak", frontEnd.outputPeak);
  appendJsonNumber(extra, "audio_frontend_noise_floor",
                   frontEnd.estimatedNoiseFloor);
  appendJsonNumber(extra, "audio_frontend_suppressed_samples",
                   frontEnd.suppressedSamples);
  appendJsonNumber(extra, "audio_frontend_limited_samples",
                   frontEnd.limitedSamples);
  appendJsonNumber(extra, "audio_frontend_max_gain_q12",
                   frontEnd.maximumGainQ12);
  appendJsonBool(extra, "tencentConfigured", config_->hasTencent());
  appendJsonNumber(extra, "wifiNetworkCount", config_->wifiNetworks().size());
  appendJsonNumber(extra, "ui_full_redraws", dashboard_->fullRedrawCount());
  appendJsonNumber(extra, "ui_body_redraws", dashboard_->bodyRedrawCount());
  appendJsonNumber(extra, "ui_partial_redraws",
                   dashboard_->partialRedrawCount());
  appendJsonNumber(extra, "ui_scroll_frame_last_us",
                   dashboard_->scrollFrameLastUs());
  appendJsonNumber(extra, "ui_scroll_frame_max_us",
                   dashboard_->scrollFrameMaxUs());
  appendJsonNumber(extra, "ui_scroll_compose_max_us",
                   dashboard_->scrollComposeMaxUs());
  appendJsonNumber(extra, "ui_scroll_transfer_max_us",
                   dashboard_->scrollTransferMaxUs());
  appendJsonNumber(extra, "ui_scroll_frames_over_budget",
                   dashboard_->scrollFramesOverBudget());
  appendJsonNumber(extra, "capsule_full_scans", library_->fullScanCount());
  appendJsonNumber(extra, "capsule_incremental_refreshes",
                   library_->incrementalRefreshCount());
  appendJsonNumber(extra, "capsule_refresh_fallbacks",
                   library_->refreshFallbackCount());
  appendJsonNumber(extra, "capsule_last_scan_us", library_->lastScanUs());
  appendJsonNumber(extra, "capsule_max_scan_us", library_->maxScanUs());
  appendJsonBool(extra, "ui_frame_buffer", dashboard_->frameBufferReady());
  appendJsonBool(extra, "ui_animation_buffer",
                 dashboard_->animationBufferReady());
  const RuntimePowerSnapshot &power = power_->snapshot();
  appendJsonString(extra, "powerMode", powerModeName(power.mode));
  appendJsonNumber(extra, "cpuMhz", power.cpuMhz);
  appendJsonNumber(extra, "powerTransitions", power.transitions);
  appendJsonNumber(extra, "lightSleepAttempts", power.lightSleepAttempts);
  appendJsonNumber(extra, "lightSleepCount", power.lightSleepCount);
  appendJsonNumber(extra, "lightSleepFailures", power.lightSleepFailures);
  appendJsonNumber(extra, "lightSleepMs", power.lightSleepUs / 1000ULL);
  appendJsonNumber(extra, "deepSleepWakeCount", power.deepSleepWakeCount);
  appendJsonNumber(extra, "deepSleepArmAttempts", power.deepSleepArmAttempts);
  appendJsonNumber(extra, "deepSleepArmFailures", power.deepSleepArmFailures);
  appendJsonBool(extra, "wokeFromDeepSleep", power.wokeFromDeepSleep);
  appendJsonBool(extra, "deepSleepTouchWakeArmed",
                 power.deepSleepTouchWakeArmed);
  appendJsonNumber(extra, "lastWakeCause", power.lastWakeCause);
  appendJsonNumber(extra, "wakeCauses", power.wakeCauses);
  const PowerDiagnosticsSnapshot &powerDiagnostic = powerDiagnostics_->snapshot();
  appendJsonNumber(extra, "powerActiveFacts", powerDiagnostic.activeFacts);
  appendJsonNumber(extra, "powerLightBlockers", powerDiagnostic.lightBlockers);
  appendJsonNumber(extra, "powerDeepBlockers", powerDiagnostic.deepBlockers);
  appendJsonNumber(extra, "powerCurrentBlockers",
                   powerDiagnostic.currentBlockers);
  appendJsonNumber(extra, "powerIdleMs", powerDiagnostic.idleMs);
  appendJsonNumber(extra, "powerDiagnosticCount", powerDiagnostics_->count());
  appendJsonNumber(extra, "powerDiagnosticPersistFailures",
                   powerDiagnostic.persistFailures);
  appendJsonNumber(extra, "powerAutomaticScreenWakes",
                   powerDiagnostic.automaticScreenWakes);
  appendJsonNumber(extra, "resetReason", esp_reset_reason());
  appendJsonNumber(extra, "internalHeapFree",
                   heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                           MALLOC_CAP_8BIT));
  appendJsonNumber(extra, "internalHeapLargest",
                   heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL |
                                                    MALLOC_CAP_8BIT));
  appendJsonNumber(extra, "psramFree", ESP.getFreePsram());
  appendJsonBool(extra, "automaticPmSupported", power.automaticPmSupported);
  appendJsonBool(extra, "bleModemSleepSupported",
                 power.bleModemSleepSupported);
  appendJsonNumber(extra, "provisioningDiagnosticCount",
                   provisioningDiagnostics_->count());
  appendJsonNumber(extra, "runtimeDiagnosticCount",
                   runtimeDiagnostics_ == nullptr ? 0
                                                   : runtimeDiagnostics_->count());
  appendJsonString(extra, "provisioningStartupPhase",
                   provisioningCoordinator_ == nullptr
                       ? "unavailable"
                       : provisioningCoordinator_->phaseName());
  if (extra.length() + 1U > kLinkMaxControlBytes) {
    extra = kStatusPrefix;
    appendJsonBool(extra, "diagnosticsTruncated", true);
    appendJsonBool(extra, "recording", recorder_->recording());
    appendJsonBool(extra, "transcribing", tencent_->working());
    appendJsonNumber(extra, "batteryPercent", status.batteryPercent);
    appendJsonString(extra, "wifi", wifi_->phaseName());
    appendJsonBool(extra, "sdReady", status.sdCard);
  }
  extra += '}';
  return statusBuffer_;
}

String LinkDiagnostics::provisioningJson() const {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "status", "ok");
  cJSON_AddNumberToObject(root, "version", kLinkVersion);
  cJSON_AddNumberToObject(root, "schemaVersion", 1);
  cJSON *records = cJSON_AddArrayToObject(root, "records");
  if (provisioningDiagnostics_ != nullptr) {
    for (size_t index = 0; index < provisioningDiagnostics_->count(); ++index) {
      const StoredProvisioningLogRecord *record =
          provisioningDiagnostics_->newest(index);
      if (record == nullptr) continue;
      cJSON *item = cJSON_CreateObject();
      cJSON_AddNumberToObject(item, "sequence", record->sequence);
      cJSON_AddNumberToObject(item, "epoch", record->epoch);
      cJSON_AddNumberToObject(item, "elapsedMs", record->elapsedMs);
      cJSON_AddStringToObject(item, "stage", provisioningLogStageKey(
          static_cast<ProvisioningLogStage>(record->stage)));
      cJSON_AddNumberToObject(item, "outcome", record->outcome);
      cJSON_AddNumberToObject(item, "attempt", record->attempt);
      cJSON_AddStringToObject(item, "ssid", record->ssid);
      cJSON_AddNumberToObject(item, "rssi", record->rssi);
      cJSON_AddNumberToObject(item, "reason", record->reason);
      cJSON_AddStringToObject(item, "reasonKind",
                              wifiFailureKey(record->reason));
      cJSON_AddItemToArray(records, item);
    }
  }
  const String result = printed(root);
  cJSON_Delete(root);
  return result;
}

String LinkDiagnostics::powerJson() const {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "status", "ok");
  cJSON_AddNumberToObject(root, "version", kLinkVersion);
  cJSON_AddNumberToObject(root, "schemaVersion", 1);
  cJSON *blockerKeys = cJSON_AddArrayToObject(root, "blockerKeys");
  for (uint8_t bit = 0;
       bit <= static_cast<uint8_t>(PowerBlocker::beforeDeepTimeout); ++bit) {
    cJSON_AddItemToArray(blockerKeys, cJSON_CreateString(powerBlockerKey(
        static_cast<PowerBlocker>(bit))));
  }
  cJSON *records = cJSON_AddArrayToObject(root, "records");
  if (powerDiagnostics_ != nullptr) {
    for (size_t index = 0; index < powerDiagnostics_->count(); ++index) {
      const StoredPowerLogRecord *record = powerDiagnostics_->newest(index);
      if (record == nullptr) continue;
      cJSON *item = cJSON_CreateObject();
      cJSON_AddNumberToObject(item, "sequence", record->sequence);
      cJSON_AddNumberToObject(item, "epoch", record->epoch);
      cJSON_AddNumberToObject(item, "uptimeMs", record->uptimeMs);
      cJSON_AddNumberToObject(item, "durationMs", record->durationMs);
      cJSON_AddStringToObject(item, "event", powerLogEventKey(
          static_cast<PowerLogEvent>(record->event)));
      cJSON_AddStringToObject(item, "mode", powerModeName(
          static_cast<PowerMode>(record->mode)));
      cJSON_AddNumberToObject(item, "blockerMask", record->blockers);
      cJSON_AddNumberToObject(item, "detail", record->detail);
      char wakeMask[19];
      snprintf(wakeMask, sizeof(wakeMask), "%016llx",
               static_cast<unsigned long long>(record->ext1WakeMask));
      cJSON_AddStringToObject(item, "wakeMask", wakeMask);
      cJSON_AddNumberToObject(item, "error", record->error);
      cJSON_AddNumberToObject(item, "flags", record->flags);
      cJSON_AddNumberToObject(item, "resetReason", record->resetReason);
      cJSON_AddNumberToObject(item, "wakeCause", record->wakeCause);
      cJSON_AddNumberToObject(item, "batteryPercent",
                              record->batteryPercent);
      cJSON_AddItemToArray(records, item);
    }
  }
  const String result = printed(root);
  cJSON_Delete(root);
  return result;
}

String LinkDiagnostics::runtimeJson() const {
  if (runtimeDiagnostics_ == nullptr) {
    return "{\"status\":\"unavailable\",\"version\":1,\"records\":[]}";
  }
  return runtimeDiagnostics_->json();
}

String LinkDiagnostics::runtimeTraceJson(size_t newestOffset,
                                         size_t limit) const {
  if (runtimeDiagnostics_ == nullptr) {
    return "{\"status\":\"unavailable\",\"version\":1,\"records\":[]}";
  }
  return runtimeDiagnostics_->traceJson(newestOffset, limit);
}

bool LinkDiagnostics::clearProvisioning(Print &log) const {
  return provisioningDiagnostics_ != nullptr &&
      provisioningDiagnostics_->clear(log);
}

bool LinkDiagnostics::clearPower(Print &log) const {
  return powerDiagnostics_ != nullptr && powerDiagnostics_->clear(log);
}

bool LinkDiagnostics::clearRuntime(Print &log) const {
  return runtimeDiagnostics_ != nullptr && runtimeDiagnostics_->clear(log);
}

}  // namespace pokepod
