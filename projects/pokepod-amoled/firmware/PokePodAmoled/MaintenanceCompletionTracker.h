#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace pokepod {

class MaintenanceCompletionTracker {
 public:
  void beginAccepted() {
    clearPendingEnd();
    ++startRevision_;
  }

  void endResultPersisted(const char *transactionId) {
    storePendingEnd(transactionId);
  }

  bool resultFetched(const char *transactionId, bool fullySent) {
    if (!fullySent || transactionId == nullptr ||
        pendingEndTransactionId_[0] == '\0' ||
        strcmp(pendingEndTransactionId_, transactionId) != 0) {
      return false;
    }
    clearPendingEnd();
    completedStartRevision_ = startRevision_;
    ++completionRevision_;
    return true;
  }

  void disconnect() { clearPendingEnd(); }

  bool pendingEnd() const { return pendingEndTransactionId_[0] != '\0'; }
  uint32_t startRevision() const { return startRevision_; }
  uint32_t completionRevision() const { return completionRevision_; }
  uint32_t completedStartRevision() const {
    return completedStartRevision_;
  }

 private:
  static constexpr size_t kTransactionIdBytes = 37;

  void clearPendingEnd() { pendingEndTransactionId_[0] = '\0'; }

  void storePendingEnd(const char *transactionId) {
    if (transactionId == nullptr) {
      clearPendingEnd();
      return;
    }
    strncpy(pendingEndTransactionId_, transactionId,
            kTransactionIdBytes - 1);
    pendingEndTransactionId_[kTransactionIdBytes - 1] = '\0';
  }

  char pendingEndTransactionId_[kTransactionIdBytes] = {};
  uint32_t startRevision_ = 0;
  uint32_t completionRevision_ = 0;
  uint32_t completedStartRevision_ = 0;
};

}  // namespace pokepod
