#pragma once

#include <Arduino.h>
#include <FS.h>
#include <vector>

#include "LinkFrame.h"
#include "LinkPolicy.h"

namespace pokepod {

class BoardServices;
class AudioPipeline;
class CapsuleLibrary;
class DeviceConfig;
class TencentWorker;
class UsbVoiceBridge;
class WavRecorder;
class WifiController;

class PokePodLinkService {
 public:
  bool begin(Stream &stream, fs::FS &fs, BoardServices &board,
             AudioPipeline &audio,
             UsbVoiceBridge &usb, CapsuleLibrary &library,
             WavRecorder &recorder, DeviceConfig &config,
             WifiController &wifi, TencentWorker &tencent, Print &log);
  void poll(uint32_t nowMs);
  bool active() const { return sessionActive_; }

 private:
  enum class ReceivePhase : uint8_t { magic, header, payload };
  enum class IncomingKind : uint8_t { none, stagedFile, command, systemFont };

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
  void handleConfigure(uint32_t requestId, void *jsonRoot);
  void handleCommandFile(uint32_t requestId, const String &path,
                         const String &transactionId);
  bool executeCommand(const String &path, const String &transactionId,
                      String &message);

  void sendOk(uint32_t requestId, const char *extraJson = nullptr);
  void sendBusy(uint32_t requestId, uint32_t retryAfterMs = 150);
  void sendError(uint32_t requestId, const char *message);
  bool sendJson(uint32_t requestId, const String &json);
  bool sendFile(uint32_t requestId, const String &path);
  bool sendFrame(LinkFrameType type, uint16_t flags, uint32_t requestId,
                 const uint8_t *payload, size_t size);
  void rememberCompleted(uint32_t requestId);

  bool beginIncoming(IncomingKind kind, uint32_t requestId,
                     uint32_t expectedBytes, const String &temporaryPath,
                     const String &finalPath, const String &transactionId);
  bool ensureDirectoryTree(const String &path);
  bool writeTextAtomic(const String &path, const String &text);
  bool validFontFile(const String &path) const;
  String readText(const String &path, size_t limit) const;
  bool collectFiles(const String &directory, const String &relative,
                    uint8_t depth, std::vector<String> &files) const;
  String metadataFingerprint() const;
  String deviceId() const;
  bool foregroundBusy() const;
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
  bool removeTree(const String &path);
  bool copyTree(const String &source, const String &target, uint8_t depth = 0);
  bool rewriteCopiedMetadata(const String &directory, const String &id);
  String newUuid() const;

  Stream *stream_ = nullptr;
  fs::FS *fs_ = nullptr;
  BoardServices *board_ = nullptr;
  AudioPipeline *audio_ = nullptr;
  UsbVoiceBridge *usb_ = nullptr;
  CapsuleLibrary *library_ = nullptr;
  WavRecorder *recorder_ = nullptr;
  DeviceConfig *config_ = nullptr;
  WifiController *wifi_ = nullptr;
  TencentWorker *tencent_ = nullptr;
  Print *log_ = nullptr;

  ReceivePhase receivePhase_ = ReceivePhase::magic;
  uint8_t headerBytes_[kLinkHeaderBytes] = {};
  size_t headerUsed_ = 0;
  LinkFrameHeader currentHeader_;
  uint8_t payload_[kLinkMaxDataBytes] = {};
  size_t payloadUsed_ = 0;
  uint8_t magicMatched_ = 0;
  bool sessionActive_ = false;
  LinkRequestHistory completed_;

  IncomingKind incomingKind_ = IncomingKind::none;
  uint32_t incomingRequestId_ = 0;
  uint32_t incomingExpected_ = 0;
  uint32_t incomingReceived_ = 0;
  File incomingFile_;
  String incomingTemporaryPath_;
  String incomingFinalPath_;
  String incomingTransactionId_;
  uint32_t incomingLastByteMs_ = 0;
  uint32_t rebootAtMs_ = 0;
};

}  // namespace pokepod
