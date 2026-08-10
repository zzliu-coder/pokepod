#include "PokePodLinkService.h"

#include "WifiFailurePolicy.h"

#include <SD_MMC.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <mbedtls/sha256.h>
#include <algorithm>
#include <vector>

#include "AudioPipeline.h"
#include "AudioCaptureRouter.h"
#include "BoardServices.h"
#include "CapsuleLibrary.h"
#include "CapsulePolicy.h"
#include "DeviceConfig.h"
#include "Dashboard.h"
#include "FontPolicy.h"
#include "TencentWorker.h"
#include "UsbLinkBridge.h"
#include "BleVoiceService.h"
#include "ProvisioningDiagnostics.h"
#include "ProvisioningCoordinator.h"
#include "RuntimePowerManager.h"
#include "WavRecorder.h"
#include "WifiController.h"
#include "WirelessSyncIdentityBlob.h"
#include "WirelessSyncPairing.h"
#include "WirelessSyncProtocol.h"

namespace pokepod {
namespace {

constexpr uint32_t kMaxIncomingCommandBytes = 1024 * 1024;
constexpr size_t kMaxCommandJsonBytes = kMaxIncomingCommandBytes;
constexpr size_t kReadPageFiles = 12;

cJSON *asJson(void *value) { return static_cast<cJSON *>(value); }

const char *jsonString(cJSON *root, const char *name) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  return cJSON_IsString(item) && item->valuestring != nullptr
      ? item->valuestring : nullptr;
}

bool jsonBool(cJSON *root, const char *name, bool fallback = false) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  return cJSON_IsBool(item) ? cJSON_IsTrue(item) : fallback;
}

int64_t jsonInt64(cJSON *root, const char *name, int64_t fallback = -1) {
  cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  return cJSON_IsNumber(item) ? static_cast<int64_t>(item->valuedouble) : fallback;
}

String printed(cJSON *root) {
  char *value = cJSON_PrintUnformatted(root);
  const String result = value == nullptr ? String() : String(value);
  cJSON_free(value);
  return result;
}

String jsonEscaped(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t index = 0; index < value.length(); ++index) {
    const char character = value[index];
    switch (character) {
      case '\\': escaped += "\\\\"; break;
      case '"': escaped += "\\\""; break;
      case '\n': escaped += "\\n"; break;
      case '\r': escaped += "\\r"; break;
      case '\t': escaped += "\\t"; break;
      default:
        if (static_cast<uint8_t>(character) >= 0x20) escaped += character;
        break;
    }
  }
  return escaped;
}

void replaceStringOrNull(cJSON *root, const char *name, const char *value) {
  cJSON *replacement = value == nullptr ? cJSON_CreateNull()
                                         : cJSON_CreateString(value);
  if (cJSON_HasObjectItem(root, name)) {
    cJSON_ReplaceItemInObjectCaseSensitive(root, name, replacement);
  } else {
    cJSON_AddItemToObject(root, name, replacement);
  }
}

bool hiddenReadDenied(const String &relative) {
  return relative == ".staging" || relative.startsWith(".staging/") ||
         relative == ".system" || relative.startsWith(".system/") ||
         relative == ".commands" || relative.startsWith(".commands/");
}

uint64_t fnvUpdate(uint64_t hash, const uint8_t *bytes, size_t count) {
  for (size_t index = 0; index < count; ++index) {
    hash ^= bytes[index];
    hash *= 1099511628211ULL;
  }
  return hash;
}

bool metadataName(const String &name) {
  return name.endsWith("/capsule.json") || name.endsWith("/processing.json") ||
         name.endsWith("/raw.txt") || name.endsWith("/polished.md") ||
         name.endsWith("/final.md") || name.endsWith("/trash.json");
}

String parentPath(const String &path) {
  const int slash = path.lastIndexOf('/');
  return slash <= 0 ? String("/") : path.substring(0, slash);
}

bool sameUuid(const char *left, const char *right) {
  return left != nullptr && right != nullptr && isUuid(left) && isUuid(right) &&
         strcasecmp(left, right) == 0;
}

}  // namespace

bool PokePodLinkService::begin(Stream &stream, fs::FS &fs,
                               BoardServices &board, AudioPipeline &audio,
                               AudioCaptureRouter &captureRouter,
                               UsbLinkBridge &usb, BleVoiceService &bleVoice,
                               Dashboard &dashboard,
                               CapsuleLibrary &library, WavRecorder &recorder,
                               DeviceConfig &config, WifiController &wifi,
                               TencentWorker &tencent,
                               ProvisioningDiagnostics &provisioningDiagnostics,
                               RuntimePowerManager &power, Print &log,
                               LinkServiceCoordinator *coordinator,
                               LinkTransport transport,
                               WirelessSyncPairingProvider *pairingProvider,
                               LinkTransferGate *transferGate,
                               ProvisioningCoordinator *provisioningCoordinator) {
  stream_ = &stream;
  fs_ = &fs;
  board_ = &board;
  audio_ = &audio;
  captureRouter_ = &captureRouter;
  usb_ = &usb;
  bleVoice_ = &bleVoice;
  dashboard_ = &dashboard;
  library_ = &library;
  recorder_ = &recorder;
  config_ = &config;
  wifi_ = &wifi;
  tencent_ = &tencent;
  provisioningDiagnostics_ = &provisioningDiagnostics;
  provisioningCoordinator_ = provisioningCoordinator;
  power_ = &power;
  log_ = &log;
  coordinator_ = coordinator;
  transport_ = transport;
  pairingProvider_ = pairingProvider;
  transferGate_ = transferGate;
  requestLeaseHeld_ = false;
  activeMaintenance_ = "";
  if (payload_ == nullptr) {
    payload_ = static_cast<uint8_t *>(heap_caps_calloc(
        kLinkMaxDataBytes, sizeof(uint8_t),
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  }
  if (payload_ == nullptr) {
    stream_ = nullptr;
    log_->println(
        "{\"event\":\"link_buffer\",\"ok\":false,\"memory\":\"psram\"}");
    return false;
  }
  log_->printf(
      "{\"event\":\"link_buffer\",\"ok\":true,\"memory\":\"psram\",\"bytes\":%u}\n",
      static_cast<unsigned>(kLinkMaxDataBytes));
  if (!ensureDirectoryTree(String(kCapsuleSystem) + "/commands/results") ||
      !ensureDirectoryTree(String(kCapsuleSystem) + "/commands/incoming")) {
    return false;
  }
  if (!cleanupPurgeStaging()) {
    log_->println("{\"event\":\"purge_cleanup_deferred\"}");
  }
  return true;
}

void PokePodLinkService::disconnect() {
  if (incomingFile_) incomingFile_.close();
  if (fs_ != nullptr && !incomingTemporaryPath_.isEmpty()) {
    fs_->remove(incomingTemporaryPath_);
  }
  incomingKind_ = IncomingKind::none;
  incomingRequestId_ = 0;
  incomingExpected_ = incomingReceived_ = 0;
  incomingChunkAcks_ = false;
  incomingTemporaryPath_ = incomingFinalPath_ = incomingTransactionId_ = "";
  incomingLastByteMs_ = 0;
  activeMaintenance_ = "";
  maintenanceCompletion_.disconnect();
  sessionActive_ = false;
  completed_.clear();
  resetFrame();
  releaseRequestLease();
}

void PokePodLinkService::poll(uint32_t nowMs) {
  if (stream_ == nullptr) return;
  if (!transferPermitted()) {
    disconnect();
    return;
  }
  size_t budget = 32768;
  while (transferPermitted() && stream_->available() > 0 && budget-- > 0) {
    const int value = stream_->read();
    if (value >= 0) consumeByte(static_cast<uint8_t>(value));
  }
  // A request can start after the caller captured nowMs. Subtracting that
  // older timestamp from the freshly recorded byte time underflows uint32_t
  // and used to reject every upload immediately on some loop iterations.
  if (incomingKind_ != IncomingKind::none &&
      static_cast<uint32_t>(millis() - incomingLastByteMs_) > 5000) {
    failIncoming("binary transfer timed out");
  }
  if (rebootAtMs_ != 0 && static_cast<int32_t>(nowMs - rebootAtMs_) >= 0) {
    if (stream_ != nullptr) stream_->flush();
    ESP.restart();
  }
}

void PokePodLinkService::consumeByte(uint8_t value) {
  if (receivePhase_ == ReceivePhase::magic) {
    static constexpr uint8_t magic[4] = {'P', 'P', 'V', '2'};
    if (value == magic[magicMatched_]) {
      headerBytes_[magicMatched_++] = value;
      if (magicMatched_ == 4) {
        sessionActive_ = true;
        headerUsed_ = 4;
        receivePhase_ = ReceivePhase::header;
      }
    } else {
      magicMatched_ = value == 'P' ? 1 : 0;
      if (magicMatched_ == 1) headerBytes_[0] = value;
    }
    return;
  }
  if (receivePhase_ == ReceivePhase::header) {
    headerBytes_[headerUsed_++] = value;
    if (headerUsed_ < kLinkHeaderBytes) return;
    if (!decodeLinkHeader(headerBytes_, sizeof(headerBytes_), currentHeader_)) {
      const uint32_t requestId = linkGet32(headerBytes_ + 8);
      resetFrame();
      if (requestId != 0) sendError(requestId, "invalid Link v2 header");
      return;
    }
    payloadUsed_ = 0;
    receivePhase_ = ReceivePhase::payload;
    if (currentHeader_.payloadLength == 0) processFrame();
    return;
  }
  if (payload_ != nullptr && payloadUsed_ < kLinkMaxDataBytes) {
    payload_[payloadUsed_++] = value;
  }
  if (payloadUsed_ == currentHeader_.payloadLength) processFrame();
}

void PokePodLinkService::resetFrame() {
  receivePhase_ = ReceivePhase::magic;
  headerUsed_ = 0;
  payloadUsed_ = 0;
  magicMatched_ = 0;
  currentHeader_ = LinkFrameHeader();
}

void PokePodLinkService::processFrame() {
  const LinkFrameHeader header = currentHeader_;
  const size_t size = payloadUsed_;
  if (!transferPermitted()) {
    resetFrame();
    return;
  }
  if (!validateLinkPayload(header, payload_, size)) {
    resetFrame();
    sendError(header.requestId, "Link v2 CRC mismatch");
    return;
  }
  resetFrame();
  if (header.type == LinkFrameType::requestJson) {
    processRequest(header.requestId, payload_, size);
  } else if (header.type == LinkFrameType::data) {
    processData(header.requestId, header.flags, payload_, size);
  } else {
    sendError(header.requestId, "unexpected Link v2 frame type");
  }
}

void PokePodLinkService::processRequest(uint32_t requestId,
                                        const uint8_t *payload, size_t size) {
  if (requestId == 0 || completed_.contains(requestId) ||
      incomingKind_ != IncomingKind::none) {
    sendError(requestId, requestId == 0 ? "requestId must be non-zero" :
              (completed_.contains(requestId) ? "duplicate requestId" :
                                                "another request is receiving data"));
    return;
  }
  cJSON *root = cJSON_ParseWithLength(
      reinterpret_cast<const char *>(payload), size);
  const char *operation = root == nullptr ? nullptr : jsonString(root, "operation");
  if (root == nullptr || !cJSON_IsObject(root) || operation == nullptr ||
      jsonInt64(root, "version") != kLinkVersion) {
    cJSON_Delete(root);
    sendError(requestId, "malformed Link v2 request");
    rememberCompleted(requestId);
    return;
  }

  const int64_t binaryLength = jsonInt64(root, "binaryLength", 0);
  const bool chunkAcks = jsonBool(root, "chunkAcks");
  if (strcmp(operation, "font-write") == 0) {
    if (!acquireRequestLease(requestId)) {
      cJSON_Delete(root);
      rememberCompleted(requestId);
      return;
    }
    cJSON_Delete(root);
    const String finalPath = String(kCapsuleSystem) + "/fonts/cjk20.a4";
    const String temporaryPath = finalPath + ".part";
    if (foregroundBusy()) {
      sendBusy(requestId);
      rememberCompleted(requestId);
      releaseRequestLease();
    } else if (binaryLength < 20 || binaryLength > 5 * 1024 * 1024 ||
               !ensureDirectoryTree(parentPath(finalPath)) ||
               !beginIncoming(IncomingKind::systemFont, requestId,
                              static_cast<uint32_t>(binaryLength),
                              temporaryPath, finalPath, "", chunkAcks)) {
      sendError(requestId, "invalid font transfer");
      rememberCompleted(requestId);
      releaseRequestLease();
    }
    return;
  }
  if (strcmp(operation, "stage-write") == 0 || strcmp(operation, "command") == 0) {
    if (!acquireRequestLease(requestId)) {
      cJSON_Delete(root);
      rememberCompleted(requestId);
      return;
    }
    if (foregroundBusy()) {
      cJSON_Delete(root);
      sendBusy(requestId);
      rememberCompleted(requestId);
      releaseRequestLease();
      return;
    }
    const char *transactionId = jsonString(root, "transactionId");
    if (!isUuid(transactionId) || binaryLength < 0 ||
        binaryLength > static_cast<int64_t>(kMaxIncomingCommandBytes * 3U)) {
      cJSON_Delete(root);
      sendError(requestId, "invalid transaction or binary length");
      rememberCompleted(requestId);
      releaseRequestLease();
      return;
    }
    if (strcmp(operation, "stage-write") == 0) {
      const char *capsuleId = jsonString(root, "capsuleId");
      const char *path = jsonString(root, "path");
      if (!isUuid(capsuleId) || !safeLinkRelativePath(path) ||
          binaryLength > 3LL * 1024LL * 1024LL) {
        cJSON_Delete(root);
        sendError(requestId, "invalid staged file target");
        rememberCompleted(requestId);
        releaseRequestLease();
        return;
      }
      const String directory = String(kCapsuleStaging) + "/" + transactionId +
          "/" + capsuleId;
      const String finalPath = directory + "/" + path;
      const String temporaryPath = finalPath + ".part";
      cJSON_Delete(root);
      if (!ensureDirectoryTree(parentPath(finalPath)) ||
          !beginIncoming(IncomingKind::stagedFile, requestId,
                         static_cast<uint32_t>(binaryLength), temporaryPath,
                         finalPath, transactionId, chunkAcks)) {
        sendError(requestId, "cannot open staged file");
        rememberCompleted(requestId);
        releaseRequestLease();
      }
      return;
    }
    if (binaryLength > static_cast<int64_t>(kMaxIncomingCommandBytes)) {
      cJSON_Delete(root);
      sendError(requestId, "command is too large");
      rememberCompleted(requestId);
      releaseRequestLease();
      return;
    }
    const String finalPath = String(kCapsuleSystem) + "/commands/incoming/" +
        transactionId + ".json";
    const String temporaryPath = finalPath + ".part";
    cJSON_Delete(root);
    if (!beginIncoming(IncomingKind::command, requestId,
                       static_cast<uint32_t>(binaryLength), temporaryPath,
                       finalPath, transactionId, chunkAcks)) {
      sendError(requestId, "cannot open command file");
      rememberCompleted(requestId);
      releaseRequestLease();
    }
    return;
  }

  if (acquireRequestLease(requestId)) handleImmediate(requestId, root);
  cJSON_Delete(root);
  rememberCompleted(requestId);
  if (activeMaintenance_.isEmpty()) releaseRequestLease();
}

bool PokePodLinkService::beginIncoming(IncomingKind kind, uint32_t requestId,
                                       uint32_t expectedBytes,
                                       const String &temporaryPath,
                                       const String &finalPath,
                                       const String &transactionId,
                                       bool chunkAcks) {
  if (fs_->exists(temporaryPath)) fs_->remove(temporaryPath);
  File output = fs_->open(temporaryPath, FILE_WRITE);
  if (!output) return false;
  incomingKind_ = kind;
  incomingRequestId_ = requestId;
  incomingExpected_ = expectedBytes;
  incomingReceived_ = 0;
  incomingChunkAcks_ = chunkAcks;
  incomingFile_ = output;
  incomingTemporaryPath_ = temporaryPath;
  incomingFinalPath_ = finalPath;
  incomingTransactionId_ = transactionId;
  incomingLastByteMs_ = millis();
  return true;
}

void PokePodLinkService::processData(uint32_t requestId, uint16_t flags,
                                     const uint8_t *payload, size_t size) {
  if (incomingKind_ == IncomingKind::none || requestId != incomingRequestId_) {
    sendError(requestId, "unexpected data frame");
    return;
  }
  if (incomingReceived_ + size > incomingExpected_ ||
      (size > 0 && incomingFile_.write(payload, size) != size)) {
    failIncoming("staged write failed");
    return;
  }
  incomingReceived_ += size;
  incomingLastByteMs_ = millis();
  const bool last = (flags & 1U) != 0;
  if (last && incomingReceived_ != incomingExpected_) {
    failIncoming("binary length mismatch");
    return;
  }
  if (incomingChunkAcks_) {
    sendEvent(requestId, "{\"event\":\"binary_ack\",\"received\":" +
        String(incomingReceived_) + "}");
  }
  if (last) finishIncoming();
}

void PokePodLinkService::finishIncoming() {
  const IncomingKind kind = incomingKind_;
  const uint32_t requestId = incomingRequestId_;
  const String temporary = incomingTemporaryPath_;
  const String finalPath = incomingFinalPath_;
  const String transaction = incomingTransactionId_;
  incomingFile_.flush();
  const bool writeOk = incomingFile_.getWriteError() == 0;
  incomingFile_.close();
  incomingKind_ = IncomingKind::none;
  incomingRequestId_ = 0;
  incomingExpected_ = incomingReceived_ = 0;
  incomingChunkAcks_ = false;
  incomingTemporaryPath_ = incomingFinalPath_ = incomingTransactionId_ = "";
  incomingLastByteMs_ = 0;
  if (!writeOk || (kind == IncomingKind::systemFont && !validFontFile(temporary)) ||
      (fs_->exists(finalPath) && !fs_->remove(finalPath)) ||
      !fs_->rename(temporary, finalPath)) {
    fs_->remove(temporary);
    sendError(requestId, "atomic file commit failed");
    rememberCompleted(requestId);
    releaseRequestLease();
    return;
  }
  if (kind == IncomingKind::command) {
    handleCommandFile(requestId, finalPath, transaction);
  } else if (kind == IncomingKind::systemFont) {
    sendOk(requestId, "\"installed\":true,\"rebootRequired\":true");
  } else {
    sendOk(requestId);
  }
  rememberCompleted(requestId);
  if (activeMaintenance_.isEmpty()) releaseRequestLease();
}

void PokePodLinkService::failIncoming(const char *message) {
  const uint32_t requestId = incomingRequestId_;
  incomingFile_.close();
  if (fs_ != nullptr && !incomingTemporaryPath_.isEmpty()) {
    fs_->remove(incomingTemporaryPath_);
  }
  incomingKind_ = IncomingKind::none;
  incomingRequestId_ = 0;
  incomingExpected_ = incomingReceived_ = 0;
  incomingChunkAcks_ = false;
  incomingTemporaryPath_ = incomingFinalPath_ = incomingTransactionId_ = "";
  incomingLastByteMs_ = 0;
  sendError(requestId, message);
  rememberCompleted(requestId);
  if (activeMaintenance_.isEmpty()) releaseRequestLease();
}

void PokePodLinkService::handleImmediate(uint32_t requestId, void *jsonRoot) {
  cJSON *root = asJson(jsonRoot);
  const char *operation = jsonString(root, "operation");
  if (strcmp(operation, "hello") == 0) {
    const char *capabilities = transport_ == LinkTransport::usb
        ? "\"protocol\":\"PokePod Link\",\"capabilities\":[\"read\",\"stage-write\",\"command\",\"configure\",\"set-time\",\"record\",\"stop\",\"font-write\",\"provisioning-diagnostics\",\"provisioning-start\",\"provisioning-stop\",\"pairing-export\",\"reboot\"]"
        : "\"protocol\":\"PokePod Link\",\"capabilities\":[\"read\",\"stage-write\",\"command\",\"configure\",\"set-time\",\"record\",\"stop\",\"font-write\",\"provisioning-diagnostics\",\"reboot\"]";
    sendOk(requestId, capabilities);
  } else if (strcmp(operation, "status") == 0) {
    const BoardStatus &status = board_->status();
    String extra = "\"recording\":";
    extra += recorder_->recording() ? "true" : "false";
    extra += ",\"transcribing\":";
    extra += tencent_->working() ? "true" : "false";
    extra += ",\"batteryPercent\":" + String(status.batteryPercent);
    extra += ",\"charging\":";
    extra += status.charging ? "true" : "false";
    extra += ",\"wifi\":\"" + String(wifi_->phaseName()) + "\"";
    extra += ",\"wifiDisconnectReason\":" +
        String(wifi_->lastDisconnectReason());
    extra += ",\"wifiDisconnectKind\":\"" +
        String(wifiFailureKey(wifi_->lastDisconnectReason())) + "\"";
    extra += ",\"wifiRadioOn\":" +
        String(wifi_->radioOn() ? "true" : "false");
    extra += ",\"wifiPowerSave\":" +
        String(wifi_->powerSaveEnabled() ? "true" : "false");
    extra += ",\"wifiPowerSaveError\":" +
        String(wifi_->powerSaveError());
    extra += ",\"pendingCapsules\":" + String(library_->pendingCount());
    extra += ",\"asr_hash_ms\":" + String(tencent_->lastHashElapsedMs());
    extra += ",\"asr_connect_ms\":" + String(tencent_->lastConnectElapsedMs());
    extra += ",\"asr_upload_ms\":" + String(tencent_->lastUploadElapsedMs());
    extra += ",\"asr_total_ms\":" + String(tencent_->lastTotalElapsedMs());
    extra += ",\"asr_last_code\":\"" + jsonEscaped(tencent_->lastCode()) + "\"";
    extra += ",\"asr_tls_error\":" + String(tencent_->lastNetworkError());
    extra += ",\"asr_tls_detail\":\"" +
        jsonEscaped(tencent_->lastNetworkErrorDetail()) + "\"";
    extra += ",\"asr_heap_free_before_tls\":" +
        String(tencent_->lastInternalHeapFreeBeforeTls());
    extra += ",\"asr_heap_largest_before_tls\":" +
        String(tencent_->lastInternalHeapLargestBeforeTls());
    extra += ",\"asr_psram_free_before_tls\":" +
        String(tencent_->lastPsramFreeBeforeTls());
    extra += ",\"sdReady\":";
    extra += status.sdCard ? "true" : "false";
    extra += ",\"variant\":\"" + String(variantName(status.variant)) + "\"";
    extra += ",\"ioExpander\":" + String(status.ioExpander ? "true" : "false");
    extra += ",\"display\":" + String(status.display ? "true" : "false");
    extra += ",\"touch\":" + String(status.touch ? "true" : "false");
    extra += ",\"rtc\":" + String(status.rtc ? "true" : "false");
    extra += ",\"imu\":" + String(status.imu ? "true" : "false");
    extra += ",\"pmu\":" + String(status.pmu ? "true" : "false");
    extra += ",\"vbusPresent\":" + String(status.vbusPresent ? "true" : "false");
    extra += ",\"screenOn\":" + String(status.screenOn ? "true" : "false");
    extra += ",\"audio\":" + String(audio_ != nullptr && audio_->ready() ? "true" : "false");
    extra += ",\"usb\":" + String(usb_->ready() ? "true" : "false");
    extra += ",\"host_connected\":" + String(usb_->hostConnected() ? "true" : "false");
    extra += ",\"bleVoiceConnected\":" +
        String(bleVoice_ != nullptr && bleVoice_->connected() ? "true" : "false");
    extra += ",\"bleVoiceReady\":" +
        String(bleVoice_ != nullptr && bleVoice_->appReady() ? "true" : "false");
    extra += ",\"bleVoiceMtu\":" +
        String(bleVoice_ == nullptr ? 0 : bleVoice_->mtu());
    const BleVoiceQualitySnapshot bleQuality = bleVoice_ == nullptr
        ? BleVoiceQualitySnapshot() : bleVoice_->quality();
    extra += ",\"bleVoiceNotifyAttempts\":" +
        String(bleQuality.notifyAttempts);
    extra += ",\"bleVoiceNotifyAccepted\":" +
        String(bleQuality.notifyAccepted);
    extra += ",\"bleVoiceNotifyFailures\":" +
        String(bleQuality.notifyFailures);
    extra += ",\"bleVoiceQueueOverflows\":" +
        String(bleQuality.queueOverflows);
    extra += ",\"bleVoiceSessionFailures\":" +
        String(bleQuality.sessionFailures);
    extra += ",\"bleVoiceReadyTimeouts\":" +
        String(bleQuality.readyTimeouts);
    extra += ",\"bleVoiceStopAckTimeouts\":" +
        String(bleQuality.stopAckTimeouts);
    extra += ",\"bleVoiceStreamTimeouts\":" +
        String(bleQuality.streamTimeouts);
    extra += ",\"bleVoiceLastErrorCode\":" +
        String(bleQuality.lastErrorCode);
    extra += ",\"audio_read_bytes\":" + String(static_cast<unsigned long>(audio_->bytesRead()));
    extra += ",\"audio_read_failures\":" + String(audio_->readFailures());
    extra += ",\"audio_peak\":" + String(audio_->peakSample());
    const AudioFrontEndMetrics &frontEnd = recorder_->audioMetrics();
    extra += ",\"audio_frontend_channel\":\"" +
        String(audioInputChannelName(frontEnd.selectedChannel)) + "\"";
    extra += ",\"audio_frontend_left_peak\":" + String(frontEnd.leftPeak);
    extra += ",\"audio_frontend_right_peak\":" + String(frontEnd.rightPeak);
    extra += ",\"audio_frontend_output_peak\":" + String(frontEnd.outputPeak);
    extra += ",\"audio_frontend_limited_samples\":" +
        String(frontEnd.limitedSamples);
    extra += ",\"tencentConfigured\":" +
        String(config_->hasTencent() ? "true" : "false");
    extra += ",\"wifiNetworkCount\":" +
        String(static_cast<unsigned>(config_->wifiNetworks().size()));
    extra += ",\"ui_full_redraws\":" + String(dashboard_->fullRedrawCount());
    extra += ",\"ui_body_redraws\":" + String(dashboard_->bodyRedrawCount());
    extra += ",\"ui_partial_redraws\":" + String(dashboard_->partialRedrawCount());
    extra += ",\"ui_frame_buffer\":" +
        String(dashboard_->frameBufferReady() ? "true" : "false");
    extra += ",\"ui_animation_buffer\":" +
        String(dashboard_->animationBufferReady() ? "true" : "false");
    const RuntimePowerSnapshot &power = power_->snapshot();
    extra += ",\"powerMode\":\"" + String(powerModeName(power.mode)) + "\"";
    extra += ",\"cpuMhz\":" + String(power.cpuMhz);
    extra += ",\"powerTransitions\":" + String(power.transitions);
    extra += ",\"lightSleepCount\":" + String(power.lightSleepCount);
    extra += ",\"lightSleepMs\":" +
        String(static_cast<unsigned long>(power.lightSleepUs / 1000ULL));
    extra += ",\"deepSleepWakeCount\":" +
        String(power.deepSleepWakeCount);
    extra += ",\"wokeFromDeepSleep\":" +
        String(power.wokeFromDeepSleep ? "true" : "false");
    extra += ",\"deepSleepTouchWakeArmed\":" +
        String(power.deepSleepTouchWakeArmed ? "true" : "false");
    extra += ",\"lastWakeCause\":" + String(power.lastWakeCause);
    extra += ",\"resetReason\":" +
        String(static_cast<unsigned>(esp_reset_reason()));
    extra += ",\"internalHeapFree\":" + String(static_cast<unsigned>(
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    extra += ",\"internalHeapLargest\":" + String(static_cast<unsigned>(
        heap_caps_get_largest_free_block(
            MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
    extra += ",\"psramFree\":" +
        String(static_cast<unsigned>(ESP.getFreePsram()));
    extra += ",\"automaticPmSupported\":" +
        String(power.automaticPmSupported ? "true" : "false");
    extra += ",\"bleModemSleepSupported\":" +
        String(power.bleModemSleepSupported ? "true" : "false");
    extra += ",\"provisioningDiagnosticCount\":" +
        String(static_cast<unsigned>(provisioningDiagnostics_->count()));
    extra += ",\"provisioningStartupPhase\":\"" +
        String(provisioningCoordinator_ == nullptr ? "unavailable" :
               provisioningCoordinator_->phaseName()) + "\"";
    sendOk(requestId, extra.c_str());
  } else if (strcmp(operation, "provisioning-start") == 0) {
    if (transport_ != LinkTransport::usb ||
        provisioningCoordinator_ == nullptr) {
      sendError(requestId,
                "provisioning-start is only available over USB");
    } else if (foregroundBusy()) {
      sendBusy(requestId);
    } else if (!provisioningCoordinator_->request(millis())) {
      sendError(requestId, "provisioning startup request rejected");
    } else {
      const String extra = "\"phase\":\"" +
          String(provisioningCoordinator_->phaseName()) + "\"";
      sendOk(requestId, extra.c_str());
    }
  } else if (strcmp(operation, "provisioning-stop") == 0) {
    if (transport_ != LinkTransport::usb ||
        provisioningCoordinator_ == nullptr) {
      sendError(requestId,
                "provisioning-stop is only available over USB");
    } else {
      provisioningCoordinator_->stop();
      sendOk(requestId, "\"phase\":\"idle\"");
    }
  } else if (strcmp(operation, "get-provisioning-diagnostics") == 0) {
    sendJson(requestId, provisioningDiagnosticsJson());
  } else if (strcmp(operation, "clear-provisioning-diagnostics") == 0) {
    if (foregroundBusy()) sendBusy(requestId);
    else if (provisioningDiagnostics_->clear(*log_)) sendOk(requestId);
    else sendError(requestId, "provisioning diagnostics clear failed");
  } else if (strcmp(operation, "identity") == 0) {
    const String extra = "\"deviceId\":\"" + deviceId() +
        "\",\"displayName\":\"PokePod\",\"platform\":\"pokepod\",\"manufacturer\":\"PokeCapsule\",\"model\":\"" +
        String(variantName(board_->status().variant)) + "\"";
    sendOk(requestId, extra.c_str());
  } else if (strcmp(operation, "pairing-export") == 0) {
    if (transport_ != LinkTransport::usb || pairingProvider_ == nullptr) {
      sendError(requestId, "pairing-export is only available over USB");
    } else {
      String bundle;
      String error;
      if (pairingProvider_->pairingBundle(jsonBool(root, "rotate"),
                                          bundle, error)) {
        const std::string encoded = base64UrlEncode(
            reinterpret_cast<const uint8_t *>(bundle.c_str()),
            bundle.length());
        const String extra = "\"bundle\":\"" + String(encoded.c_str()) + "\"";
        sendOk(requestId, extra.c_str());
      } else {
        sendError(requestId, error.c_str());
      }
    }
  } else if (strcmp(operation, "fingerprint") == 0) {
    if (foregroundBusy()) sendBusy(requestId);
    else {
      const String extra = "\"fingerprint\":\"" + metadataFingerprint() + "\"";
      sendOk(requestId, extra.c_str());
    }
  } else if (strcmp(operation, "read") == 0) {
    if (foregroundBusy()) sendBusy(requestId);
    else handleRead(requestId, root);
  } else if (strcmp(operation, "commit") == 0) {
    const char *transactionId = jsonString(root, "transactionId");
    const char *capsuleId = jsonString(root, "capsuleId");
    const String path = String(kCapsuleStaging) + "/" +
        (transactionId == nullptr ? "" : transactionId) + "/" +
        (capsuleId == nullptr ? "" : capsuleId);
    if (foregroundBusy()) sendBusy(requestId);
    else if (!isUuid(transactionId) || !isUuid(capsuleId) || !fs_->exists(path)) {
      sendError(requestId, "staging transaction is incomplete");
    } else sendOk(requestId, "\"staged\":true");
  } else if (strcmp(operation, "result") == 0) {
    const char *transactionId = jsonString(root, "transactionId");
    if (!isUuid(transactionId)) {
      sendError(requestId, "invalid transactionId");
      return;
    }
    const String path = String(kCapsuleSystem) + "/commands/results/" +
        transactionId + ".json";
    if (!fs_->exists(path)) {
      sendOk(requestId, "\"available\":false");
    } else {
      const bool fullySent = sendFile(requestId, path);
      maintenanceCompletion_.resultFetched(transactionId, fullySent);
    }
  } else if (strcmp(operation, "configure") == 0) {
    if (foregroundBusy()) sendBusy(requestId);
    else handleConfigure(requestId, root);
  } else if (strcmp(operation, "set-time") == 0) {
    const int64_t unixTimeMs = jsonInt64(root, "unixTimeMs");
    if (unixTimeMs < 1704067200000LL || !board_->setUtcEpoch(unixTimeMs / 1000)) {
      sendError(requestId, "invalid UTC time");
    } else sendOk(requestId);
  } else if (strcmp(operation, "record") == 0) {
    if (foregroundBusy()) sendBusy(requestId);
    else if (audio_ == nullptr || !audio_->ready() || !board_->sdReady()) {
      sendError(requestId, "audio or SD is not ready");
    } else {
      const String id = newUuid();
      tencent_->wake();
      const bool acquired = captureRouter_->acquire(
          AudioCaptureOwner::localCapsule);
      if (!acquired) {
        sendBusy(requestId);
      } else if (audio_->startCapture(*log_) &&
                 recorder_->start(*log_, id, board_->utcNow())) {
        const String extra = "\"recording\":true,\"capsuleId\":\"" + id + "\"";
        sendOk(requestId, extra.c_str());
      } else {
        captureRouter_->release(AudioCaptureOwner::localCapsule);
        audio_->stopHardware(*log_);
        sendError(requestId, "recording start failed");
      }
    }
  } else if (strcmp(operation, "stop") == 0) {
    if (!recorder_->recording()) sendError(requestId, "recording is not active");
    else {
      const bool committed = recorder_->stop(*log_);
      captureRouter_->release(AudioCaptureOwner::localCapsule);
      audio_->stopHardware(*log_);
      if (committed) {
        library_->scan();
        sendOk(requestId, "\"recording\":false,\"queued\":true");
      } else {
        sendError(requestId, "recording commit failed");
      }
    }
  } else if (strcmp(operation, "reboot") == 0) {
    sendOk(requestId);
    rebootAtMs_ = millis() + 100;
  } else {
    sendError(requestId, "unsupported Link v2 operation");
  }
}

void PokePodLinkService::handleRead(uint32_t requestId, void *jsonRoot) {
  cJSON *root = asJson(jsonRoot);
  const char *requested = jsonString(root, "path");
  if (requested == nullptr) {
    sendError(requestId, "read path is required");
    return;
  }
  if (strcmp(requested, ".") == 0 && jsonBool(root, "recursive")) {
    std::vector<String> files;
    if (!collectFiles(kCapsuleRoot, "", 0, files) ||
        !transferPermitted()) {
      return;
    }
    std::sort(files.begin(), files.end());
    const char *cursorText = jsonString(root, "cursor");
    size_t cursor = cursorText == nullptr ? 0 : strtoul(cursorText, nullptr, 10);
    if (cursor > files.size()) {
      sendError(requestId, "invalid read cursor");
      return;
    }
    cJSON *response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "status", "ok");
    cJSON_AddNumberToObject(response, "version", kLinkVersion);
    cJSON *items = cJSON_AddArrayToObject(response, "files");
    const size_t end = std::min(files.size(), cursor + kReadPageFiles);
    for (size_t index = cursor; index < end; ++index) {
      File file = fs_->open(String(kCapsuleRoot) + "/" + files[index], FILE_READ);
      if (!file || file.isDirectory()) {
        if (file) file.close();
        continue;
      }
      char digest[65] = {};
      if (linkAudioFileNeedsDigest(files[index].c_str()) &&
          !sha256File(file, digest)) {
        file.close();
        cJSON_Delete(response);
        sendError(requestId, "audio digest failed");
        return;
      }
      cJSON *item = cJSON_CreateObject();
      cJSON_AddStringToObject(item, "path", files[index].c_str());
      cJSON_AddNumberToObject(item, "length", file.size());
      if (digest[0] != '\0') {
        cJSON_AddStringToObject(item, "sha256", digest);
      }
      cJSON_AddItemToArray(items, item);
      file.close();
    }
    if (end < files.size()) cJSON_AddStringToObject(response, "nextCursor", String(end).c_str());
    sendJson(requestId, printed(response));
    cJSON_Delete(response);
    return;
  }
  const String relative(requested);
  if (!safeLinkRelativePath(requested, true) || hiddenReadDenied(relative)) {
    sendError(requestId, "unsafe read path");
    return;
  }
  sendFile(requestId, String(kCapsuleRoot) + "/" + relative);
}

bool PokePodLinkService::sha256File(File &file, char output[65]) const {
  if (!file || file.isDirectory() || output == nullptr || !file.seek(0)) {
    return false;
  }
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  bool ok = mbedtls_sha256_starts(&context, 0) == 0;
  uint8_t buffer[1024];
  size_t remaining = file.size();
  while (ok && remaining > 0 && transferPermitted()) {
    const size_t wanted = std::min(remaining, sizeof(buffer));
    const int received = file.read(buffer, wanted);
    if (received <= 0) {
      ok = false;
      break;
    }
    ok = mbedtls_sha256_update(
        &context, buffer, static_cast<size_t>(received)) == 0;
    remaining -= static_cast<size_t>(received);
  }
  if (remaining > 0) ok = false;
  uint8_t digest[32] = {};
  if (ok) ok = mbedtls_sha256_finish(&context, digest) == 0;
  mbedtls_sha256_free(&context);
  if (!ok) return false;
  static constexpr char kHex[] = "0123456789abcdef";
  for (size_t index = 0; index < sizeof(digest); ++index) {
    output[index * 2] = kHex[digest[index] >> 4];
    output[index * 2 + 1] = kHex[digest[index] & 0x0f];
  }
  output[64] = '\0';
  return true;
}

void PokePodLinkService::handleConfigure(uint32_t requestId, void *jsonRoot) {
  cJSON *root = asJson(jsonRoot);
  cJSON *values = cJSON_GetObjectItemCaseSensitive(root, "values");
  if (!cJSON_IsObject(values)) {
    sendError(requestId, "configuration values are required");
    return;
  }
  DeviceSettings settings = config_->settings();
  struct StringField { const char *wire; String *target; };
  const StringField fields[] = {
      {"wifiSsid", &settings.wifiSsid}, {"wifiPassword", &settings.wifiPassword},
      {"secretId", &settings.secretId}, {"secretKey", &settings.secretKey},
      {"hotwordId", &settings.hotwordId},
  };
  for (const StringField &field : fields) {
    cJSON *item = cJSON_GetObjectItemCaseSensitive(values, field.wire);
    if (cJSON_IsString(item) && item->valuestring != nullptr) *field.target = item->valuestring;
    else if (cJSON_IsNull(item)) *field.target = "";
    else if (item != nullptr) {
      sendError(requestId, "invalid configuration value type");
      return;
    }
  }
  cJSON *wifiEnabled = cJSON_GetObjectItemCaseSensitive(values, "wifiEnabled");
  cJSON *raiseToWake = cJSON_GetObjectItemCaseSensitive(values, "raiseToWake");
  if (cJSON_IsBool(wifiEnabled)) settings.wifiEnabled = cJSON_IsTrue(wifiEnabled);
  if (cJSON_IsBool(raiseToWake)) settings.raiseToWake = cJSON_IsTrue(raiseToWake);
  if (!config_->save(settings, *log_)) {
    sendError(requestId, "configuration save failed");
    return;
  }
  wifi_->configurationChanged();
  tencent_->wake();
  const String extra = "\"wifiConfigured\":" +
      String(config_->hasWifi() ? "true" : "false") +
      ",\"wifiNetworkCount\":" +
      String(static_cast<unsigned>(config_->wifiNetworks().size())) +
      ",\"tencentConfigured\":" +
      String(config_->hasTencent() ? "true" : "false");
  sendOk(requestId, extra.c_str());
}

void PokePodLinkService::handleCommandFile(uint32_t requestId,
                                           const String &path,
                                           const String &transactionId) {
  String message;
  const String resultPath = String(kCapsuleSystem) + "/commands/results/" +
      transactionId + ".json";
  const bool accepted = fs_->exists(resultPath) ||
      executeCommand(path, transactionId, message);
  fs_->remove(path);
  if (!accepted) {
    log_->printf("{\"event\":\"link_command_failed\",\"transaction\":\"%s\",\"message\":\"%s\"}\n",
                 transactionId.c_str(), message.c_str());
  }
  sendOk(requestId, "\"accepted\":true");
}

bool PokePodLinkService::safeFolder(const char *value,
                                    bool allowBuiltIn) const {
  if (value == nullptr) return false;
  if (allowBuiltIn && (strcmp(value, "Inbox") == 0 ||
                       strcmp(value, "Archive") == 0)) return true;
  const size_t length = strlen(value);
  if (length == 0 || length > 161 || value[0] == '.' || value[0] == '/' ||
      value[length - 1] == '/') return false;
  uint8_t segments = 1;
  size_t segmentLength = 0;
  for (size_t index = 0; index < length; ++index) {
    const unsigned char c = static_cast<unsigned char>(value[index]);
    if (c == '\\' || c < 0x20) return false;
    if (c == '/') {
      if (segmentLength == 0 || ++segments > 2 || value[index + 1] == '.') return false;
      segmentLength = 0;
    } else {
      ++segmentLength;
    }
  }
  return segmentLength > 0;
}

String PokePodLinkService::folderDirectory(const char *value) const {
  if (!safeFolder(value)) return String();
  if (strcmp(value, "Inbox") == 0) return String(kCapsuleInbox);
  if (strcmp(value, "Archive") == 0) return String(kCapsuleArchive);
  return String(kCapsuleRoot) + "/" + value;
}

String PokePodLinkService::activeCapsuleDirectory(const String &id) const {
  const CapsuleSummary *record = library_->find(id);
  return record == nullptr || record->readOnly ? String() : record->directory;
}

bool PokePodLinkService::collectCommandIds(void *jsonRoot,
                                           std::vector<String> &ids) const {
  cJSON *array = cJSON_GetObjectItemCaseSensitive(asJson(jsonRoot), "capsuleIds");
  if (!cJSON_IsArray(array) || cJSON_GetArraySize(array) > 500) return false;
  cJSON *item = nullptr;
  cJSON_ArrayForEach(item, array) {
    const char *value = cJSON_GetStringValue(item);
    if (!isUuid(value)) return false;
    String normalized(value);
    normalized.toLowerCase();
    if (std::find(ids.begin(), ids.end(), normalized) != ids.end()) return false;
    ids.push_back(normalized);
  }
  return true;
}

bool PokePodLinkService::validateExpectedRevisions(
    void *jsonRoot, const std::vector<String> &ids, bool processing,
    bool trash, String &message) const {
  cJSON *expected = cJSON_GetObjectItemCaseSensitive(
      asJson(jsonRoot), "expectedRevisions");
  if (!cJSON_IsObject(expected) || cJSON_GetArraySize(expected) != ids.size()) {
    message = "complete expectedRevisions are required";
    return false;
  }
  for (const String &id : ids) {
    const String directory = trash ? String(kCapsuleTrash) + "/" + id
                                   : activeCapsuleDirectory(id);
    const String metadata = directory +
        (trash ? "/trash.json" : (processing ? "/processing.json" : "/capsule.json"));
    cJSON *root = cJSON_Parse(readText(metadata, 8192).c_str());
    cJSON *revision = root == nullptr ? nullptr :
        cJSON_GetObjectItemCaseSensitive(root, "revision");
    cJSON *wanted = cJSON_GetObjectItemCaseSensitive(expected, id.c_str());
    const bool matches = !directory.isEmpty() && cJSON_IsNumber(revision) &&
        cJSON_IsNumber(wanted) && revision->valueint == wanted->valueint;
    cJSON_Delete(root);
    if (!matches) {
      message = "revision conflict for " + id;
      return false;
    }
  }
  return true;
}

bool PokePodLinkService::touchCapsule(const String &directory) {
  const String path = directory + "/capsule.json";
  cJSON *root = cJSON_Parse(readText(path, 8192).c_str());
  if (root == nullptr) return false;
  cJSON *revision = cJSON_GetObjectItemCaseSensitive(root, "revision");
  const int next = cJSON_IsNumber(revision) ? revision->valueint + 1 : 1;
  if (revision != nullptr) cJSON_SetNumberValue(revision, next);
  else cJSON_AddNumberToObject(root, "revision", next);
  replaceStringOrNull(root, "updatedAt", board_->utcNow().c_str());
  const bool ok = writeTextAtomic(path, printed(root) + "\n");
  cJSON_Delete(root);
  return ok;
}

bool PokePodLinkService::mutateFavoriteOrTags(
    void *jsonRoot, const char *operation, const std::vector<String> &ids,
    String &message) {
  if (ids.empty() || !validateExpectedRevisions(
          jsonRoot, ids, false, false, message)) return false;
  cJSON *root = asJson(jsonRoot);
  cJSON *requestedTags = cJSON_GetObjectItemCaseSensitive(root, "tags");
  std::vector<String> tags;
  if (strcmp(operation, "setFavorite") != 0) {
    if (!cJSON_IsArray(requestedTags) || cJSON_GetArraySize(requestedTags) == 0) {
      message = "tags are required";
      return false;
    }
    cJSON *item = nullptr;
    cJSON_ArrayForEach(item, requestedTags) {
      const char *value = cJSON_GetStringValue(item);
      if (value == nullptr || value[0] == '#' || strlen(value) == 0 ||
          strlen(value) > 50) {
        message = "invalid tag";
        return false;
      }
      tags.emplace_back(value);
    }
  }
  struct Update { String path; String value; };
  std::vector<Update> updates;
  for (const String &id : ids) {
    const String path = activeCapsuleDirectory(id) + "/capsule.json";
    cJSON *capsule = cJSON_Parse(readText(path, 8192).c_str());
    if (capsule == nullptr) {
      message = "capsule metadata is malformed";
      return false;
    }
    if (strcmp(operation, "setFavorite") == 0) {
      cJSON *favorite = cJSON_GetObjectItemCaseSensitive(root, "favorite");
      if (!cJSON_IsBool(favorite)) {
        cJSON_Delete(capsule);
        message = "favorite is required";
        return false;
      }
      cJSON_ReplaceItemInObjectCaseSensitive(
          capsule, "favorite", cJSON_CreateBool(cJSON_IsTrue(favorite)));
    } else {
      cJSON *old = cJSON_GetObjectItemCaseSensitive(capsule, "tags");
      cJSON *replacement = cJSON_CreateArray();
      std::vector<String> values;
      cJSON *item = nullptr;
      cJSON_ArrayForEach(item, old) {
        const char *value = cJSON_GetStringValue(item);
        if (value != nullptr) values.emplace_back(value);
      }
      if (strcmp(operation, "addTags") == 0) {
        for (const String &tag : tags) {
          bool found = false;
          for (const String &value : values) {
            if (value.equalsIgnoreCase(tag)) found = true;
          }
          if (!found) values.push_back(tag);
        }
      } else if (strcmp(operation, "removeTags") == 0 ||
                 strcmp(operation, "deleteTag") == 0) {
        values.erase(std::remove_if(values.begin(), values.end(),
            [&](const String &value) {
              for (const String &tag : tags) if (value.equalsIgnoreCase(tag)) return true;
              return false;
            }), values.end());
      } else if (strcmp(operation, "renameTag") == 0 ||
                 strcmp(operation, "mergeTag") == 0) {
        if (tags.size() != 2) {
          cJSON_Delete(replacement);
          cJSON_Delete(capsule);
          message = "tag rename needs old and new names";
          return false;
        }
        for (String &value : values) {
          if (value.equalsIgnoreCase(tags[0])) value = tags[1];
        }
        for (size_t left = 0; left < values.size(); ++left) {
          values.erase(std::remove_if(values.begin() + left + 1, values.end(),
              [&](const String &value) { return value.equalsIgnoreCase(values[left]); }),
              values.end());
        }
      }
      for (const String &value : values) cJSON_AddItemToArray(replacement, cJSON_CreateString(value.c_str()));
      if (cJSON_HasObjectItem(capsule, "tags")) {
        cJSON_ReplaceItemInObjectCaseSensitive(capsule, "tags", replacement);
      } else {
        cJSON_AddItemToObject(capsule, "tags", replacement);
      }
    }
    cJSON *revision = cJSON_GetObjectItemCaseSensitive(capsule, "revision");
    if (!cJSON_IsNumber(revision)) {
      cJSON_Delete(capsule);
      message = "capsule revision is missing";
      return false;
    }
    cJSON_SetNumberValue(revision, revision->valueint + 1);
    replaceStringOrNull(capsule, "updatedAt", board_->utcNow().c_str());
    updates.push_back({path, printed(capsule) + "\n"});
    cJSON_Delete(capsule);
  }
  for (const Update &update : updates) {
    if (!writeTextAtomic(update.path, update.value)) {
      message = "capsule metadata commit failed";
      return false;
    }
  }
  message = "committed";
  return true;
}

bool PokePodLinkService::moveOrCopy(void *jsonRoot, bool copy,
                                    const std::vector<String> &ids,
                                    String &message) {
  cJSON *root = asJson(jsonRoot);
  const char *destination = jsonString(root, "destination");
  const String targetFolder = folderDirectory(destination);
  if (ids.empty() || targetFolder.isEmpty() || !fs_->exists(targetFolder) ||
      !validateExpectedRevisions(root, ids, false, false, message)) {
    if (message.isEmpty()) message = "invalid move destination";
    return false;
  }
  for (const String &id : ids) {
    const String source = activeCapsuleDirectory(id);
    if (source.isEmpty()) {
      message = "capsule is missing";
      return false;
    }
    if (copy) {
      const String newId = newUuid();
      const String target = targetFolder + "/" + newId;
      if (!copyTree(source, target) || !rewriteCopiedMetadata(target, newId)) {
        removeTree(target);
        message = "capsule copy failed";
        return false;
      }
    } else {
      const String target = targetFolder + "/" + id;
      if (source == target) continue;
      if (fs_->exists(target) || !fs_->rename(source, target) || !touchCapsule(target)) {
        message = "capsule move failed";
        return false;
      }
    }
  }
  message = "committed";
  return true;
}

bool PokePodLinkService::trashOperation(void *jsonRoot, const char *operation,
                                        const std::vector<String> &ids,
                                        String &message) {
  cJSON *root = asJson(jsonRoot);
  const bool trash = strcmp(operation, "deleteCapsules") != 0;
  for (const String &id : ids) {
    const CapsuleSummary *record = library_->find(id);
    if (record != nullptr && record->readOnly) {
      message = "future schema capsule is read-only";
      return false;
    }
  }
  if (ids.empty() || !validateExpectedRevisions(
          root, ids, false, trash, message)) return false;
  if (strcmp(operation, "purgeCapsules") == 0) {
    const CapsuleBatchResult result = library_->purge(ids);
    if (!result.ok) {
      message = result.rollbackFailed > 0
          ? "purge rollback failed for " + result.rollbackFailedId
          : "purge failed for " + result.failedId;
      return false;
    }
    message = "committed";
    return true;
  }
  for (const String &id : ids) {
    if (strcmp(operation, "deleteCapsules") == 0) {
      const String source = activeCapsuleDirectory(id);
      const int slash = source.lastIndexOf('/');
      String original = slash < 0 ? "Inbox" : source.substring(strlen(kCapsuleRoot) + 1, slash);
      if (original.isEmpty()) original = "Inbox";
      cJSON *capsule = cJSON_Parse(readText(source + "/capsule.json", 8192).c_str());
      cJSON *revision = capsule == nullptr ? nullptr : cJSON_GetObjectItemCaseSensitive(capsule, "revision");
      const int nextRevision = cJSON_IsNumber(revision) ? revision->valueint + 1 : 2;
      cJSON_Delete(capsule);
      cJSON *metadata = cJSON_CreateObject();
      cJSON_AddNumberToObject(metadata, "schemaVersion", 1);
      cJSON_AddStringToObject(metadata, "capsuleId", id.c_str());
      cJSON_AddStringToObject(metadata, "trashedAt", board_->utcNow().c_str());
      cJSON_AddStringToObject(metadata, "originalFolder", original.c_str());
      cJSON_AddNumberToObject(metadata, "revision", nextRevision);
      const bool wrote = writeTextAtomic(source + "/trash.json", printed(metadata) + "\n");
      cJSON_Delete(metadata);
      const String target = String(kCapsuleTrash) + "/" + id;
      if (!wrote || fs_->exists(target) || !fs_->rename(source, target)) {
        message = "move to trash failed";
        return false;
      }
    } else if (strcmp(operation, "restoreCapsules") == 0) {
      const String source = String(kCapsuleTrash) + "/" + id;
      cJSON *metadata = cJSON_Parse(readText(source + "/trash.json", 8192).c_str());
      const char *original = metadata == nullptr ? nullptr : jsonString(metadata, "originalFolder");
      String targetFolder = folderDirectory(original);
      if (targetFolder.isEmpty() || !fs_->exists(targetFolder)) targetFolder = kCapsuleInbox;
      const String target = targetFolder + "/" + id;
      cJSON_Delete(metadata);
      if (fs_->exists(target) || !fs_->remove(source + "/trash.json") ||
          !fs_->rename(source, target) || !touchCapsule(target)) {
        message = "trash restore failed";
        return false;
      }
    }
  }
  message = "committed";
  return true;
}

bool PokePodLinkService::folderOperation(void *jsonRoot,
                                         const char *operation,
                                         String &message) {
  cJSON *root = asJson(jsonRoot);
  const char *folder = jsonString(root, "folderPath");
  if (!safeFolder(folder, false)) {
    message = "invalid user folder";
    return false;
  }
  const String source = String(kCapsuleRoot) + "/" + folder;
  if (strcmp(operation, "createFolder") == 0) {
    if (fs_->exists(source) || !ensureDirectoryTree(source)) {
      message = "folder create failed";
      return false;
    }
  } else if (strcmp(operation, "renameFolder") == 0) {
    const char *newFolder = jsonString(root, "newFolderPath");
    if (!safeFolder(newFolder, false) || !fs_->exists(source)) {
      message = "invalid folder rename";
      return false;
    }
    const String target = String(kCapsuleRoot) + "/" + newFolder;
    if (fs_->exists(target) || !ensureDirectoryTree(parentPath(target)) ||
        !fs_->rename(source, target)) {
      message = "folder rename failed";
      return false;
    }
  } else if (strcmp(operation, "deleteFolderToInbox") == 0) {
    std::vector<String> files;
    collectFiles(source, "", 0, files);
    for (const String &file : files) {
      if (!file.endsWith("/capsule.json") && file != "capsule.json") continue;
      const String relativeDirectory = file.substring(0, file.length() - strlen("/capsule.json"));
      const int slash = relativeDirectory.lastIndexOf('/');
      const String id = slash < 0 ? relativeDirectory : relativeDirectory.substring(slash + 1);
      if (!isUuid(id.c_str())) continue;
      const String capsuleSource = source + "/" + relativeDirectory;
      const String target = String(kCapsuleInbox) + "/" + id;
      if (fs_->exists(target) || !fs_->rename(capsuleSource, target) || !touchCapsule(target)) {
        message = "folder evacuation failed";
        return false;
      }
    }
    if (!removeTree(source)) {
      message = "folder cleanup failed";
      return false;
    }
  }
  message = "committed";
  return true;
}

bool PokePodLinkService::cleanupPurgeStaging() {
  if (fs_ == nullptr) return false;
  File root = fs_->open(kCapsuleStaging);
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return true;
  }
  std::vector<String> purgeDirectories;
  File entry = root.openNextFile();
  while (entry) {
    const String full = entry.name();
    const bool isDirectory = entry.isDirectory();
    entry.close();
    const int slash = full.lastIndexOf('/');
    const String name = slash >= 0 ? full.substring(slash + 1) : full;
    if (isDirectory && purgeStagingDirectoryName(name.c_str())) {
      purgeDirectories.push_back(String(kCapsuleStaging) + "/" + name);
    }
    entry = root.openNextFile();
  }
  root.close();
  bool ok = true;
  for (const String &directory : purgeDirectories) {
    if (!removeTree(directory)) ok = false;
  }
  return ok;
}

bool PokePodLinkService::removeTree(const String &path) {
  File root = fs_->open(path);
  if (!root) return true;
  if (!root.isDirectory()) {
    root.close();
    return fs_->remove(path);
  }
  std::vector<String> children;
  File entry = root.openNextFile();
  while (entry) {
    const String full = entry.name();
    entry.close();
    const int slash = full.lastIndexOf('/');
    const String name = slash >= 0 ? full.substring(slash + 1) : full;
    children.push_back(path + "/" + name);
    entry = root.openNextFile();
  }
  root.close();
  for (const String &child : children) {
    if (!removeTree(child)) return false;
  }
  return fs_->rmdir(path);
}

bool PokePodLinkService::copyTree(const String &source, const String &target,
                                  uint8_t depth) {
  if (depth > 6) return false;
  File input = fs_->open(source, FILE_READ);
  if (!input) return false;
  if (!input.isDirectory()) {
    File output = fs_->open(target, FILE_WRITE);
    if (!output) {
      input.close();
      return false;
    }
    uint8_t buffer[4096];
    bool ok = true;
    while (input.available()) {
      const size_t count = input.read(buffer, sizeof(buffer));
      if (count == 0 || output.write(buffer, count) != count) {
        ok = false;
        break;
      }
    }
    output.flush();
    output.close();
    input.close();
    return ok;
  }
  input.close();
  if (!fs_->exists(target) && !fs_->mkdir(target)) return false;
  File directory = fs_->open(source);
  File entry = directory.openNextFile();
  while (entry) {
    const String full = entry.name();
    entry.close();
    const int slash = full.lastIndexOf('/');
    const String name = slash < 0 ? full : full.substring(slash + 1);
    if (!copyTree(source + "/" + name, target + "/" + name, depth + 1)) {
      directory.close();
      return false;
    }
    entry = directory.openNextFile();
  }
  directory.close();
  return true;
}

bool PokePodLinkService::rewriteCopiedMetadata(const String &directory,
                                               const String &id) {
  const String capsulePath = directory + "/capsule.json";
  const String processingPath = directory + "/processing.json";
  cJSON *capsule = cJSON_Parse(readText(capsulePath, 8192).c_str());
  cJSON *processing = cJSON_Parse(readText(processingPath, 8192).c_str());
  if (capsule == nullptr || processing == nullptr) {
    cJSON_Delete(capsule);
    cJSON_Delete(processing);
    return false;
  }
  replaceStringOrNull(capsule, "id", id.c_str());
  replaceStringOrNull(capsule, "createdAt", board_->utcNow().c_str());
  replaceStringOrNull(capsule, "updatedAt", board_->utcNow().c_str());
  cJSON_ReplaceItemInObjectCaseSensitive(capsule, "revision", cJSON_CreateNumber(1));
  replaceStringOrNull(processing, "capsuleId", id.c_str());
  cJSON *processingRevision = cJSON_GetObjectItemCaseSensitive(processing, "revision");
  if (!cJSON_IsNumber(processingRevision)) {
    cJSON_Delete(capsule);
    cJSON_Delete(processing);
    return false;
  }
  cJSON_SetNumberValue(processingRevision, processingRevision->valueint + 1);
  const bool ok = writeTextAtomic(capsulePath, printed(capsule) + "\n") &&
      writeTextAtomic(processingPath, printed(processing) + "\n");
  cJSON_Delete(capsule);
  cJSON_Delete(processing);
  return ok;
}

String PokePodLinkService::newUuid() const {
  uint8_t bytes[16];
  for (size_t offset = 0; offset < sizeof(bytes); offset += 4) {
    const uint32_t value = esp_random();
    memcpy(bytes + offset, &value, sizeof(value));
  }
  char result[37];
  formatUuidV4(bytes, result);
  return String(result);
}

String PokePodLinkService::provisioningDiagnosticsJson() const {
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

bool PokePodLinkService::executeCommand(const String &path,
                                        const String &transactionId,
                                        String &message) {
  const String source = readText(path, kMaxCommandJsonBytes);
  cJSON *root = cJSON_ParseWithLength(source.c_str(), source.length());
  bool success = false;
  bool completedMaintenance = false;
  if (root == nullptr || !cJSON_IsObject(root)) {
    message = "command JSON is malformed";
  } else if (jsonInt64(root, "schemaVersion") != 2 ||
             !sameUuid(jsonString(root, "transactionId"), transactionId.c_str())) {
    message = "command schema or transactionId is invalid";
  } else {
    const char *operation = jsonString(root, "operation");
    if (operation == nullptr) {
      message = "command operation is missing";
    } else if (strcmp(operation, "rescan") == 0) {
      success = library_->scan();
      message = success ? "committed" : "rescan failed";
    } else {
      // The maintenance lifecycle is persisted in the command result directory.
      // Mutating commands are implemented by the shared capsule library below;
      // unsupported high-level organization commands fail explicitly and never
      // partially mutate the SD card.
      const char *maintenanceId = jsonString(root, "maintenanceId");
      if (strcmp(operation, "beginMaintenance") == 0) {
        if (!isUuid(maintenanceId)) message = "invalid maintenanceId";
        else if (!activeMaintenance_.isEmpty() && !activeMaintenance_.equalsIgnoreCase(maintenanceId)) {
          message = "another maintenance session is active";
        } else {
          activeMaintenance_ = maintenanceId;
          // A newly accepted transaction immediately invalidates the prior
          // session's completed presentation, even if persisting this command
          // result later fails.
          maintenanceCompletion_.beginAccepted();
          success = true;
          message = "committed";
        }
      } else if (strcmp(operation, "endMaintenance") == 0) {
        if (activeMaintenance_.isEmpty() || !activeMaintenance_.equalsIgnoreCase(maintenanceId)) {
          message = "maintenance session does not own the device";
        } else {
          activeMaintenance_ = "";
          success = true;
          completedMaintenance = true;
          message = "committed";
        }
      } else if (activeMaintenance_.isEmpty() ||
                 !activeMaintenance_.equalsIgnoreCase(maintenanceId)) {
        message = "maintenance session does not own the device";
      } else {
        std::vector<String> commandIds;
        const bool hasIds = collectCommandIds(root, commandIds);
        const char *id = hasIds && commandIds.size() == 1
            ? commandIds[0].c_str() : nullptr;
        if (strcmp(operation, "createFolder") == 0 ||
            strcmp(operation, "renameFolder") == 0) {
          success = folderOperation(root, operation, message);
        } else if (strcmp(operation, "deleteFolderToInbox") == 0) {
          if (!hasIds || !validateExpectedRevisions(
                  root, commandIds, false, false, message)) {
            success = false;
          } else success = folderOperation(root, operation, message);
        } else if ((strcmp(operation, "moveCapsules") == 0 ||
                    strcmp(operation, "copyCapsules") == 0) && hasIds) {
          success = moveOrCopy(root, strcmp(operation, "copyCapsules") == 0,
                               commandIds, message);
        } else if ((strcmp(operation, "deleteCapsules") == 0 ||
                    strcmp(operation, "restoreCapsules") == 0 ||
                    strcmp(operation, "purgeCapsules") == 0) && hasIds) {
          success = trashOperation(root, operation, commandIds, message);
        } else if ((strcmp(operation, "setFavorite") == 0 ||
                    strcmp(operation, "addTags") == 0 ||
                    strcmp(operation, "removeTags") == 0 ||
                    strcmp(operation, "renameTag") == 0 ||
                    strcmp(operation, "mergeTag") == 0 ||
                    strcmp(operation, "deleteTag") == 0) && hasIds) {
          if (commandIds.empty() && (strcmp(operation, "renameTag") == 0 ||
                                     strcmp(operation, "mergeTag") == 0 ||
                                     strcmp(operation, "deleteTag") == 0)) {
            success = true;
            message = "committed";
          } else {
            success = mutateFavoriteOrTags(root, operation, commandIds, message);
          }
        } else if (strcmp(operation, "requeueTranscription") == 0 && isUuid(id)) {
          if (!validateExpectedRevisions(root, commandIds, true, false, message)) {
            success = false;
          } else {
            const CapsuleSummary *record = library_->find(id);
            if (record == nullptr || record->status != CapsuleStatus::failed) {
              message = "only failed capsules can be requeued";
            } else {
              success = library_->requeue(id);
              if (success) tencent_->wake();
              message = success ? "committed" : "requeue failed";
            }
          }
        } else if (strcmp(operation, "commitCorrection") == 0 && isUuid(id)) {
          const CapsuleSummary *record = library_->find(id);
          const char *stagedPath = jsonString(root, "stagedPath");
          const String expectedStaged = ".staging/" + transactionId + "/" + id;
          const String staged = String(kCapsuleRoot) + "/" +
              (stagedPath == nullptr ? "" : stagedPath) + "/polished.md";
          const String text = readText(staged, 1024 * 1024);
          if (record == nullptr || stagedPath == nullptr ||
              String(stagedPath) != expectedStaged || text.isEmpty()) {
            message = "invalid staged correction";
          } else {
            const String processingPath = record->directory + "/processing.json";
            const String polishedPath = record->directory + "/polished.md";
            const bool hadPolished = fs_->exists(polishedPath);
            const String previousPolished = hadPolished
                ? readText(polishedPath, 1024 * 1024) : String();
            const String processingText = readText(processingPath, 8192);
            cJSON *processing = cJSON_Parse(processingText.c_str());
            if (processing == nullptr) message = "processing metadata is malformed";
            else {
              cJSON *revision = cJSON_GetObjectItemCaseSensitive(processing, "revision");
              const int expected = static_cast<int>(jsonInt64(root, "expectedRevision"));
              if (!cJSON_IsNumber(revision) || revision->valueint != expected) {
                message = "processing revision conflict";
              } else if (!writeTextAtomic(polishedPath, text)) {
                message = "polished text commit failed";
              } else {
                cJSON_SetNumberValue(revision, expected + 1);
                cJSON_ReplaceItemInObjectCaseSensitive(processing, "status", cJSON_CreateString("ready"));
                replaceStringOrNull(processing, "polishedTextFile", "polished.md");
                replaceStringOrNull(processing, "errorStage", nullptr);
                replaceStringOrNull(processing, "error", nullptr);
                success = writeTextAtomic(processingPath, printed(processing) + "\n");
                message = success ? "committed" : "processing metadata commit failed";
                if (success) {
                  const String stagedDirectory = String(kCapsuleRoot) + "/" + expectedStaged;
                  removeTree(stagedDirectory);
                  fs_->rmdir(parentPath(stagedDirectory));
                } else if (hadPolished) {
                  writeTextAtomic(polishedPath, previousPolished);
                } else {
                  fs_->remove(polishedPath);
                }
              }
              cJSON_Delete(processing);
            }
          }
        } else if (strcmp(operation, "commitFinalText") == 0 && isUuid(id)) {
          const CapsuleSummary *record = library_->find(id);
          const char *inlineText = jsonString(root, "finalText");
          String text = inlineText == nullptr ? String() : String(inlineText);
          const char *stagedPath = jsonString(root, "stagedPath");
          const String expectedStaged = ".staging/" + transactionId + "/" + id;
          const String stagedFinal = String(kCapsuleRoot) + "/" +
              expectedStaged + "/final.md";
          const bool stagedFinalExists = fs_->exists(stagedFinal);
          if (inlineText == nullptr && stagedPath != nullptr &&
              String(stagedPath) == expectedStaged) {
            text = readText(stagedFinal, 1024 * 1024);
          }
          if (record == nullptr || (inlineText == nullptr && !stagedFinalExists)) {
            message = "invalid final text command";
          } else {
            const String capsulePath = record->directory + "/capsule.json";
            const String finalPath = record->directory + "/final.md";
            const bool hadFinal = fs_->exists(finalPath);
            const String previousFinal = hadFinal
                ? readText(finalPath, 1024 * 1024) : String();
            cJSON *capsule = cJSON_Parse(readText(capsulePath, 8192).c_str());
            cJSON *revision = capsule == nullptr ? nullptr :
                cJSON_GetObjectItemCaseSensitive(capsule, "revision");
            const int expected = static_cast<int>(jsonInt64(root, "expectedRevision"));
            if (!cJSON_IsNumber(revision) || revision->valueint != expected) {
              message = "capsule revision conflict";
            } else if (!writeTextAtomic(finalPath, text)) {
              message = "final text commit failed";
            } else {
              cJSON_SetNumberValue(revision, expected + 1);
              replaceStringOrNull(capsule, "updatedAt", board_->utcNow().c_str());
              success = writeTextAtomic(capsulePath, printed(capsule) + "\n");
              message = success ? "committed" : "capsule metadata commit failed";
              if (success && stagedPath != nullptr) {
                const String stagedDirectory = String(kCapsuleRoot) + "/" + expectedStaged;
                removeTree(stagedDirectory);
                fs_->rmdir(parentPath(stagedDirectory));
              } else if (!success && hadFinal) {
                writeTextAtomic(finalPath, previousFinal);
              } else if (!success) {
                fs_->remove(finalPath);
              }
            }
            cJSON_Delete(capsule);
          }
        } else if (strcmp(operation, "commitImport") == 0 && isUuid(id)) {
          const char *stagedPath = jsonString(root, "stagedPath");
          const char *destination = jsonString(root, "destination");
          const String staged = String(kCapsuleRoot) + "/" +
              (stagedPath == nullptr ? "" : stagedPath);
          const String targetFolder = folderDirectory(destination);
          const String target = targetFolder + "/" + id;
          const String capsuleText = readText(staged + "/capsule.json", 8192);
          const String processingText = readText(staged + "/processing.json", 8192);
          cJSON *capsule = cJSON_Parse(capsuleText.c_str());
          cJSON *processing = cJSON_Parse(processingText.c_str());
          const char *metadataId = capsule == nullptr ? nullptr : jsonString(capsule, "id");
          const char *processingId = processing == nullptr ? nullptr :
              jsonString(processing, "capsuleId");
          const char *audioFile = processing == nullptr ? nullptr :
              jsonString(processing, "audioFile");
          const int64_t processingSchema = processing == nullptr ? -1 :
              jsonInt64(processing, "schemaVersion");
          const String expectedStaged = ".staging/" + transactionId + "/" + id;
          if (stagedPath == nullptr || String(stagedPath) != expectedStaged ||
              targetFolder.isEmpty() || !sameUuid(id, metadataId) ||
              !sameUuid(id, processingId) ||
              (processingSchema != 1 && processingSchema != 2) ||
              !safeCapsuleFileName(audioFile) ||
              !fs_->exists(staged + "/" + audioFile) ||
              !fs_->exists(targetFolder) || fs_->exists(target)) {
            message = "invalid staged import";
          } else {
            success = fs_->rename(staged, target);
            message = success ? "committed" : "import commit failed";
            if (success) fs_->rmdir(parentPath(staged));
          }
          cJSON_Delete(capsule);
          cJSON_Delete(processing);
        } else {
          message = "command operation is not supported by this firmware";
        }
      }
    }
  }

  cJSON *result = cJSON_CreateObject();
  cJSON_AddNumberToObject(result, "schemaVersion", 1);
  cJSON_AddStringToObject(result, "commandId", transactionId.c_str());
  cJSON_AddStringToObject(result, "transactionId", transactionId.c_str());
  cJSON_AddBoolToObject(result, "ok", success);
  cJSON_AddBoolToObject(result, "success", success);
  cJSON_AddStringToObject(result, "message", message.c_str());
  cJSON_AddStringToObject(result, "completedAt", board_->utcNow().c_str());
  const String resultPath = String(kCapsuleSystem) + "/commands/results/" +
      transactionId + ".json";
  const bool persisted = writeTextAtomic(resultPath, printed(result) + "\n");
  cJSON_Delete(result);
  cJSON_Delete(root);
  library_->scan();
  if (persisted && completedMaintenance) {
    maintenanceCompletion_.endResultPersisted(transactionId.c_str());
  }
  return persisted;
}

bool PokePodLinkService::sendOk(uint32_t requestId, const char *extraJson) {
  String value = "{\"status\":\"ok\",\"version\":2";
  if (extraJson != nullptr && extraJson[0] != '\0') {
    value += ',';
    value += extraJson;
  }
  value += '}';
  return sendJson(requestId, value);
}

void PokePodLinkService::sendBusy(uint32_t requestId, uint32_t retryAfterMs) {
  sendJson(requestId, "{\"status\":\"busy\",\"version\":2,\"retryAfterMs\":" +
      String(retryAfterMs) + "}");
}

void PokePodLinkService::sendError(uint32_t requestId, const char *message) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "status", "error");
  cJSON_AddNumberToObject(root, "version", 2);
  cJSON_AddStringToObject(root, "message", message == nullptr ? "error" : message);
  sendJson(requestId, printed(root));
  cJSON_Delete(root);
}

bool PokePodLinkService::sendJson(uint32_t requestId, const String &json) {
  if (json.length() > kLinkMaxControlBytes) return false;
  return sendFrame(LinkFrameType::responseJson, 0, requestId,
                   reinterpret_cast<const uint8_t *>(json.c_str()), json.length());
}

bool PokePodLinkService::sendEvent(uint32_t requestId, const String &json) {
  if (json.length() > kLinkMaxControlBytes) return false;
  return sendFrame(LinkFrameType::eventJson, 0, requestId,
                   reinterpret_cast<const uint8_t *>(json.c_str()), json.length());
}

bool PokePodLinkService::sendFile(uint32_t requestId, const String &path) {
  if (!transferPermitted()) return false;
  File file = fs_->open(path, FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    sendError(requestId, "file is missing");
    return false;
  }
  const size_t length = file.size();
  if (!sendOk(requestId,
              ("\"available\":true,\"binaryLength\":" +
               String(length)).c_str())) {
    file.close();
    return false;
  }
  size_t sent = 0;
  while (sent < length) {
    if (!transferPermitted()) {
      file.close();
      return false;
    }
    const size_t count = file.read(
        payload_, std::min<size_t>(kLinkMaxDataBytes, length - sent));
    if (count == 0) {
      file.close();
      return false;
    }
    sent += count;
    if (!sendFrame(LinkFrameType::data, sent == length ? 1 : 0,
                   requestId, payload_, count)) {
      file.close();
      return false;
    }
  }
  file.close();
  return true;
}

bool PokePodLinkService::sendFrame(LinkFrameType type, uint16_t flags,
                                   uint32_t requestId,
                                   const uint8_t *payload, size_t size) {
  if (stream_ == nullptr || !transferPermitted() || size >
      (type == LinkFrameType::data ? kLinkMaxDataBytes : kLinkMaxControlBytes)) {
    return false;
  }
  LinkFrameHeader header;
  header.type = type;
  header.flags = flags;
  header.requestId = requestId;
  header.payloadLength = size;
  header.payloadCrc32 = linkCrc32(payload, size);
  uint8_t encoded[kLinkHeaderBytes];
  if (!encodeLinkHeader(header, encoded, sizeof(encoded))) return false;
  const auto writeAll = [this](const uint8_t *data, size_t length) {
    size_t offset = 0;
    while (offset < length) {
      if (!transferPermitted()) return false;
      const size_t written = stream_->write(
          data + offset, std::min<size_t>(512, length - offset));
      if (written == 0 || !transferPermitted()) return false;
      offset += written;
      yield();
    }
    return true;
  };
  return writeAll(encoded, sizeof(encoded)) &&
      (size == 0 || writeAll(payload, size));
}

bool PokePodLinkService::transferPermitted() const {
  return linkTransferPermitted(transferGate_, millis());
}

void PokePodLinkService::rememberCompleted(uint32_t requestId) {
  if (requestId != 0) completed_.complete(requestId);
}

bool PokePodLinkService::ensureDirectoryTree(const String &path) {
  if (fs_ == nullptr || path.isEmpty() || path[0] != '/') return false;
  String current;
  for (size_t index = 1; index <= path.length(); ++index) {
    if (index == path.length() || path[index] == '/') {
      current = path.substring(0, index);
      if (!fs_->exists(current) && !fs_->mkdir(current)) return false;
    }
  }
  return true;
}

bool PokePodLinkService::writeTextAtomic(const String &path,
                                         const String &text) {
  const String temporary = path + ".tmp";
  if (fs_->exists(temporary)) fs_->remove(temporary);
  File file = fs_->open(temporary, FILE_WRITE);
  if (!file) return false;
  const bool wrote = file.print(text) == text.length() && file.getWriteError() == 0;
  file.flush();
  file.close();
  if (!wrote) {
    fs_->remove(temporary);
    return false;
  }
  if (fs_->exists(path) && !fs_->remove(path)) return false;
  return fs_->rename(temporary, path);
}

bool PokePodLinkService::validFontFile(const String &path) const {
  File file = fs_->open(path, FILE_READ);
  uint8_t header[kFontHeaderBytes];
  if (!file || file.isDirectory() ||
      file.read(header, sizeof(header)) != sizeof(header)) {
    if (file) file.close();
    return false;
  }
  const uint32_t count = linkGet32(header + 12);
  const uint32_t entrySize = linkGet32(header + 16);
  const bool ok = memcmp(header, "PKF2", 4) == 0 &&
      validFontLayout(linkGet16(header + 4), linkGet16(header + 6),
                      header[8], count, entrySize, file.size());
  file.close();
  return ok;
}

String PokePodLinkService::readText(const String &path, size_t limit) const {
  if (fs_ == nullptr) return String();
  File file = fs_->open(path, FILE_READ);
  if (!file || file.isDirectory() || file.size() > limit) {
    if (file) file.close();
    return String();
  }
  String value;
  if (!value.reserve(file.size() + 1)) {
    file.close();
    return String();
  }
  while (file.available()) value += static_cast<char>(file.read());
  file.close();
  return value;
}

bool PokePodLinkService::collectFiles(const String &directory,
                                      const String &relative, uint8_t depth,
                                      std::vector<String> &files) const {
  if (!transferPermitted() || depth > 5 || files.size() >= 2048) return false;
  File root = fs_->open(directory);
  if (!root || !root.isDirectory()) return false;
  File entry = root.openNextFile();
  while (entry && transferPermitted()) {
    const String full = entry.name();
    const bool isDirectory = entry.isDirectory();
    entry.close();
    const int slash = full.lastIndexOf('/');
    const String name = slash >= 0 ? full.substring(slash + 1) : full;
    const String childRelative = relative.isEmpty() ? name : relative + "/" + name;
    if (isDirectory) {
      if (!hiddenReadDenied(childRelative)) {
        if (!collectFiles(directory + "/" + name, childRelative,
                          depth + 1, files)) {
          root.close();
          return false;
        }
      }
    } else if (!hiddenReadDenied(childRelative)) {
      files.push_back(childRelative);
    }
    entry = root.openNextFile();
  }
  root.close();
  return transferPermitted();
}

String PokePodLinkService::metadataFingerprint() const {
  std::vector<String> files;
  if (!collectFiles(kCapsuleRoot, "", 0, files)) return String();
  std::sort(files.begin(), files.end());
  uint64_t hash = 1469598103934665603ULL;
  uint8_t buffer[512];
  for (const String &relative : files) {
    if (!transferPermitted()) return String();
    if (!metadataName(relative)) continue;
    hash = fnvUpdate(hash, reinterpret_cast<const uint8_t *>(relative.c_str()), relative.length());
    File file = fs_->open(String(kCapsuleRoot) + "/" + relative, FILE_READ);
    while (file && file.available() && transferPermitted()) {
      const size_t count = file.read(buffer, sizeof(buffer));
      hash = fnvUpdate(hash, buffer, count);
    }
    if (file) file.close();
    if (!transferPermitted()) return String();
  }
  char value[24];
  snprintf(value, sizeof(value), "%016llx", static_cast<unsigned long long>(hash));
  return String(value);
}

String PokePodLinkService::deviceId() const {
  char value[24];
  formatPokePodDeviceId(ESP.getEfuseMac(), value);
  return String(value);
}

bool PokePodLinkService::acquireRequestLease(uint32_t requestId) {
  if (requestLeaseHeld_) return true;
  if (coordinator_ == nullptr || transport_ == LinkTransport::none) {
    requestLeaseHeld_ = true;
    return true;
  }
  if (!coordinator_->acquire(transport_)) {
    sendBusy(requestId, 250);
    return false;
  }
  requestLeaseHeld_ = true;
  return true;
}

void PokePodLinkService::releaseRequestLease() {
  if (!requestLeaseHeld_) return;
  if (coordinator_ != nullptr && transport_ != LinkTransport::none) {
    coordinator_->release(transport_);
  }
  requestLeaseHeld_ = false;
}

bool PokePodLinkService::foregroundBusy() const {
  if (recorder_ == nullptr || tencent_ == nullptr || bleVoice_ == nullptr ||
      captureRouter_ == nullptr) return true;
  if (audio_ != nullptr && audio_->playing()) return true;
  return linkForegroundBusy(recorder_->recording(), tencent_->working(),
                            bleVoice_->streaming(),
                            captureRouter_->available());
}

}  // namespace pokepod
