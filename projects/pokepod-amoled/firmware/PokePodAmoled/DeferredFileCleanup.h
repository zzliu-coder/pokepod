#pragma once

#include <Arduino.h>
#include <FS.h>

#include "StorageCoordinator.h"

namespace pokepod {

// Closes a filesystem handle, optionally removes its temporary file, and only
// then releases the logical storage reservation that protected the operation.
// A failed physical IO acquisition leaves every resource owned so a later poll
// can retry without exposing a half-cleaned session to another storage owner.
class DeferredFileCleanup {
 public:
  bool begin(File &file, fs::FS *fs, const String &removePath,
             StorageReservation &reservation, StorageOwner owner,
             StorageAccess access) {
    if (pending_) return false;
    file_ = &file;
    fs_ = fs;
    removePath_ = removePath;
    reservation_ = &reservation;
    owner_ = owner;
    access_ = access;
    pending_ = true;
    return true;
  }

  bool poll() {
    if (!pending_) return true;
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        owner_, access_, 0);
    if (!lease) return false;

    if (file_ != nullptr && *file_) file_->close();
    if (fs_ != nullptr && !removePath_.isEmpty() && fs_->exists(removePath_) &&
        !fs_->remove(removePath_)) {
      return false;
    }

    lease.release();
    if (reservation_ != nullptr) reservation_->release();
    file_ = nullptr;
    fs_ = nullptr;
    removePath_ = "";
    reservation_ = nullptr;
    owner_ = StorageOwner::none;
    access_ = StorageAccess::read;
    pending_ = false;
    return true;
  }

  bool pending() const { return pending_; }

 private:
  File *file_ = nullptr;
  fs::FS *fs_ = nullptr;
  String removePath_;
  StorageReservation *reservation_ = nullptr;
  StorageOwner owner_ = StorageOwner::none;
  StorageAccess access_ = StorageAccess::read;
  bool pending_ = false;
};

}  // namespace pokepod
