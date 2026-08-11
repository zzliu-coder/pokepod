#pragma once

#include <Arduino.h>
#include <FS.h>
#include <mbedtls/sha256.h>
#include <vector>

#include "LinkFrame.h"
#include "LinkPolicy.h"
#include "LinkServiceCoordinator.h"
#include "LinkManifestStepper.h"
#include "LinkTransferGate.h"
#include "LinkTransferStepper.h"
#include "MaintenanceCompletionTracker.h"
#include "CapsuleTransaction.h"
#include "CapabilityRegistry.h"
#include "StorageCoordinator.h"

namespace pokepod {

class BoardServices;
class AudioPipeline;
class AudioCaptureRuntime;
class AudioCaptureRouter;
class CapsuleLibrary;
class DeviceConfig;
class Dashboard;
class TencentWorker;
class UsbLinkBridge;
class BleVoiceService;
class WavRecorder;
class WifiController;
class ProvisioningDiagnostics;
class PowerDiagnostics;
class ProvisioningCoordinator;
class RuntimePowerManager;
class WirelessSyncPairingProvider;

class PokePodLinkService {
 public:
  bool begin(Stream &stream, fs::FS &fs, BoardServices &board,
             AudioPipeline &audio,
             AudioCaptureRouter &captureRouter,
             UsbLinkBridge &usb, BleVoiceService &bleVoice,
             Dashboard &dashboard,
             CapsuleLibrary &library,
             WavRecorder &recorder, DeviceConfig &config,
             WifiController &wifi, TencentWorker &tencent,
             ProvisioningDiagnostics &provisioningDiagnostics,
             PowerDiagnostics &powerDiagnostics,
             RuntimePowerManager &power, Print &log,
             LinkServiceCoordinator *coordinator = nullptr,
             LinkTransport transport = LinkTransport::none,
             WirelessSyncPairingProvider *pairingProvider = nullptr,
             LinkTransferGate *transferGate = nullptr,
             ProvisioningCoordinator *provisioningCoordinator = nullptr,
             LinkWriteChannel *writeChannel = nullptr,
             AudioCaptureRuntime *captureRuntime = nullptr,
             const CapabilityRegistry *capabilities = nullptr);
  void poll(uint32_t nowMs);
  // Finishes read-only handle cleanup after an immediate transport cancel.
  // This never reads frames or writes responses, so a Wi-Fi service can call
  // it before authentication and while its five-minute window is closed.
  void pollDeferredCleanup();
  void disconnect();
  bool active() const { return sessionActive_; }
  bool receivingBinary() const { return incomingKind_ != IncomingKind::none; }
  bool maintenanceActive() const { return !activeMaintenance_.isEmpty(); }
  uint32_t maintenanceStartRevision() const {
    return maintenanceCompletion_.startRevision();
  }
  uint32_t maintenanceCompletionRevision() const {
    return maintenanceCompletion_.completionRevision();
  }
  uint32_t maintenanceCompletedStartRevision() const {
    return maintenanceCompletion_.completedStartRevision();
  }

 private:
  enum class ReceivePhase : uint8_t { magic, header, payload };
  enum class IncomingKind : uint8_t { none, stagedFile, command, systemFont };
  enum class OutgoingPhase : uint8_t { none, response, data };
  enum class TxCompletion : uint8_t {
    none,
    fileResponse,
    fileData,
    fileFinal,
  };

  struct ManifestDirectoryCursor {
    File directory;
    String absolute;
    String relative;
    uint8_t depth = 0;
  };

  void consumeByte(uint8_t value);
  void resetFrame();
  void processFrame();
  void processRequest(uint32_t requestId, const uint8_t *payload, size_t size);
  void processData(uint32_t requestId, uint16_t flags,
                   const uint8_t *payload, size_t size);
  void finishIncoming();
  void failIncoming(const char *message);

  void handleImmediate(uint32_t requestId, void *jsonRoot);
  void handleRead(uint32_t requestId, void *jsonRoot);
  bool beginManifest(uint32_t requestId, LinkManifestMode mode,
                     size_t cursor = 0);
  void advanceManifest(uint32_t nowMs);
  void advanceManifestScan();
  void advanceManifestFile();
  void advanceManifestHash();
  void finishManifestResponse();
  void failManifest(const char *message);
  void abortManifest();
  bool cleanupManifestStorage();
  void finishPendingManifestResponse();
  void finishPendingManifestFailure();
  void handleConfigure(uint32_t requestId, void *jsonRoot);
  void handleCommandFile(uint32_t requestId, const String &path,
                         const String &transactionId);
  bool executeCommand(const String &path, const String &transactionId,
                      String &message);

  bool sendOk(uint32_t requestId, const char *extraJson = nullptr);
  void sendBusy(uint32_t requestId, uint32_t retryAfterMs = 150);
  void sendError(uint32_t requestId, const char *message);
  bool sendJson(uint32_t requestId, const String &json);
  bool sendEvent(uint32_t requestId, const String &json);
  bool sendFile(uint32_t requestId, const String &path,
                const char *resultTransactionId = nullptr);
  bool sendFrame(LinkFrameType type, uint16_t flags, uint32_t requestId,
                 const uint8_t *payload, size_t size);
  bool queueFrame(LinkFrameType type, uint16_t flags, uint32_t requestId,
                  const uint8_t *payload, size_t size,
                  TxCompletion completion);
  void advanceTransmit(uint32_t nowMs);
  void queueNextFileChunk();
  void finishOutgoingFile(bool success);
  void abortOutgoing();
  void onFrameSent(TxCompletion completion);
  void releaseRequestLeaseNow();
  bool drainLinkCapture();
  bool stopLinkRecording(bool commit);
  void rememberCompleted(uint32_t requestId);

  bool beginIncoming(IncomingKind kind, uint32_t requestId,
                     uint32_t expectedBytes, const String &temporaryPath,
                     const String &finalPath, const String &transactionId,
                     bool chunkAcks);
  bool ensureDirectoryTree(const String &path);
  bool writeTextAtomic(const String &path, const String &text);
  bool validFontFile(const String &path) const;
  String readText(const String &path, size_t limit) const;
  bool collectFiles(const String &directory, const String &relative,
                    uint8_t depth, std::vector<String> &files) const;
  String deviceId() const;
  bool foregroundBusy() const;
  bool acquireRequestLease(uint32_t requestId);
  void releaseRequestLease();
  bool safeFolder(const char *value, bool allowBuiltIn = true) const;
  String folderDirectory(const char *value) const;
  String activeCapsuleDirectory(const String &id) const;
  bool collectCommandIds(void *jsonRoot, std::vector<String> &ids) const;
  bool validateExpectedRevisions(void *jsonRoot,
                                 const std::vector<String> &ids,
                                 bool processing, bool trash,
                                 String &message) const;
  bool touchCapsule(const String &directory);
  bool mutateFavoriteOrTags(void *jsonRoot, const char *operation,
                            const std::vector<String> &ids, String &message);
  bool moveOrCopy(void *jsonRoot, bool copy,
                  const std::vector<String> &ids, String &message);
  bool trashOperation(void *jsonRoot, const char *operation,
                      const std::vector<String> &ids, String &message);
  bool folderOperation(void *jsonRoot, const char *operation,
                       String &message);
  bool cleanupPurgeStaging();
  bool removeTree(const String &path);
  bool copyTree(const String &source, const String &target, uint8_t depth = 0);
  bool rewriteCopiedMetadata(const String &directory, const String &id);
  String newUuid() const;
  String provisioningDiagnosticsJson() const;
  String powerDiagnosticsJson() const;
  bool transferPermitted() const;
  StorageOwner storageOwner() const;

  Stream *stream_ = nullptr;
  fs::FS *fs_ = nullptr;
  BoardServices *board_ = nullptr;
  AudioPipeline *audio_ = nullptr;
  AudioCaptureRuntime *captureRuntime_ = nullptr;
  const CapabilityRegistry *capabilities_ = nullptr;
  AudioCaptureRouter *captureRouter_ = nullptr;
  UsbLinkBridge *usb_ = nullptr;
  BleVoiceService *bleVoice_ = nullptr;
  Dashboard *dashboard_ = nullptr;
  CapsuleLibrary *library_ = nullptr;
  WavRecorder *recorder_ = nullptr;
  DeviceConfig *config_ = nullptr;
  WifiController *wifi_ = nullptr;
  TencentWorker *tencent_ = nullptr;
  ProvisioningDiagnostics *provisioningDiagnostics_ = nullptr;
  PowerDiagnostics *powerDiagnostics_ = nullptr;
  ProvisioningCoordinator *provisioningCoordinator_ = nullptr;
  RuntimePowerManager *power_ = nullptr;
  Print *log_ = nullptr;
  LinkServiceCoordinator *coordinator_ = nullptr;
  LinkTransport transport_ = LinkTransport::none;
  WirelessSyncPairingProvider *pairingProvider_ = nullptr;
  LinkTransferGate *transferGate_ = nullptr;
  LinkWriteChannel *writeChannel_ = nullptr;
  bool requestLeaseHeld_ = false;
  bool releaseRequestLeaseWhenTxDrained_ = false;
  CapsuleTransaction transaction_;
  StorageReservation incomingStorageReservation_;
  bool commandStorageActive_ = false;
  bool linkOwnedRecording_ = false;

  ReceivePhase receivePhase_ = ReceivePhase::magic;
  uint8_t headerBytes_[kLinkHeaderBytes] = {};
  size_t headerUsed_ = 0;
  LinkFrameHeader currentHeader_;
  // Link v2 needs a full data-frame buffer. There are two service instances
  // (USB and Wi-Fi); keeping both 16 KiB buffers in internal DRAM starves the
  // Wi-Fi driver of large contiguous blocks when the provisioning AP starts.
  // The board has mandatory PSRAM, so allocate these long-lived buffers there.
  uint8_t *payload_ = nullptr;
  uint8_t *txFrame_ = nullptr;
  uint8_t *pendingControlFrame_ = nullptr;
  size_t payloadUsed_ = 0;
  uint8_t magicMatched_ = 0;
  bool frameProcessedThisPoll_ = false;
  bool sessionActive_ = false;
  LinkRequestHistory completed_;

  IncomingKind incomingKind_ = IncomingKind::none;
  uint32_t incomingRequestId_ = 0;
  uint32_t incomingExpected_ = 0;
  uint32_t incomingReceived_ = 0;
  bool incomingChunkAcks_ = false;
  File incomingFile_;
  String incomingTemporaryPath_;
  String incomingFinalPath_;
  String incomingTransactionId_;
  uint32_t incomingLastByteMs_ = 0;
  uint32_t rebootAtMs_ = 0;
  String activeMaintenance_;
  MaintenanceCompletionTracker maintenanceCompletion_;

  LinkTransferStepper txStepper_;
  size_t txFrameBytes_ = 0;
  TxCompletion txCompletion_ = TxCompletion::none;
  size_t pendingControlBytes_ = 0;
  TxCompletion pendingControlCompletion_ = TxCompletion::none;
  OutgoingPhase outgoingPhase_ = OutgoingPhase::none;
  uint32_t outgoingRequestId_ = 0;
  size_t outgoingLength_ = 0;
  size_t outgoingRead_ = 0;
  File outgoingFile_;
  String outgoingResultTransactionId_;
  StorageReservation outgoingStorageReservation_;

  LinkManifestStepper manifestStepper_;
  std::vector<ManifestDirectoryCursor> manifestDirectories_;
  File manifestFile_;
  StorageReservation manifestStorageReservation_;
  uint32_t manifestRequestId_ = 0;
  bool manifestRootOpened_ = false;
  bool manifestShaActive_ = false;
  mbedtls_sha256_context manifestSha_;
  String manifestError_;
  bool manifestCleanupPending_ = false;
  uint32_t manifestResponseRequestId_ = 0;
  String manifestResponseJson_;
  uint32_t manifestFailureRequestId_ = 0;
  String manifestFailureMessage_;
};

}  // namespace pokepod
