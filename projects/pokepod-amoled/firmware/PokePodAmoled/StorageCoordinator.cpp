#include "StorageCoordinator.h"

#ifdef ARDUINO
#include <esp_timer.h>
#else
#include <chrono>
#endif

namespace pokepod {

namespace {
StorageCoordinator storageCoordinatorInstance;
}

StorageIoLease::StorageIoLease(StorageIoLease &&other) noexcept
    : coordinator_(other.coordinator_), acquiredUs_(other.acquiredUs_) {
  other.coordinator_ = nullptr;
  other.acquiredUs_ = 0;
}

StorageIoLease &StorageIoLease::operator=(StorageIoLease &&other) noexcept {
  if (this != &other) {
    release();
    coordinator_ = other.coordinator_;
    acquiredUs_ = other.acquiredUs_;
    other.coordinator_ = nullptr;
    other.acquiredUs_ = 0;
  }
  return *this;
}

StorageIoLease::~StorageIoLease() { release(); }

void StorageIoLease::release() {
  if (coordinator_ == nullptr) return;
  StorageCoordinator *coordinator = coordinator_;
  coordinator_ = nullptr;
  coordinator->releaseIo(acquiredUs_);
}

StorageReservation::StorageReservation(StorageReservation &&other) noexcept
    : coordinator_(other.coordinator_), owner_(other.owner_),
      access_(other.access_), context_(other.context_) {
  other.coordinator_ = nullptr;
  other.owner_ = StorageOwner::none;
}

StorageReservation &StorageReservation::operator=(
    StorageReservation &&other) noexcept {
  if (this != &other) {
    release();
    coordinator_ = other.coordinator_;
    owner_ = other.owner_;
    access_ = other.access_;
    context_ = other.context_;
    other.coordinator_ = nullptr;
    other.owner_ = StorageOwner::none;
  }
  return *this;
}

StorageReservation::~StorageReservation() { release(); }

void StorageReservation::release() {
  if (coordinator_ == nullptr) return;
  StorageCoordinator *coordinator = coordinator_;
  coordinator_ = nullptr;
  coordinator->releaseReservation(owner_, access_, context_);
  owner_ = StorageOwner::none;
}

StorageCoordinator::StorageCoordinator() {
#ifdef ARDUINO
  mutex_ = xSemaphoreCreateRecursiveMutexStatic(&mutexStorage_);
#endif
}

StorageCoordinator &StorageCoordinator::instance() {
  return storageCoordinatorInstance;
}

uint64_t StorageCoordinator::monotonicUs() {
#ifdef ARDUINO
  return static_cast<uint64_t>(esp_timer_get_time());
#else
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now().time_since_epoch()).count());
#endif
}

uintptr_t StorageCoordinator::currentContext() {
#ifdef ARDUINO
  return reinterpret_cast<uintptr_t>(xTaskGetCurrentTaskHandle());
#else
  static thread_local uint8_t marker;
  return reinterpret_cast<uintptr_t>(&marker);
#endif
}

bool StorageCoordinator::lock(uint32_t timeoutMs, uint64_t &acquiredUs) {
  const uint64_t startedUs = monotonicUs();
#ifdef ARDUINO
  if (mutex_ == nullptr || xSemaphoreTakeRecursive(
          mutex_, timeoutMs == 0 ? 0 : pdMS_TO_TICKS(timeoutMs)) != pdTRUE) {
    return false;
  }
#else
  const bool locked = timeoutMs == 0 ? mutex_.try_lock() :
      mutex_.try_lock_for(std::chrono::milliseconds(timeoutMs));
  if (!locked) return false;
#endif
  acquiredUs = monotonicUs();
  const uint64_t waitedUs = acquiredUs - startedUs;
  if (waitedUs > metrics_.maximumWaitUs) {
    metrics_.maximumWaitUs = static_cast<uint32_t>(
        waitedUs > UINT32_MAX ? UINT32_MAX : waitedUs);
  }
  return true;
}

void StorageCoordinator::unlock() {
#ifdef ARDUINO
  xSemaphoreGiveRecursive(mutex_);
#else
  mutex_.unlock();
#endif
}

void StorageCoordinator::lockUntilAcquired() {
#ifdef ARDUINO
  if (mutex_ != nullptr) xSemaphoreTakeRecursive(mutex_, portMAX_DELAY);
#else
  mutex_.lock();
#endif
}

bool StorageCoordinator::reservationAllows(StorageOwner owner,
                                            StorageAccess access,
                                            uintptr_t context) const {
  if (owner == StorageOwner::none) return false;
  if (access == StorageAccess::mutation) {
    if (mutationOwner_ != StorageOwner::none &&
        (mutationOwner_ != owner || mutationContext_ != context)) {
      return false;
    }
    return readOwner_ == StorageOwner::none ||
        (readOwner_ == owner && readContext_ == context);
  }
  return mutationOwner_ == StorageOwner::none ||
      (mutationOwner_ == owner && mutationContext_ == context);
}

StorageReservation StorageCoordinator::reserve(StorageOwner owner,
                                                StorageAccess access,
                                                uint32_t timeoutMs) {
  uint64_t acquiredUs = 0;
  if (!lock(timeoutMs, acquiredUs)) {
    return {};
  }
  const uintptr_t context = currentContext();
  if (!reservationAllows(owner, access, context)) {
    ++metrics_.rejected;
    unlock();
    return {};
  }
  if (access == StorageAccess::mutation) {
    mutationOwner_ = owner;
    mutationContext_ = context;
    ++mutationDepth_;
  } else {
    if (readOwner_ != StorageOwner::none &&
        (readOwner_ != owner || readContext_ != context)) {
      ++metrics_.rejected;
      unlock();
      return {};
    }
    readOwner_ = owner;
    readContext_ = context;
    ++readDepth_;
  }
  ++metrics_.reservations;
  unlock();
  return StorageReservation(this, owner, access, context);
}

StorageIoLease StorageCoordinator::acquireIo(StorageOwner owner,
                                             StorageAccess access,
                                             uint32_t timeoutMs) {
  uint64_t acquiredUs = 0;
  if (!lock(timeoutMs, acquiredUs)) {
    return {};
  }
  if (!reservationAllows(owner, access, currentContext())) {
    ++metrics_.rejected;
    unlock();
    return {};
  }
  ++metrics_.ioAcquires;
  return StorageIoLease(this, acquiredUs);
}

void StorageCoordinator::releaseIo(uint64_t acquiredUs) {
  const uint64_t heldUs = monotonicUs() - acquiredUs;
  if (heldUs > metrics_.maximumHoldUs) {
    metrics_.maximumHoldUs = static_cast<uint32_t>(
        heldUs > UINT32_MAX ? UINT32_MAX : heldUs);
  }
  unlock();
}

void StorageCoordinator::releaseReservation(StorageOwner owner,
                                             StorageAccess access,
                                             uintptr_t context) {
  // Destruction must not leak a logical owner merely because a long SD read
  // holds the physical mutex at that instant.
  lockUntilAcquired();
  if (access == StorageAccess::mutation && mutationOwner_ == owner &&
      mutationContext_ == context &&
      mutationDepth_ > 0) {
    if (--mutationDepth_ == 0) {
      mutationOwner_ = StorageOwner::none;
      mutationContext_ = 0;
    }
  } else if (access == StorageAccess::read && readOwner_ == owner &&
             readContext_ == context &&
             readDepth_ > 0) {
    if (--readDepth_ == 0) {
      readOwner_ = StorageOwner::none;
      readContext_ = 0;
    }
  }
  unlock();
}

bool StorageCoordinator::mutationActive() const {
  uint64_t acquiredUs = 0;
  StorageCoordinator *self = const_cast<StorageCoordinator *>(this);
  if (!self->lock(0, acquiredUs)) return true;
  const bool active = mutationOwner_ != StorageOwner::none;
  self->unlock();
  return active;
}

StorageOwner StorageCoordinator::mutationOwner() const {
  uint64_t acquiredUs = 0;
  StorageCoordinator *self = const_cast<StorageCoordinator *>(this);
  if (!self->lock(0, acquiredUs)) return StorageOwner::none;
  const StorageOwner owner = mutationOwner_;
  self->unlock();
  return owner;
}

StorageOwner StorageCoordinator::readOwner() const {
  uint64_t acquiredUs = 0;
  StorageCoordinator *self = const_cast<StorageCoordinator *>(this);
  if (!self->lock(0, acquiredUs)) return StorageOwner::none;
  const StorageOwner owner = readOwner_;
  self->unlock();
  return owner;
}

StorageCoordinatorMetrics StorageCoordinator::metrics() const {
  uint64_t acquiredUs = 0;
  StorageCoordinator *self = const_cast<StorageCoordinator *>(this);
  if (!self->lock(0, acquiredUs)) return {};
  const StorageCoordinatorMetrics snapshot = metrics_;
  self->unlock();
  return snapshot;
}

}  // namespace pokepod
