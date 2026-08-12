#pragma once

#include <Arduino.h>
#include <FS.h>
#include <stdint.h>

#include "DeferredFileCleanup.h"
#include "LinkFrame.h"
#include "LinkOperation.h"
#include "StorageCoordinator.h"

namespace pokepod {

enum class LinkFileTransferFrameCompletion : uint8_t {
  none = 0,
  response,
  data,
  final,
};

// Narrow bridge back to the Link transport.  File transfer owns the File,
// reservation, byte counters and cleanup state; the host remains the sole
// owner of LinkOperation, transport deadlines and encoded TX buffers.
class LinkFileTransferHost {
 public:
  virtual ~LinkFileTransferHost() = default;

  virtual bool linkFileTransferPermitted() const = 0;
  virtual bool linkFileTransmitIdle() const = 0;
  virtual void linkFileCancelForDeadline(uint32_t requestId) = 0;
  virtual void linkFileSendBusy(uint32_t requestId) = 0;
  virtual void linkFileSendError(uint32_t requestId,
                                 const char *message) = 0;
  virtual bool linkFileQueueFrame(
      LinkFrameType type, uint16_t flags, uint32_t requestId,
      const uint8_t *payload, size_t size,
      LinkFileTransferFrameCompletion completion,
      LinkOperationFrameRole role, bool completionEligible) = 0;
  virtual void linkFileDisconnectTransport() = 0;
  virtual void linkFileClaimResources(uint32_t requestId) = 0;
  virtual void linkFileReleaseResources() = 0;
  virtual void linkFileAdvanceSettlement() = 0;
  virtual void linkFileCancelTransmitFrames() = 0;
  virtual fs::FS *linkFileSystem() = 0;
  virtual StorageOwner linkFileStorageOwner() const = 0;
  virtual uint32_t linkFileStorageIoTimeout() const = 0;
  virtual uint8_t *linkFilePayloadBuffer() = 0;
  virtual size_t linkFilePayloadCapacity() const = 0;
  virtual void linkFileResultFetched(const char *transactionId,
                                     bool fullySent) = 0;
};

// Cooperatively streams one file without sharing its File or reservation with
// the transport service.  A final Link frame is not treated as fetched until
// owner-scoped physical cleanup closes the handle and releases the reservation.
class LinkFileTransfer {
 public:
  void bind(LinkFileTransferHost &host) { host_ = &host; }

  bool send(uint32_t requestId, const String &path,
            const char *resultTransactionId = nullptr);
  void advance();
  bool pollCleanup();
  void abort();
  void onFrameSent(LinkFileTransferFrameCompletion completion);

  bool active() const { return phase_ != Phase::none || cleanupPending_; }
  bool cleanupPending() const { return cleanupPending_; }
  bool dataReady() const { return phase_ == Phase::data; }
  bool quiesced() const {
    return phase_ == Phase::none && !cleanupPending_ && !file_ &&
        !reservation_;
  }

 private:
  enum class Phase : uint8_t { none = 0, response, data };

  void finish(bool success);
  void finishCleanup();

  LinkFileTransferHost *host_ = nullptr;
  Phase phase_ = Phase::none;
  uint32_t requestId_ = 0;
  size_t length_ = 0;
  size_t read_ = 0;
  File file_;
  String resultTransactionId_;
  StorageReservation reservation_;
  DeferredFileCleanup cleanup_;
  bool cleanupPending_ = false;
  bool cleanupSuccess_ = false;
};

}  // namespace pokepod
