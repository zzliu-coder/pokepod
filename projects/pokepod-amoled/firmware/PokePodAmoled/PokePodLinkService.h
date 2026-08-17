#pragma once

#include <Arduino.h>
#include <FS.h>
#include <mbedtls/sha256.h>
#include <algorithm>
#include <cstring>
#include <vector>

#include "LinkFrame.h"
#include "LinkFileTransfer.h"
#include "LinkCapsuleTransactionGate.h"
#include "LinkBoundedTextRead.h"
#include "LinkRecordingSession.h"
#include "LinkDiagnostics.h"
#include "LinkPolicy.h"
#include "LinkServiceCoordinator.h"
#include "LinkManifestStepper.h"
#include "LinkLivenessProbe.h"
#include "LinkPollBudget.h"
#include "LinkCommandExecutor.h"
#include "LinkOperation.h"
#include "LinkTreeStepper.h"
#include "LinkTransferGate.h"
#include "LinkTransferStepper.h"
#include "MaintenanceCompletionTracker.h"
#include "FirmwareUpdateSession.h"
#include "CapsuleTransaction.h"
#include "CapsuleBatchJournalStore.h"
#include "CapabilityRegistry.h"
#include "DeferredFileCleanup.h"
#include "StorageCoordinator.h"

namespace pokepod {

using LinkDeviceExerciseAction = bool (*)();

class BoardServices;
class AudioPipeline;
class AudioCaptureRuntime;
class AudioCaptureDispatcher;
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
class DeviceRebootCoordinator;
class RuntimeDiagnostics;

class PokePodLinkService : private LinkFileTransferHost {
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
             AudioCaptureDispatcher *captureDispatcher = nullptr,
             const CapabilityRegistry *capabilities = nullptr,
             DeviceRebootCoordinator *rebootCoordinator = nullptr,
             RuntimeDiagnostics *runtimeDiagnostics = nullptr,
             LinkDeviceExerciseAction wirelessVoiceStart = nullptr,
             LinkDeviceExerciseAction wirelessVoiceStop = nullptr);
  void poll(uint32_t nowMs);
  // Finishes read-only handle cleanup after an immediate transport cancel.
  // This never reads frames or writes responses, so a Wi-Fi service can call
  // it before authentication and while its five-minute window is closed.
  void pollDeferredCleanup();
  void disconnect();
  // Safe shutdown closes the transport immediately, then lets durable
  // rollback and owner-scoped handle cleanup advance cooperatively.  No new
  // frame is accepted after this point.
  void requestQuiesce();
  bool quiesced() const;
  bool active() const { return sessionActive_; }
  bool receivingBinary() const {
    return incomingKind_ != IncomingKind::none || firmwareUpdate_.active();
  }
  bool maintenanceActive() const { return !activeMaintenance_.isEmpty(); }
  uint32_t usbHostSessionGeneration() const {
    return usbHostSessionGeneration_;
  }
  bool deviceLifecycleRestartReady() const;
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
  enum class TransactionPurpose : uint8_t {
    none, startupRecovery, incoming, commandText, commandTextResult,
    commandFailureResult
  };
  enum class BatchPending : uint8_t {
    none,
    backupCapsule,
    backupTrash,
    backupProcessing,
    applyTree,
    applyMetadata,
    applyPath,
    rollbackTree,
    rollbackMetadata,
    finalizeFolder,
    persistResult,
    cleanupTree,
    cleanupArtifacts,
    metadataRead,
  };
  enum class BatchStart : uint8_t { notApplicable, started, rejected };
  enum class CommandLoadState : uint8_t { none, reading, dispatch };
  enum class BatchReadContinuation : uint8_t {
    none,
    importCapsule,
    importProcessing,
    metadataCapsule,
    metadataProcessing,
  };

  class StringByteSource final : public CapsuleTransactionByteSource {
   public:
    void bind(const String &value) { value_ = &value; }
    uint32_t length() const override {
      return value_ == nullptr ? 0 : value_->length();
    }
    size_t readAt(uint32_t offset, uint8_t *destination,
                  size_t maximumBytes) override {
      if (value_ == nullptr || destination == nullptr ||
          offset >= value_->length()) return 0;
      const size_t count = std::min(
          maximumBytes, static_cast<size_t>(value_->length() - offset));
      memcpy(destination, value_->c_str() + offset, count);
      return count;
    }
   private:
    const String *value_ = nullptr;
  };

  struct ManifestDirectoryCursor {
    File directory;
    String absolute;
    String relative;
    uint8_t depth = 0;
  };

  // LinkTransportSession.cpp owns connection generations, request admission,
  // frame parsing/transmit, terminal response draining and disconnect settlement.
  void consumeByte(uint8_t value, LinkPollPhaseGate *gate = nullptr);
  uint32_t activateConnectionGeneration();
  LinkOperationAdmission admitLinkOperation(uint32_t requestId);
  bool operationOwns(uint32_t requestId) const;
  void cancelLinkOperation(LinkOperationCancelReason reason);
  void advanceLinkOperationSettlement();
  bool recoverStalledLink(uint32_t nowMs);
  String linkProbeJson() const;
  void resetFrame();
  void processFrame(LinkPollPhaseGate *gate = nullptr);
  enum class MetadataReadResult : uint8_t { pending, ready, failed };
  void processRequest(uint32_t requestId, const uint8_t *payload, size_t size);
  void processData(uint32_t requestId, uint16_t flags,
                   const uint8_t *payload, size_t size);
  void finishIncoming();
  void advanceTransactionRunner();
  void advanceBatchStartupRecovery();
  bool startBatchStartupRecovery(const String &transactionId);
  void quarantineBatchJournal(const String &transactionId);
  void advanceStartupPartCleanup();
  void finishStartupRecovery(bool recovered);
  void finishIncomingTransaction(bool committed);
  void failIncoming(const char *message);
  bool cleanupIncomingStorage();
  void finishIncomingCleanup();
  void finishFirmwareUpdate(bool ok);
  void failFirmwareUpdate(const char *message);

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
  // LinkCapsuleCommands.cpp owns command loading, durable batch execution,
  // boot recovery and command-scoped cleanup. LinkOperation and storage-owner
  // authority deliberately remain members of this single service instance.
  void handleCommandFile(uint32_t requestId, const String &path,
                         const String &transactionId);
  bool beginCommandLoad(uint32_t requestId, const String &path,
                        const String &transactionId,
                        bool alreadyComplete = false);
  void advanceCommandLoad();
  void finishCommandLoad(bool keepCommand);
  void dispatchLoadedCommand();
  BatchStart tryStartBatchCommand(uint32_t requestId, void *jsonRoot,
                                  const String &path,
                                  const String &transactionId);
  bool tryStartTextCommand(uint32_t requestId, void *jsonRoot,
                           const String &path,
                           const String &transactionId);
  void finishTextCommand(bool committed);
  void finishTextResult(bool persisted);
  bool startCommandFailureResult(uint32_t requestId, const String &path,
                                 const String &transactionId,
                                 const char *message);
  void finishCommandFailureResult(bool persisted);
  void applyCompletedCommandSideEffects(void *jsonRoot);
  void applyDurableCommandSideEffects(const char *operation,
                                      const char *targetId);
  void advanceBatchCommand();
  void advanceBatchPending();
  bool startBatchWork(const LinkCommandExecutor::Work &work);
  bool startBatchPreflight(size_t index);
  bool startBatchApply(size_t index);
  bool startBatchRollback(size_t index);
  bool startBatchFinalize();
  bool startBatchRollbackFinalize();
  void finishBatchWork(bool ok);
  bool buildBatchPlan(size_t index, StoredCapsuleBatchPlan &plan,
                      String &message);
  bool rememberBatchId(const char *uuid);
  bool buildSimpleBatchPlan(void *jsonRoot, StoredCapsuleBatchPlan &plan,
                            String &message);
  bool startBatchMetadataCommit(const StoredCapsuleBatchPlan &plan,
                                bool rollback);
  bool buildBatchMetadata(const StoredCapsuleBatchPlan &plan,
                          String &value, String &message);
  bool startBatchResultPersistence();
  MetadataReadResult parseMetadataStep(const String &path, void *&root);
  void clearBatchReadContinuation();
  void applyBatchResultSideEffects();
  bool cleanupBatchArtifacts();
  void finishBatchCommand();
  void abandonBatchCommand();
  bool batchForegroundPermitted() const;
  String batchArtifactPath(size_t index, const char *suffix) const;
  String batchPurgePath(const StoredCapsuleBatchPlan &plan) const;
  String batchFolderStagingPath() const;
  void updateBatchJournalState(bool itemInFlight);

  bool sendOk(uint32_t requestId, const char *extraJson = nullptr);
  void sendBusy(uint32_t requestId, uint32_t retryAfterMs = 150);
  void sendError(uint32_t requestId, const char *message);
  bool sendJson(uint32_t requestId, const String &json);
  bool sendJson(uint32_t requestId, const String &json,
                bool completionEligible);
  bool sendJson(uint32_t requestId, const String &json,
                bool completionEligible, bool preserveForFallback);
  bool sendTerminalOrDisconnect(uint32_t requestId, const String &json,
                                bool completionEligible = true);
  bool sendEvent(uint32_t requestId, const String &json);
  bool sendFrame(LinkFrameType type, uint16_t flags, uint32_t requestId,
                 const uint8_t *payload, size_t size,
                 LinkOperationFrameRole role,
                 bool completionEligible = false,
                 bool preserveForFallback = false);
  bool queueFrame(LinkFrameType type, uint16_t flags, uint32_t requestId,
                  const uint8_t *payload, size_t size,
                  LinkFileTransferFrameCompletion completion,
                  LinkOperationFrameRole role,
                  bool completionEligible = false,
                  bool preserveForFallback = false);
  void advanceTransmit(uint32_t nowMs);
  // LinkFileTransferHost: transport and LinkOperation remain single-owner in
  // this service while LinkFileTransfer owns the file and read reservation.
  bool linkFileTransferPermitted() const override;
  bool linkFileTransmitIdle() const override;
  void linkFileCancelForDeadline(uint32_t requestId) override;
  void linkFileSendBusy(uint32_t requestId) override;
  void linkFileSendError(uint32_t requestId, const char *message) override;
  bool linkFileQueueFrame(
      LinkFrameType type, uint16_t flags, uint32_t requestId,
      const uint8_t *payload, size_t size,
      LinkFileTransferFrameCompletion completion,
      LinkOperationFrameRole role, bool completionEligible) override;
  void linkFileDisconnectTransport() override;
  void linkFileClaimResources(uint32_t requestId) override;
  void linkFileReleaseResources() override;
  void linkFileAdvanceSettlement() override;
  void linkFileCancelTransmitFrames() override;
  fs::FS *linkFileSystem() override;
  StorageOwner linkFileStorageOwner() const override;
  uint32_t linkFileStorageIoTimeout() const override;
  uint8_t *linkFilePayloadBuffer() override;
  size_t linkFilePayloadCapacity() const override;
  void linkFileResultFetched(const char *transactionId,
                             bool fullySent) override;

  void handleLinkRecordingEvent(const LinkRecordingEvent &event);
  bool pollDeferredCleanup(LinkPollPhaseGate &gate);

  bool beginIncoming(IncomingKind kind, uint32_t requestId,
                     uint32_t expectedBytes, const String &temporaryPath,
                     const String &finalPath, const String &transactionId,
                     bool chunkAcks);
  bool ensureDirectoryTree(const String &path);
  bool writeTextAtomic(const String &path, const String &text);
  bool validFontFile(const String &path) const;
  MetadataReadResult readMetadataStep(const String &path, size_t limit,
                                      const uint8_t *&data, size_t &bytes);
  void resetMetadataRead();
  String deviceId() const;
  bool foregroundBusy() const;
  bool safeFolder(const char *value, bool allowBuiltIn = true) const;
  String folderDirectory(const char *value) const;
  String activeCapsuleDirectory(const String &id) const;
  bool cleanupPurgeStaging();
  void queueDeferredTreeCleanup(const String &path);
  bool stepDeferredTreeCleanup();
  bool storageExists(const String &path, StorageAccess access) const;
  bool storageRename(const String &source, const String &target);
  bool storageRemove(const String &path);
  bool storageMkdir(const String &path);
  bool storageRmdir(const String &path);
  void closeStorageFile(File &file, StorageAccess access) const;
  bool finishCopiedFiles(File &input, File &output) const;
  void deferStorageFile(File &file, StorageAccess access,
                        bool flushBeforeClose = false) const;
  bool stepDeferredFileCleanup();
  void finishCommandStorageCleanup();
  String newUuid() const;
  bool transferPermitted() const;
  uint32_t storageIoTimeout() const;
  StorageOwner storageOwner() const;

  Stream *stream_ = nullptr;
  fs::FS *fs_ = nullptr;
  BoardServices *board_ = nullptr;
  AudioPipeline *audio_ = nullptr;
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
  ProvisioningCoordinator *provisioningCoordinator_ = nullptr;
  Print *log_ = nullptr;
  LinkServiceCoordinator *coordinator_ = nullptr;
  DeviceRebootCoordinator *rebootCoordinator_ = nullptr;
  RuntimeDiagnostics *runtimeDiagnostics_ = nullptr;
  LinkDeviceExerciseAction wirelessVoiceStart_ = nullptr;
  LinkDeviceExerciseAction wirelessVoiceStop_ = nullptr;
  LinkTransport transport_ = LinkTransport::none;
  WirelessSyncPairingProvider *pairingProvider_ = nullptr;
  LinkTransferGate *transferGate_ = nullptr;
  LinkWriteChannel *writeChannel_ = nullptr;
  LinkOperation operation_;
  LinkLivenessProbe liveness_;
  uint32_t connectionGeneration_ = 0;
  uint32_t nextConnectionGeneration_ = 0;
  uint32_t usbHostSessionGeneration_ = 0;
  CapsuleTransaction transaction_;
  CapsuleTransactionRunner transactionRunner_;
  LinkCapsuleTransactionGate transactionGate_;
  TransactionPurpose transactionPurpose_ = TransactionPurpose::none;
  IncomingKind transactionIncomingKind_ = IncomingKind::none;
  uint32_t transactionRequestId_ = 0;
  String transactionPreparedPath_;
  String transactionFinalPath_;
  String transactionId_;
  bool transactionRespond_ = false;
  String commandTextPath_;
  String commandTextTransactionId_;
  String commandTextStagingDirectory_;
  String commandTextValue_;
  String commandTextMetadataValue_;
  StringByteSource commandTextSource_;
  StringByteSource commandTextMetadataSource_;
  uint32_t commandTextRequestId_ = 0;
  bool commandTextRespond_ = false;
  StorageReservation startupPartCleanupReservation_;
  uint8_t startupPartCleanupIndex_ = 0;
  uint8_t startupPartCleanupFailures_ = 0;
  bool startupPartCleanupPending_ = false;
  File startupPurgeDirectory_;
  StorageReservation startupPurgeReservation_;
  bool startupPurgePending_ = false;
  bool startupBatchRecoveryPending_ = false;
  File startupBatchDirectory_;
  StorageReservation startupBatchReservation_;
  String startupBatchCandidate_;
  uint8_t startupBatchCandidateFailures_ = 0;
  bool startupReady_ = false;
  bool startupRecoveryFailed_ = false;
  bool startupBatchCandidateInvalid_ = false;
  bool mutationRecoveryBlocked_ = false;
  StorageReservation incomingStorageReservation_;
  DeferredFileCleanup incomingCleanup_;
  bool incomingCleanupPending_ = false;
  bool incomingCleanupRespond_ = false;
  uint32_t incomingCleanupRequestId_ = 0;
  String incomingCleanupMessage_;
  bool commandStorageActive_ = false;
  StorageReservation commandStorageReservation_;
  CommandLoadState commandLoadState_ = CommandLoadState::none;
  File commandLoadFile_;
  String commandLoadValue_;
  String commandLoadPath_;
  String commandLoadTransactionId_;
  uint32_t commandLoadRequestId_ = 0;
  uint32_t commandLoadExpected_ = 0;
  bool commandLoadRespond_ = false;
  bool commandLoadAlreadyComplete_ = false;
  bool commandCleanupPending_ = false;
  uint32_t commandCleanupRequestId_ = 0;
  LinkCommandExecutor batchExecutor_;
  CapsuleBatchJournalStore batchJournalStore_;
  LinkTreeStepper batchTreeStepper_;
  StoredCapsuleBatchState batchJournalState_{};
  StoredCapsuleBatchPlan batchPlan_{};
  void *batchJsonRoot_ = nullptr;
  LinkCommandExecutor::Work batchWork_{};
  BatchPending batchPending_ = BatchPending::none;
  uint8_t batchPendingStep_ = 0;
  BatchReadContinuation batchReadContinuation_ =
      BatchReadContinuation::none;
  void *batchReadJson_ = nullptr;
  String batchMetadataFirstPendingValue_;
  size_t batchCleanupIndex_ = 0;
  uint32_t batchRequestId_ = 0;
  String batchCommandPath_;
  String batchTransactionId_;
  String batchMetadataValue_;
  String batchMetadataSecondValue_;
  String batchResultValue_;
  String batchMessage_;
  void *batchNextIdItem_ = nullptr;
  size_t batchNextIdIndex_ = 0;
  uint8_t *batchSeenIds_ = nullptr;
  StringByteSource batchByteSource_;
  StringByteSource batchSecondByteSource_;
  LinkRecordingSession recordingSession_;
  FirmwareUpdateSession firmwareUpdate_;
  uint32_t firmwareUpdateRequestId_ = 0;
  LinkDiagnostics diagnostics_;
  LinkFileTransfer fileTransfer_;

  ReceivePhase receivePhase_ = ReceivePhase::magic;
  uint8_t headerBytes_[kLinkHeaderBytes] = {};
  size_t headerUsed_ = 0;
  LinkFrameHeader currentHeader_;
  // Link v2 needs a full data-frame buffer. There are two service instances
  // (USB and Wi-Fi); keeping both 16 KiB buffers in internal DRAM starves the
  // Wi-Fi driver of large contiguous blocks when the provisioning AP starts.
  // The board has mandatory PSRAM, so allocate these long-lived buffers there.
  uint8_t *payload_ = nullptr;
  uint8_t *metadataReadBuffer_ = nullptr;
  LinkBoundedTextRead metadataRead_;
  File metadataReadFile_;
  String metadataReadPath_;
  uint8_t *txFrame_ = nullptr;
  uint8_t *pendingControlFrame_ = nullptr;
  size_t payloadUsed_ = 0;
  int16_t deferredRxByte_ = -1;
  uint8_t magicMatched_ = 0;
  bool frameProcessedThisPoll_ = false;
  bool sessionActive_ = false;
  bool quiesceRequested_ = false;
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
  String activeMaintenance_;
  MaintenanceCompletionTracker maintenanceCompletion_;

  LinkTransferStepper txStepper_;
  size_t txFrameBytes_ = 0;
  LinkFileTransferFrameCompletion txCompletion_ =
      LinkFileTransferFrameCompletion::none;
  LinkOperationFrameRole txFrameRole_ = LinkOperationFrameRole::progress;
  uint32_t txFrameGeneration_ = 0;
  size_t pendingControlBytes_ = 0;
  LinkFileTransferFrameCompletion pendingControlCompletion_ =
      LinkFileTransferFrameCompletion::none;
  LinkOperationFrameRole pendingControlRole_ =
      LinkOperationFrameRole::progress;
  uint32_t pendingControlGeneration_ = 0;

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
  struct DeferredCommandFile {
    File file;
    StorageOwner owner = StorageOwner::none;
    StorageAccess access = StorageAccess::read;
    bool flushBeforeClose = false;
  };
  mutable std::vector<DeferredCommandFile> deferredCommandFiles_;
  bool deferredCommandFileFailed_ = false;
  std::vector<String> deferredTreeCleanupStack_;
  uint8_t deferredTreeCleanupFailures_ = 0;
  bool deferredTreeCleanupBlocked_ = false;
};

}  // namespace pokepod
