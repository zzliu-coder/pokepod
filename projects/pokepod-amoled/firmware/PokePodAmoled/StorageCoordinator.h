#pragma once

#include <atomic>
#include <stdint.h>

#ifdef ARDUINO
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#else
#include <mutex>
#endif

namespace pokepod {

enum class StorageOwner : uint8_t {
  none = 0,
  recorder,
  capsuleTransaction,
  tencentRead,
  audioPlayback,
  fontRead,
  usbLink,
  wifiLink,
  recovery,
};

enum class StorageAccess : uint8_t { read = 0, mutation = 1 };

struct StorageCoordinatorMetrics {
  uint32_t ioAcquires = 0;
  uint32_t reservations = 0;
  uint32_t rejected = 0;
  uint32_t maximumWaitUs = 0;
  uint32_t maximumHoldUs = 0;
};

class StorageCoordinator;

class StorageIoLease {
 public:
  StorageIoLease() = default;
  StorageIoLease(const StorageIoLease &) = delete;
  StorageIoLease &operator=(const StorageIoLease &) = delete;
  StorageIoLease(StorageIoLease &&other) noexcept;
  StorageIoLease &operator=(StorageIoLease &&other) noexcept;
  ~StorageIoLease();

  explicit operator bool() const { return coordinator_ != nullptr; }
  void release();

 private:
  friend class StorageCoordinator;
  StorageIoLease(StorageCoordinator *coordinator, uint64_t acquiredUs,
                 StorageAccess access)
      : coordinator_(coordinator), acquiredUs_(acquiredUs), access_(access) {}

  StorageCoordinator *coordinator_ = nullptr;
  uint64_t acquiredUs_ = 0;
  StorageAccess access_ = StorageAccess::read;
};

class StorageReservation {
 public:
  StorageReservation() = default;
  StorageReservation(const StorageReservation &) = delete;
  StorageReservation &operator=(const StorageReservation &) = delete;
  StorageReservation(StorageReservation &&other) noexcept;
  StorageReservation &operator=(StorageReservation &&other) noexcept;
  ~StorageReservation();

  explicit operator bool() const { return coordinator_ != nullptr; }
  StorageOwner owner() const { return owner_; }
  StorageAccess access() const { return access_; }
  void release();

 private:
  friend class StorageCoordinator;
  StorageReservation(StorageCoordinator *coordinator, StorageOwner owner,
                     StorageAccess access, uintptr_t context)
      : coordinator_(coordinator), owner_(owner), access_(access),
        context_(context) {}

  StorageCoordinator *coordinator_ = nullptr;
  StorageOwner owner_ = StorageOwner::none;
  StorageAccess access_ = StorageAccess::read;
  uintptr_t context_ = 0;
};

class StorageCoordinator {
 public:
  StorageCoordinator();
  static StorageCoordinator &instance();

  StorageReservation reserve(StorageOwner owner, StorageAccess access,
                             uint32_t timeoutMs = 0);
  StorageIoLease acquireIo(StorageOwner owner, StorageAccess access,
                           uint32_t timeoutMs = 0);

  bool mutationActive() const;
  StorageOwner mutationOwner() const;
  StorageOwner readOwner() const;
  StorageCoordinatorMetrics metrics() const;

 private:
  friend class StorageIoLease;
  friend class StorageReservation;

  bool lock(uint32_t timeoutMs, uint64_t &acquiredUs);
  void lockUntilAcquired();
  void unlock();
  void releaseIo(uint64_t acquiredUs, StorageAccess access);
  void releaseReservation(StorageOwner owner, StorageAccess access,
                          uintptr_t context);
  bool reservationAllows(StorageOwner owner, StorageAccess access,
                         uintptr_t context) const;
  static uintptr_t currentContext();
  static uint64_t monotonicUs();

#ifdef ARDUINO
  StaticSemaphore_t mutexStorage_{};
  SemaphoreHandle_t mutex_ = nullptr;
#else
  mutable std::recursive_timed_mutex mutex_;
#endif
  StorageOwner mutationOwner_ = StorageOwner::none;
  StorageOwner readOwner_ = StorageOwner::none;
  uintptr_t mutationContext_ = 0;
  uintptr_t readContext_ = 0;
  uint16_t mutationDepth_ = 0;
  uint16_t readDepth_ = 0;
  std::atomic<uint16_t> mutationReservationDepth_{0};
  std::atomic<uint16_t> mutationIoDepth_{0};
  StorageCoordinatorMetrics metrics_{};
};

}  // namespace pokepod
