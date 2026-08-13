#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace pokepod {

// NimBLE invokes GAP, security and characteristic callbacks from its host
// task. These fixed-size events are the only data that crosses from that task
// into the Arduino loop. No event owns a pointer into NimBLE storage.
enum class BleVoiceCallbackEventType : uint8_t {
  connect,
  disconnect,
  mtu,
  command,
  authentication,
  audioNotifyStatus,
  controlNotifyStatus,
  deviceInfoRead,
  passkey,
};

enum class BleVoiceNotifyKind : uint8_t {
  none,
  audio,
  control,
};

struct BleVoiceNotifyIdentity {
  BleVoiceNotifyKind kind = BleVoiceNotifyKind::none;
  uint16_t connectionId = 0xffff;
  uint32_t connectionGeneration = 0;
  uint32_t sessionGeneration = 0;
  uint32_t attemptToken = 0;

  bool valid() const {
    return kind != BleVoiceNotifyKind::none && connectionId != 0xffff &&
        connectionGeneration != 0 && attemptToken != 0;
  }

  bool matches(const BleVoiceNotifyIdentity &other) const {
    return valid() && other.valid() && kind == other.kind &&
        connectionId == other.connectionId &&
        connectionGeneration == other.connectionGeneration &&
        sessionGeneration == other.sessionGeneration &&
        attemptToken == other.attemptToken;
  }
};

struct BleVoiceCallbackEvent {
  static constexpr size_t kPeerAddressBytes = 6;
  static constexpr size_t kCommandBytes = 8;

  BleVoiceCallbackEventType type = BleVoiceCallbackEventType::disconnect;
  uint16_t connectionId = 0;
  uint16_t value16 = 0;
  uint32_t connectionGeneration = 0;
  uint32_t value32 = 0;
  uint32_t occurredAtMs = 0;
  uint8_t dataLength = 0;
  bool flag = false;
  bool peerAddressValid = false;
  BleVoiceNotifyIdentity notifyIdentity;
  uint8_t peerAddress[kPeerAddressBytes] = {};
  uint8_t data[kCommandBytes] = {};
};

// Arduino-ESP32 3.3.x invokes notification status twice on NimBLE: once from
// notify() itself and again later from the host task's NOTIFY_TX event. The
// callback API exposes only a GATT status code, so the late callback cannot be
// associated with its original send. Bind a token to the synchronous notify()
// call and admit only a callback running on that same owner task. A host-task
// callback therefore cannot borrow a newer attempt's identity.
class BleVoiceNotifyCallbackBinding {
 public:
  bool begin(const BleVoiceNotifyIdentity &identity, uintptr_t ownerTask) {
    if (!identity.valid() || ownerTask == 0) return false;
    writeBegin();
    kind_.store(static_cast<uint8_t>(identity.kind),
                std::memory_order_relaxed);
    connectionId_.store(identity.connectionId, std::memory_order_relaxed);
    connectionGeneration_.store(identity.connectionGeneration,
                                std::memory_order_relaxed);
    sessionGeneration_.store(identity.sessionGeneration,
                             std::memory_order_relaxed);
    attemptToken_.store(identity.attemptToken, std::memory_order_relaxed);
    ownerTask_.store(ownerTask, std::memory_order_relaxed);
    active_.store(true, std::memory_order_relaxed);
    writeEnd();
    return true;
  }

  void invalidate() {
    writeBegin();
    active_.store(false, std::memory_order_relaxed);
    ownerTask_.store(0, std::memory_order_relaxed);
    writeEnd();
  }

  bool capture(BleVoiceNotifyKind expectedKind, uintptr_t callbackTask,
               BleVoiceNotifyIdentity &identity) const {
    if (callbackTask == 0) return false;
    for (;;) {
      const uint32_t before = version_.load(std::memory_order_acquire);
      if ((before & 1U) != 0) continue;
      const bool active = active_.load(std::memory_order_relaxed);
      const uintptr_t ownerTask = ownerTask_.load(std::memory_order_relaxed);
      BleVoiceNotifyIdentity candidate;
      candidate.kind = static_cast<BleVoiceNotifyKind>(
          kind_.load(std::memory_order_relaxed));
      candidate.connectionId =
          connectionId_.load(std::memory_order_relaxed);
      candidate.connectionGeneration =
          connectionGeneration_.load(std::memory_order_relaxed);
      candidate.sessionGeneration =
          sessionGeneration_.load(std::memory_order_relaxed);
      candidate.attemptToken =
          attemptToken_.load(std::memory_order_relaxed);
      const uint32_t after = version_.load(std::memory_order_acquire);
      if (before != after) continue;
      if (!active || ownerTask != callbackTask ||
          candidate.kind != expectedKind || !candidate.valid()) {
        return false;
      }
      identity = candidate;
      return true;
    }
  }

 private:
  void writeBegin() {
    version_.fetch_add(1, std::memory_order_acq_rel);
  }
  void writeEnd() {
    version_.fetch_add(1, std::memory_order_release);
  }

  std::atomic<uint32_t> version_{0};
  std::atomic<bool> active_{false};
  std::atomic<uintptr_t> ownerTask_{0};
  std::atomic<uint8_t> kind_{
      static_cast<uint8_t>(BleVoiceNotifyKind::none)};
  std::atomic<uint16_t> connectionId_{0xffff};
  std::atomic<uint32_t> connectionGeneration_{0};
  std::atomic<uint32_t> sessionGeneration_{0};
  std::atomic<uint32_t> attemptToken_{0};
};

struct BleVoiceConnectionEpoch {
  uint16_t connectionId = 0xffff;
  uint32_t generation = 0;

  bool valid() const { return connectionId != 0xffff && generation != 0; }
  bool matches(const BleVoiceConnectionEpoch &other) const {
    return valid() && other.valid() && connectionId == other.connectionId &&
        generation == other.generation;
  }
};

// The normal callback mailbox deliberately closes admission on overflow, so a
// physical disconnect confirmation needs a separate one-slot atomic latch.
// The latch is owner-scoped by connection generation and cannot rearm a newer
// connection that happens to reuse the same 16-bit controller handle.
class BleVoicePhysicalDisconnectLatch {
 public:
  void observe(uint16_t connectionId, uint32_t generation) {
    if (connectionId == 0xffff || generation == 0) return;
    version_.fetch_add(1, std::memory_order_acq_rel);
    connectionId_.store(connectionId, std::memory_order_relaxed);
    generation_.store(generation, std::memory_order_relaxed);
    version_.fetch_add(1, std::memory_order_release);
  }

  bool latest(BleVoiceConnectionEpoch &epoch) const {
    for (;;) {
      const uint32_t before = version_.load(std::memory_order_acquire);
      if (before == 0 || (before & 1U) != 0) return false;
      BleVoiceConnectionEpoch candidate;
      candidate.connectionId =
          connectionId_.load(std::memory_order_relaxed);
      candidate.generation = generation_.load(std::memory_order_relaxed);
      const uint32_t after = version_.load(std::memory_order_acquire);
      if (before != after) continue;
      if (!candidate.valid()) return false;
      epoch = candidate;
      return true;
    }
  }

 private:
  std::atomic<uint32_t> version_{0};
  std::atomic<uint16_t> connectionId_{0xffff};
  std::atomic<uint32_t> generation_{0};
};

// Synchronous security callbacks must return before the Arduino loop can drain
// the connect event. This atomic snapshot contains only the minimum immutable
// decision data; session and service state still belong to the loop.
class BleVoiceCallbackSecuritySnapshot {
 public:
  bool observeConnect(uint16_t connectionId, bool peerBonded,
                      uint32_t *generation = nullptr) {
    if (connectionId == kInvalidConnectionId) return false;
    uint16_t expected = kInvalidConnectionId;
    if (connectionId_.compare_exchange_strong(
            expected, connectionId, std::memory_order_acq_rel)) {
      uint32_t next = generationCounter_.fetch_add(
          1, std::memory_order_acq_rel) + 1;
      if (next == 0) {
        generationCounter_.store(1, std::memory_order_release);
        next = 1;
      }
      connectionGeneration_.store(next, std::memory_order_release);
    } else if (expected != connectionId) {
      return false;
    }
    securityAllowed_.store(
        peerBonded || pairingAllowed_.load(std::memory_order_acquire),
        std::memory_order_release);
    if (generation != nullptr) {
      *generation = connectionGeneration_.load(std::memory_order_acquire);
    }
    return true;
  }

  bool observeDisconnect(uint16_t connectionId,
                         uint32_t *generation = nullptr) {
    if (connectionId == kInvalidConnectionId) return false;
    const uint32_t observedGeneration = generationFor(connectionId);
    if (observedGeneration == 0) return false;
    uint16_t expected = connectionId;
    if (!connectionId_.compare_exchange_strong(
            expected, kInvalidConnectionId, std::memory_order_acq_rel)) {
      return false;
    }
    securityAllowed_.store(false, std::memory_order_release);
    if (generation != nullptr) *generation = observedGeneration;
    return true;
  }

  void refreshFromMain(bool hasConnection, uint16_t connectionId,
                       bool securityAllowed, bool pairingAllowed,
                       uint32_t passkey) {
    pairingAllowed_.store(pairingAllowed, std::memory_order_release);
    passkey_.store(passkey, std::memory_order_release);
    if (hasConnection &&
        connectionId_.load(std::memory_order_acquire) == connectionId) {
      securityAllowed_.store(securityAllowed, std::memory_order_release);
    } else if (connectionId_.load(std::memory_order_acquire) ==
               kInvalidConnectionId) {
      securityAllowed_.store(pairingAllowed, std::memory_order_release);
    }
  }

  void clearConnection() {
    connectionId_.store(kInvalidConnectionId, std::memory_order_release);
    securityAllowed_.store(false, std::memory_order_release);
  }

  bool securityAllowed() const {
    return securityAllowed_.load(std::memory_order_acquire);
  }
  bool authorizationAllowed(uint16_t connectionId) const {
    return securityAllowed() &&
        connectionId_.load(std::memory_order_acquire) == connectionId;
  }
  uint16_t connectionId() const {
    return connectionId_.load(std::memory_order_acquire);
  }
  uint32_t connectionGeneration() const {
    return connectionGeneration_.load(std::memory_order_acquire);
  }
  uint32_t generationFor(uint16_t connectionId) const {
    if (connectionId_.load(std::memory_order_acquire) != connectionId) {
      return 0;
    }
    return connectionGeneration();
  }
  uint32_t passkey() const {
    return passkey_.load(std::memory_order_acquire);
  }

  static constexpr uint16_t kInvalidConnectionId = 0xffff;

 private:
  std::atomic<bool> securityAllowed_{false};
  std::atomic<bool> pairingAllowed_{false};
  std::atomic<uint16_t> connectionId_{kInvalidConnectionId};
  std::atomic<uint32_t> connectionGeneration_{0};
  std::atomic<uint32_t> generationCounter_{0};
  std::atomic<uint32_t> passkey_{0};
};

// NimBLE serializes callbacks on its host task, so this is a bounded
// single-producer/single-consumer mailbox. Producer and consumer never wait for
// each other. Capacity exhaustion latches overflow and stops admission; the
// Arduino loop then tears down the BLE session before rearming the mailbox.
template <size_t Capacity>
class BleVoiceCallbackMailbox {
 public:
  static_assert(Capacity > 0, "BLE callback mailbox must be bounded");

  bool publish(const BleVoiceCallbackEvent &event) {
    if (!accepting_.load(std::memory_order_acquire)) return false;
    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    const uint32_t head = head_.load(std::memory_order_acquire);
    if (tail - head >= Capacity) {
      failClosed();
      return false;
    }
    events_[tail % Capacity] = event;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  bool take(BleVoiceCallbackEvent &event) {
    const uint32_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_.load(std::memory_order_acquire)) return false;
    event = events_[head % Capacity];
    head_.store(head + 1, std::memory_order_release);
    return true;
  }

  bool overflowed() const {
    return overflowed_.load(std::memory_order_acquire);
  }

  bool accepting() const {
    return accepting_.load(std::memory_order_acquire);
  }

  // Read-only owner observation used by the sleep quiescence contract. A
  // concurrent publish either advances tail before this snapshot or remains a
  // producer-side fact for the next poll; it never changes queue semantics.
  bool empty() const {
    return head_.load(std::memory_order_acquire) ==
        tail_.load(std::memory_order_acquire);
  }

  void closeAdmission() { failClosed(); }

  // Only the Arduino-loop owner may rearm. Once admission is false, any
  // later producer returns without touching a slot.
  bool resetAfterOverflow() {
    accepting_.store(false, std::memory_order_release);
    head_.store(tail_.load(std::memory_order_acquire),
                std::memory_order_release);
    overflowed_.store(false, std::memory_order_release);
    accepting_.store(true, std::memory_order_release);
    return true;
  }

  constexpr size_t capacity() const { return Capacity; }

 private:
  void failClosed() {
    overflowed_.store(true, std::memory_order_release);
    accepting_.store(false, std::memory_order_release);
  }

  BleVoiceCallbackEvent events_[Capacity] = {};
  std::atomic<uint32_t> head_{0};
  std::atomic<uint32_t> tail_{0};
  std::atomic<bool> accepting_{true};
  std::atomic<bool> overflowed_{false};
};

}  // namespace pokepod
