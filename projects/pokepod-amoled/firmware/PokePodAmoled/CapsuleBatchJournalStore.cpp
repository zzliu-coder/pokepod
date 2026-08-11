#include "CapsuleBatchJournalStore.h"

namespace pokepod {

String CapsuleBatchJournalStore::path(const String &transactionId) const {
  return capsuleBatchUuid(transactionId.c_str())
      ? String(kDirectory) + "/" + transactionId + ".cbj" : String();
}

String CapsuleBatchJournalStore::statePath(
    const String &transactionId, size_t slot) const {
  const String base = path(transactionId);
  return base.isEmpty() ? String() : base + (slot == 0 ? ".a" : ".b");
}

bool CapsuleBatchJournalStore::create(
    const String &transactionId, const char *operation, uint16_t total,
    StorageOwner owner, StoredCapsuleBatchState &state,
    const char *finalizeFolder) {
  if (fs_ == nullptr || !capsuleBatchUuid(transactionId.c_str()) ||
      !capsuleBatchOperation(operation) ||
      strlen(operation) >= sizeof(state.operation) || total > 500 ||
      (strcmp(operation, "deleteFolderToInbox") == 0
           ? !capsuleBatchFolderPath(finalizeFolder)
           : finalizeFolder != nullptr && finalizeFolder[0] != '\0')) {
    return false;
  }
  memset(&state, 0, sizeof(state));
  state.magic = kCapsuleBatchJournalMagic;
  state.version = kCapsuleBatchJournalVersion;
  state.bytes = sizeof(state);
  strlcpy(state.transactionId, transactionId.c_str(),
          sizeof(state.transactionId));
  strlcpy(state.operation, operation, sizeof(state.operation));
  if (finalizeFolder != nullptr) {
    strlcpy(state.finalizeFolder, finalizeFolder,
            sizeof(state.finalizeFolder));
  }
  state.total = total;
  state.phase = CapsuleBatchPhase::preflight;
  state.flags = capsuleBatchResponseAllowed;
  state.generation = 1;
  sealCapsuleBatchState(state);

  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      owner, StorageAccess::mutation, 0);
  if (!lease) return false;
  if (!fs_->exists(kDirectory) && !fs_->mkdir(kDirectory)) return false;
  // The plan marker is created before state.  Startup scans only this marker,
  // so a cut here can never expose an authoritative state without a recovery
  // entry.  State slots are separate files: a failed close of the inactive
  // slot cannot damage the only durable slot.
  File marker = fs_->open(path(transactionId), "w");
  if (!marker) return false;
  marker.flush();
  const bool markerOk = marker.getWriteError() == 0;
  marker.close();
  if (!markerOk || marker.getWriteError() != 0) return false;
  lease.release();
  if (!writeStateSlot(statePath(transactionId, 1), state, owner)) return false;
  StoredCapsuleBatchState verified;
  return readStateSlot(statePath(transactionId, 1), verified, owner) &&
      memcmp(&verified, &state, sizeof(state)) == 0;
}

bool CapsuleBatchJournalStore::load(
    const String &transactionId, StorageOwner owner,
    StoredCapsuleBatchState &state) const {
  return loadStatus(transactionId, owner, state) == LoadResult::loaded;
}

CapsuleBatchJournalStore::LoadResult CapsuleBatchJournalStore::loadStatus(
    const String &transactionId, StorageOwner owner,
    StoredCapsuleBatchState &state) const {
  if (fs_ == nullptr || !capsuleBatchUuid(transactionId.c_str())) {
    return LoadResult::invalid;
  }
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      owner, StorageAccess::read, 0);
  if (!lease) return LoadResult::wouldBlock;
  StoredCapsuleBatchState slots[2] = {};
  for (size_t slot = 0; slot < 2; ++slot) {
    File file = fs_->open(statePath(transactionId, slot), FILE_READ);
    if (!file) continue;
    const bool read = file.read(reinterpret_cast<uint8_t *>(&slots[slot]),
                                sizeof(slots[slot])) == sizeof(slots[slot]);
    file.close();
    if (!read || !validCapsuleBatchState(slots[slot])) slots[slot] = {};
  }
  const StoredCapsuleBatchState *newest = newestCapsuleBatchState(
      slots[0], slots[1]);
  if (newest == nullptr || transactionId != newest->transactionId) {
    return LoadResult::invalid;
  }
  state = *newest;
  return LoadResult::loaded;
}

bool CapsuleBatchJournalStore::checkpoint(StoredCapsuleBatchState &state,
                                          StorageOwner owner) {
  if (!validCapsuleBatchState(state)) return false;
  const StoredCapsuleBatchState candidate = nextCapsuleBatchCheckpoint(state);
  const size_t slot = candidate.generation & 1U;
  const String slotPath = statePath(candidate.transactionId, slot);
  if (!writeStateSlot(slotPath, candidate, owner)) return false;
  StoredCapsuleBatchState verified;
  if (readStateSlot(slotPath, verified, owner) &&
      acceptCapsuleBatchCheckpoint(state, candidate, verified)) {
    return true;
  }
  // The candidate slot is inactive until verified. Remove a readable but
  // corrupt copy so a later boot can never prefer a generation rejected by
  // the live writer; the previous slot remains untouched.
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      owner, StorageAccess::mutation, 0);
  if (lease && fs_->exists(slotPath)) (void)fs_->remove(slotPath);
  return false;
}

bool CapsuleBatchJournalStore::writeStateSlot(
    const String &journalPath,
    const StoredCapsuleBatchState &state, StorageOwner owner) const {
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      owner, StorageAccess::mutation, 0);
  if (!lease) return false;
  File file = fs_->open(journalPath, "w");
  if (!file) return false;
  const bool wrote = file.write(
      reinterpret_cast<const uint8_t *>(&state), sizeof(state)) ==
      sizeof(state);
  file.flush();
  const bool flushed = wrote && file.getWriteError() == 0;
  file.close();
  const bool closed = file.getWriteError() == 0;
  if (!flushed || !closed) {
    // This is always the inactive slot.  Best-effort removal prevents a
    // close that persisted a buffer after a failed flush from looking like a
    // checkpoint the live state never accepted; the other slot is untouched.
    (void)fs_->remove(journalPath);
    return false;
  }
  return true;
}

bool CapsuleBatchJournalStore::readStateSlot(
    const String &journalPath, StoredCapsuleBatchState &state,
    StorageOwner owner) const {
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      owner, StorageAccess::read, 0);
  if (!lease) return false;
  File file = fs_->open(journalPath, FILE_READ);
  if (!file) {
    if (file) file.close();
    return false;
  }
  const bool read = file.read(reinterpret_cast<uint8_t *>(&state),
                              sizeof(state)) == sizeof(state);
  file.close();
  return read && validCapsuleBatchState(state);
}

bool CapsuleBatchJournalStore::writePlan(
    const StoredCapsuleBatchState &state, uint16_t index,
    StoredCapsuleBatchPlan &plan, StorageOwner owner) {
  if (fs_ == nullptr || index >= state.total) return false;
  sealCapsuleBatchPlan(plan);
  if (!validCapsuleBatchState(state) ||
      !validCapsuleBatchPlan(plan, state.operation)) return false;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      owner, StorageAccess::mutation, 0);
  if (!lease) return false;
  File file = fs_->open(path(state.transactionId), "r+");
  if (!file || !file.seek(planOffset(index))) {
    if (file) file.close();
    return false;
  }
  const bool wrote = file.write(
      reinterpret_cast<const uint8_t *>(&plan), sizeof(plan)) == sizeof(plan);
  file.flush();
  const bool flushed = wrote && file.getWriteError() == 0;
  file.close();
  if (!flushed || file.getWriteError() != 0) return false;
  lease.release();
  StoredCapsuleBatchPlan verified;
  return readPlan(state, index, verified, owner) &&
      memcmp(&verified, &plan, sizeof(plan)) == 0;
}

bool CapsuleBatchJournalStore::readPlan(
    const StoredCapsuleBatchState &state, uint16_t index,
    StoredCapsuleBatchPlan &plan, StorageOwner owner) const {
  if (fs_ == nullptr || index >= state.total) return false;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      owner, StorageAccess::read, 0);
  if (!lease) return false;
  File file = fs_->open(path(state.transactionId), FILE_READ);
  if (!file || !file.seek(planOffset(index))) {
    if (file) file.close();
    return false;
  }
  const bool read = file.read(reinterpret_cast<uint8_t *>(&plan),
                              sizeof(plan)) == sizeof(plan);
  file.close();
  return read && validCapsuleBatchPlan(plan, state.operation);
}

bool CapsuleBatchJournalStore::erase(const String &transactionId,
                                     StorageOwner owner) {
  if (fs_ == nullptr || !capsuleBatchUuid(transactionId.c_str())) return false;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      owner, StorageAccess::mutation, 0);
  if (!lease) return false;
  const String first = statePath(transactionId, 0);
  const String second = statePath(transactionId, 1);
  const String marker = path(transactionId);
  // The marker is removed last.  Until then startup can still discover and
  // resume a partially cleaned transaction.
  return (!fs_->exists(first) || fs_->remove(first)) &&
      (!fs_->exists(second) || fs_->remove(second)) &&
      (!fs_->exists(marker) || fs_->remove(marker));
}

bool CapsuleBatchJournalStore::quarantine(
    const String &transactionId, const String &suffix, StorageOwner owner) {
  if (fs_ == nullptr || !capsuleBatchUuid(transactionId.c_str()) ||
      suffix.isEmpty() || suffix.indexOf('/') >= 0) return false;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      owner, StorageAccess::mutation, 0);
  if (!lease) return false;
  const String marker = path(transactionId);
  const String first = statePath(transactionId, 0);
  const String second = statePath(transactionId, 1);
  const bool firstOk = !fs_->exists(first) || fs_->rename(first, first + suffix);
  const bool secondOk = !fs_->exists(second) ||
      fs_->rename(second, second + suffix);
  const bool markerOk = !fs_->exists(marker) ||
      fs_->rename(marker, marker + suffix);
  return firstOk && secondOk && markerOk;
}

}  // namespace pokepod
