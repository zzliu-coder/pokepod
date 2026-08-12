#include "LinkFileTransfer.h"

#include <algorithm>
#include <utility>

namespace pokepod {

bool LinkFileTransfer::send(uint32_t requestId, const String &path,
                            const char *resultTransactionId) {
  if (host_ == nullptr || host_->linkFileSystem() == nullptr) {
    return false;
  }
  if (!host_->linkFileTransferPermitted()) {
    host_->linkFileCancelForDeadline(requestId);
    return false;
  }
  if (active() || !host_->linkFileTransmitIdle()) {
    host_->linkFileSendBusy(requestId);
    return false;
  }

  StorageReservation reservation = StorageCoordinator::instance().reserve(
      host_->linkFileStorageOwner(), StorageAccess::read,
      host_->linkFileStorageIoTimeout());
  if (!reservation) {
    host_->linkFileSendBusy(requestId);
    return false;
  }

  File file;
  size_t length = 0;
  {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        host_->linkFileStorageOwner(), StorageAccess::read,
        host_->linkFileStorageIoTimeout());
    if (!lease) {
      host_->linkFileSendBusy(requestId);
      return false;
    }
    file = host_->linkFileSystem()->open(path, FILE_READ);
    if (!file || file.isDirectory()) {
      if (file) file.close();
      host_->linkFileSendError(requestId, "file is missing");
      return false;
    }
    length = file.size();
  }

  const String response =
      "{\"status\":\"ok\",\"version\":2,\"available\":true,"
      "\"binaryLength\":" + String(length) + "}";
  phase_ = Phase::response;
  requestId_ = requestId;
  length_ = length;
  read_ = 0;
  file_ = file;
  resultTransactionId_ = resultTransactionId == nullptr
      ? String() : String(resultTransactionId);
  reservation_ = std::move(reservation);
  host_->linkFileClaimResources(requestId);

  if (!host_->linkFileQueueFrame(
          LinkFrameType::responseJson, 0, requestId,
          reinterpret_cast<const uint8_t *>(response.c_str()),
          response.length(), LinkFileTransferFrameCompletion::response,
          length == 0 ? LinkOperationFrameRole::terminal
                      : LinkOperationFrameRole::data,
          length == 0)) {
    finish(false);
    return false;
  }
  return true;
}

void LinkFileTransfer::advance() {
  if (host_ == nullptr || phase_ != Phase::data || cleanupPending_ ||
      !file_ || !host_->linkFileTransmitIdle()) {
    return;
  }
  if (!host_->linkFileTransferPermitted()) {
    host_->linkFileDisconnectTransport();
    return;
  }
  if (read_ >= length_) {
    finish(true);
    return;
  }

  const size_t capacity = host_->linkFilePayloadCapacity();
  uint8_t *payload = host_->linkFilePayloadBuffer();
  if (payload == nullptr || capacity == 0) {
    finish(false);
    host_->linkFileDisconnectTransport();
    return;
  }

  int received = 0;
  {
    // Never wait behind another SD owner while a network deadline is running.
    // The logical read reservation remains held and a later poll retries.
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        host_->linkFileStorageOwner(), StorageAccess::read, 0);
    if (!lease) return;
    received = static_cast<int>(file_.read(
        payload, std::min(capacity, length_ - read_)));
  }
  if (received <= 0) {
    finish(false);
    host_->linkFileDisconnectTransport();
    return;
  }

  const size_t count = static_cast<size_t>(received);
  read_ += count;
  const bool final = read_ == length_;
  if (!host_->linkFileQueueFrame(
          LinkFrameType::data, final ? 1 : 0, requestId_, payload, count,
          final ? LinkFileTransferFrameCompletion::final
                : LinkFileTransferFrameCompletion::data,
          final ? LinkOperationFrameRole::terminal
                : LinkOperationFrameRole::data,
          final)) {
    finish(false);
    host_->linkFileDisconnectTransport();
  }
}

void LinkFileTransfer::finish(bool success) {
  if (!cleanupPending_) {
    cleanupSuccess_ = success;
    if (!cleanup_.begin(file_, nullptr, "", reservation_,
                        host_ == nullptr ? StorageOwner::none
                                         : host_->linkFileStorageOwner(),
                        StorageAccess::read)) {
      return;
    }
    cleanupPending_ = true;
  }
  (void)pollCleanup();
}

bool LinkFileTransfer::pollCleanup() {
  if (!cleanupPending_) return true;
  if (!cleanup_.poll()) return false;
  cleanupPending_ = false;
  finishCleanup();
  return true;
}

void LinkFileTransfer::finishCleanup() {
  const bool success = cleanupSuccess_;
  const String resultTransaction = resultTransactionId_;
  phase_ = Phase::none;
  requestId_ = 0;
  length_ = 0;
  read_ = 0;
  resultTransactionId_ = "";
  cleanupSuccess_ = false;
  if (host_ == nullptr) return;
  host_->linkFileReleaseResources();
  if (success && !resultTransaction.isEmpty()) {
    host_->linkFileResultFetched(resultTransaction.c_str(), true);
  }
  host_->linkFileAdvanceSettlement();
}

void LinkFileTransfer::abort() {
  if (host_ != nullptr) host_->linkFileCancelTransmitFrames();
  if (cleanupPending_) {
    // A socket cancellation before owner-scoped cleanup completes is not a
    // confirmed result fetch even if the final frame left the TX buffer.
    cleanupSuccess_ = false;
    (void)pollCleanup();
  } else if (phase_ != Phase::none || file_) {
    finish(false);
  } else {
    reservation_.release();
    resultTransactionId_ = "";
    if (host_ != nullptr) host_->linkFileReleaseResources();
  }
}

void LinkFileTransfer::onFrameSent(
    LinkFileTransferFrameCompletion completion) {
  if (completion == LinkFileTransferFrameCompletion::response) {
    if (length_ == 0) {
      finish(true);
    } else {
      phase_ = Phase::data;
    }
  } else if (completion == LinkFileTransferFrameCompletion::final) {
    finish(true);
  }
}

}  // namespace pokepod
