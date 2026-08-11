#include "CapsuleBatchJournalStore.h"

namespace pokepod {

String CapsuleBatchJournalStore::path(const String &transactionId) const {
  return String(kDirectory) + "/" + transactionId + ".cbj";
}

bool CapsuleBatchJournalStore::create(
    const String &transactionId, const char *operation, uint16_t total,
    StorageOwner owner, StoredCapsuleBatchState &state) {
  if (fs_ == nullptr || transactionId.length() != 36 || operation == nullptr ||
      strlen(operation) >= sizeof(state.operation) || total > 500) return false;
  memset(&state, 0, sizeof(state));
  state.magic = kCapsuleBatchJournalMagic;
  state.version = kCapsuleBatchJournalVersion;
  state.bytes = sizeof(state);
  strlcpy(state.transactionId, transactionId.c_str(),
          sizeof(state.transactionId));
  strlcpy(state.operation, operation, sizeof(state.operation));
  state.total = total;
  state.phase = CapsuleBatchPhase::preflight;
  state.flags = capsuleBatchResponseAllowed;
  state.generation = 1;
  sealCapsuleBatchState(state);

  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      owner, StorageAccess::mutation, 0);
  if (!lease) return false;
  if (!fs_->exists(kDirectory) && !fs_->mkdir(kDirectory)) return false;
  File file = fs_->open(path(transactionId), "w");
  if (!file) return false;
  const size_t first = file.write(
      reinterpret_cast<const uint8_t *>(&state), sizeof(state));
  StoredCapsuleBatchState empty = {};
  const size_t second = file.write(
      reinterpret_cast<const uint8_t *>(&empty), sizeof(empty));
  file.flush();
  const bool ok = first == sizeof(state) && second == sizeof(empty) &&
      file.getWriteError() == 0;
  file.close();
  if (!ok) return false;
  lease.release();
  StoredCapsuleBatchState verified;
  return readStateSlot(path(transactionId), 0, verified, owner) &&
      memcmp(&verified, &state, sizeof(state)) == 0;
}

bool CapsuleBatchJournalStore::load(
    const String &transactionId, StorageOwner owner,
    StoredCapsuleBatchState &state) const {
  if (fs_ == nullptr) return false;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      owner, StorageAccess::read, 0);
  if (!lease) return false;
  File file = fs_->open(path(transactionId), FILE_READ);
  if (!file || file.isDirectory()) {
    if (file) file.close();
    return false;
  }
  StoredCapsuleBatchState slots[2] = {};
  const bool read = file.read(reinterpret_cast<uint8_t *>(slots),
                              sizeof(slots)) == sizeof(slots);
  file.close();
  if (!read) return false;
  const StoredCapsuleBatchState *newest = newestCapsuleBatchState(
      slots[0], slots[1]);
  if (newest == nullptr || transactionId != newest->transactionId) return false;
  state = *newest;
  return true;
}

bool CapsuleBatchJournalStore::checkpoint(StoredCapsuleBatchState &state,
                                          StorageOwner owner) {
  if (!validCapsuleBatchState(state)) return false;
  const StoredCapsuleBatchState candidate = nextCapsuleBatchCheckpoint(state);
  const size_t slot = candidate.generation & 1U;
  const String journalPath = path(candidate.transactionId);
  if (!writeStateSlot(journalPath, "r+", slot * sizeof(candidate),
                      candidate, owner)) return false;
  StoredCapsuleBatchState verified;
  return readStateSlot(journalPath, slot * sizeof(candidate), verified, owner) &&
      acceptCapsuleBatchCheckpoint(state, candidate, verified);
}

bool CapsuleBatchJournalStore::writeStateSlot(
    const String &journalPath, const char *mode, size_t offset,
    const StoredCapsuleBatchState &state, StorageOwner owner) const {
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      owner, StorageAccess::mutation, 0);
  if (!lease) return false;
  File file = fs_->open(journalPath, mode);
  if (!file || !file.seek(offset)) {
    if (file) file.close();
    return false;
  }
  const bool wrote = file.write(
      reinterpret_cast<const uint8_t *>(&state), sizeof(state)) ==
      sizeof(state);
  file.flush();
  const bool ok = wrote && file.getWriteError() == 0;
  file.close();
  return ok;
}

bool CapsuleBatchJournalStore::readStateSlot(
    const String &journalPath, size_t offset, StoredCapsuleBatchState &state,
    StorageOwner owner) const {
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      owner, StorageAccess::read, 0);
  if (!lease) return false;
  File file = fs_->open(journalPath, FILE_READ);
  if (!file || !file.seek(offset)) {
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
  const bool ok = wrote && file.getWriteError() == 0;
  file.close();
  if (!ok) return false;
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
  return read && validCapsuleBatchPlan(plan);
}

bool CapsuleBatchJournalStore::erase(const String &transactionId,
                                     StorageOwner owner) {
  if (fs_ == nullptr) return false;
  StorageIoLease lease = StorageCoordinator::instance().acquireIo(
      owner, StorageAccess::mutation, 0);
  return lease && (!fs_->exists(path(transactionId)) ||
                   fs_->remove(path(transactionId)));
}

}  // namespace pokepod
