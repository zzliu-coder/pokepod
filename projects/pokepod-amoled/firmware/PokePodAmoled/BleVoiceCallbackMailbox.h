#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace pokepod {

// NimBLE invokes GAP, security, characteristic and notify callbacks from its
// host task. These fixed-size events are the only data that crosses from that
// task into the Arduino loop. No event owns a pointer into NimBLE storage.
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

struct BleVoiceCallbackEvent {
  static constexpr size_t kPeerAddressBytes = 6;
  static constexpr size_t kCommandBytes = 8;

  BleVoiceCallbackEventType type = BleVoiceCallbackEventType::disconnect;
  uint16_t connectionId = 0;
  uint16_t value16 = 0;
  uint32_t value32 = 0;
  uint32_t occurredAtMs = 0;
  uint8_t dataLength = 0;
  bool flag = false;
  bool peerAddressValid = false;
  uint8_t peerAddress[kPeerAddressBytes] = {};
  uint8_t data[kCommandBytes] = {};
};

// Synchronous security callbacks must return before the Arduino loop can drain
// the connect event. This atomic snapshot contains only the minimum immutable
// decision data; session and service state still belong to the loop.
class BleVoiceCallbackSecuritySnapshot {
 public:
  bool observeConnect(uint16_t connectionId, bool peerBonded) {
    if (connectionId == kInvalidConnectionId) return false;
    uint16_t expected = kInvalidConnectionId;
    if (!connectionId_.compare_exchange_strong(
            expected, connectionId, std::memory_order_acq_rel) &&
        expected != connectionId) {
      return false;
    }
    securityAllowed_.store(
        peerBonded || pairingAllowed_.load(std::memory_order_acquire),
        std::memory_order_release);
    return true;
  }

  bool observeDisconnect(uint16_t connectionId) {
    if (connectionId == kInvalidConnectionId) return false;
    uint16_t expected = connectionId;
    if (!connectionId_.compare_exchange_strong(
            expected, kInvalidConnectionId, std::memory_order_acq_rel)) {
      return false;
    }
    securityAllowed_.store(false, std::memory_order_release);
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
  uint32_t passkey() const {
    return passkey_.load(std::memory_order_acquire);
  }

  static constexpr uint16_t kInvalidConnectionId = 0xffff;

 private:
  std::atomic<bool> securityAllowed_{false};
  std::atomic<bool> pairingAllowed_{false};
  std::atomic<uint16_t> connectionId_{kInvalidConnectionId};
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
