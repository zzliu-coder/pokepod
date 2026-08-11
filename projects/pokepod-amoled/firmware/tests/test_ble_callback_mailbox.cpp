#include <assert.h>

#include <atomic>
#include <thread>

#include "AudioCaptureRouter.h"
#include "BleNotifyReliability.h"
#include "BleVoiceCallbackMailbox.h"
#include "BleVoiceProtocol.h"
#include "VoiceSessionController.h"

using namespace pokepod;

namespace {

BleVoiceCallbackEvent event(BleVoiceCallbackEventType type,
                            uint16_t connectionId = 0) {
  BleVoiceCallbackEvent value;
  value.type = type;
  value.connectionId = connectionId;
  return value;
}

BleVoiceCallbackEvent command(uint16_t connectionId,
                              BleVoiceCommandType type,
                              uint32_t sessionId) {
  BleVoiceControl control;
  control.type = static_cast<uint8_t>(type);
  control.sessionId = sessionId;
  BleVoiceCallbackEvent value =
      event(BleVoiceCallbackEventType::command, connectionId);
  value.dataLength = static_cast<uint8_t>(
      encodeBleVoiceControl(control, value.data, sizeof(value.data)));
  return value;
}

void waitFor(const std::atomic<int> &phase, int expected) {
  while (phase.load(std::memory_order_acquire) != expected) {
    std::this_thread::yield();
  }
}

struct MainOwnerModel {
  void consume(const BleVoiceCallbackEvent &value, uint32_t nowMs) {
    switch (value.type) {
      case BleVoiceCallbackEventType::connect:
        connected = true;
        connectionId = value.connectionId;
        break;
      case BleVoiceCallbackEventType::disconnect:
        if (value.connectionId == connectionId) {
          connected = authenticated = appReady = false;
          controller.abort(VoiceSessionError::disconnected);
        }
        break;
      case BleVoiceCallbackEventType::authentication:
        if (value.connectionId == connectionId) authenticated = value.flag;
        break;
      case BleVoiceCallbackEventType::mtu:
        if (value.connectionId == connectionId) mtu = value.value16;
        break;
      case BleVoiceCallbackEventType::command: {
        BleVoiceControl control;
        if (!decodeBleVoiceControl(value.data, value.dataLength, control)) break;
        const auto type = static_cast<BleVoiceCommandType>(control.type);
        if (type == BleVoiceCommandType::ready && control.sessionId == 0) {
          appReady = connected && authenticated && bleVoiceMtuReady(mtu);
        } else if (type == BleVoiceCommandType::ready) {
          controller.markReady(control.sessionId, nowMs);
        } else if (type == BleVoiceCommandType::stopAck &&
                   controller.acceptsStopAck(control.sessionId)) {
          controller.complete();
        }
        break;
      }
      case BleVoiceCallbackEventType::audioNotifyStatus:
        notify.resolve(value.flag, nowMs);
        break;
      case BleVoiceCallbackEventType::controlNotifyStatus:
      case BleVoiceCallbackEventType::deviceInfoRead:
      case BleVoiceCallbackEventType::passkey:
        break;
    }
  }

  bool connected = false;
  bool authenticated = false;
  bool appReady = false;
  uint16_t connectionId = 0;
  uint16_t mtu = 23;
  AudioCaptureRouter router;
  VoiceSessionController controller;
  BleNotifyInFlight notify;
};

template <size_t Capacity>
void drain(BleVoiceCallbackMailbox<Capacity> &mailbox, MainOwnerModel &owner,
           uint32_t nowMs) {
  BleVoiceCallbackEvent value;
  while (mailbox.take(value)) owner.consume(value, nowMs);
}

}  // namespace

int main() {
  BleVoiceCallbackMailbox<16> mailbox;
  MainOwnerModel owner;
  std::atomic<int> phase{0};

  // The callback thread only publishes immutable fixed events. All session,
  // capture, PCM and in-flight notification state remains on this main thread.
  std::thread callbackThread([&]() {
    waitFor(phase, 1);
    BleVoiceCallbackEvent connected =
        event(BleVoiceCallbackEventType::connect, 41);
    connected.flag = true;
    assert(mailbox.publish(connected));
    BleVoiceCallbackEvent authenticated =
        event(BleVoiceCallbackEventType::authentication, 41);
    authenticated.flag = true;
    assert(mailbox.publish(authenticated));
    BleVoiceCallbackEvent mtu = event(BleVoiceCallbackEventType::mtu, 41);
    mtu.value16 = 185;
    assert(mailbox.publish(mtu));
    assert(mailbox.publish(command(41, BleVoiceCommandType::ready, 0)));
    phase.store(2, std::memory_order_release);

    waitFor(phase, 3);
    assert(mailbox.publish(command(41, BleVoiceCommandType::ready, 501)));
    phase.store(4, std::memory_order_release);

    waitFor(phase, 5);
    BleVoiceCallbackEvent accepted =
        event(BleVoiceCallbackEventType::audioNotifyStatus, 41);
    accepted.flag = true;
    assert(mailbox.publish(accepted));
    phase.store(6, std::memory_order_release);

    waitFor(phase, 7);
    assert(mailbox.publish(command(41, BleVoiceCommandType::stopAck, 501)));
    phase.store(8, std::memory_order_release);

    waitFor(phase, 9);
    // A stale first-session ready cannot authorize the second session.
    assert(mailbox.publish(command(41, BleVoiceCommandType::ready, 501)));
    assert(mailbox.publish(command(41, BleVoiceCommandType::ready, 502)));
    assert(mailbox.publish(event(BleVoiceCallbackEventType::disconnect, 41)));
    phase.store(10, std::memory_order_release);
  });

  phase.store(1, std::memory_order_release);
  waitFor(phase, 2);
  drain(mailbox, owner, 10);
  assert(owner.connected && owner.authenticated && owner.appReady);
  assert(owner.mtu == 185);

  assert(owner.controller.begin(501, 11, owner.connected, owner.mtu,
                                owner.router));
  int16_t pcm[kBleVoiceSamplesPerFrame] = {};
  assert(owner.controller.appendMono16(pcm, kBleVoiceSamplesPerFrame, 12));
  phase.store(3, std::memory_order_release);
  waitFor(phase, 4);
  drain(mailbox, owner, 13);
  assert(owner.controller.state() == VoiceSessionState::streaming);

  BleVoiceAudioFrame frame;
  assert(owner.controller.peekFrame(frame));
  const uint32_t sequence = readVoiceU32(frame.bytes + 6);
  assert(owner.notify.start(sequence, 14));
  assert(owner.notify.beginAttempt(14));
  phase.store(5, std::memory_order_release);
  waitFor(phase, 6);
  drain(mailbox, owner, 15);
  assert(owner.notify.accepted());
  assert(owner.controller.commitFrame(sequence));

  owner.controller.end();
  assert(owner.controller.markSessionEndSent(16));
  phase.store(7, std::memory_order_release);
  waitFor(phase, 8);
  drain(mailbox, owner, 17);
  assert(owner.controller.state() == VoiceSessionState::idle);
  assert(owner.router.available());

  assert(owner.controller.begin(502, 18, owner.connected, owner.mtu,
                                owner.router));
  phase.store(9, std::memory_order_release);
  waitFor(phase, 10);
  drain(mailbox, owner, 19);
  assert(!owner.connected);
  assert(owner.controller.state() == VoiceSessionState::failed);
  assert(owner.controller.error() == VoiceSessionError::disconnected);
  assert(owner.router.available());
  callbackThread.join();

  // Sustained producer/consumer overlap cannot create a false overflow. The
  // producer applies bounded backpressure in the test so capacity exhaustion
  // is excluded and every fixed event must arrive exactly once and in order.
  constexpr uint32_t kStressEvents = 20000;
  BleVoiceCallbackMailbox<32> stress;
  std::atomic<uint32_t> consumed{0};
  std::thread stressProducer([&]() {
    for (uint32_t sequenceValue = 0; sequenceValue < kStressEvents;
         ++sequenceValue) {
      while (sequenceValue - consumed.load(std::memory_order_acquire) >= 16) {
        std::this_thread::yield();
      }
      BleVoiceCallbackEvent value =
          event(BleVoiceCallbackEventType::passkey);
      value.value32 = sequenceValue;
      assert(stress.publish(value));
    }
  });
  for (uint32_t expected = 0; expected < kStressEvents;) {
    BleVoiceCallbackEvent value;
    if (!stress.take(value)) {
      std::this_thread::yield();
      continue;
    }
    assert(value.type == BleVoiceCallbackEventType::passkey);
    assert(value.value32 == expected);
    ++expected;
    consumed.store(expected, std::memory_order_release);
  }
  stressProducer.join();
  assert(!stress.overflowed());
  assert(stress.accepting());

  // A secondary callback cannot steal the synchronous security snapshot from
  // the current controller connection.
  BleVoiceCallbackSecuritySnapshot security;
  assert(!security.observeConnect(
      BleVoiceCallbackSecuritySnapshot::kInvalidConnectionId, true));
  assert(!security.securityAllowed());
  security.refreshFromMain(false,
                           BleVoiceCallbackSecuritySnapshot::kInvalidConnectionId,
                           false, true, 123456);
  assert(security.passkey() == 123456);
  assert(security.observeConnect(10, false));
  assert(security.connectionId() == 10);
  assert(security.securityAllowed());
  assert(security.authorizationAllowed(10));
  assert(!security.observeConnect(11, true));
  assert(security.connectionId() == 10);
  assert(security.authorizationAllowed(10));
  assert(!security.authorizationAllowed(11));
  assert(!security.observeDisconnect(11));
  assert(security.connectionId() == 10);
  assert(security.observeDisconnect(10));
  assert(security.connectionId() ==
         BleVoiceCallbackSecuritySnapshot::kInvalidConnectionId);
  assert(!security.securityAllowed());
  // A main-loop snapshot taken immediately before the disconnect callback
  // cannot resurrect the physically closed connection.
  security.refreshFromMain(true, 10, true, false, 654321);
  assert(security.connectionId() ==
         BleVoiceCallbackSecuritySnapshot::kInvalidConnectionId);
  assert(!security.securityAllowed());
  assert(security.passkey() == 654321);
  assert(security.observeConnect(12, true));
  assert(security.authorizationAllowed(12));

  // Capacity exhaustion is sticky and fail-closed. No later session event can
  // sneak through until the main owner explicitly resets the mailbox.
  BleVoiceCallbackMailbox<2> overflow;
  assert(overflow.publish(event(BleVoiceCallbackEventType::connect, 7)));
  assert(overflow.publish(event(BleVoiceCallbackEventType::mtu, 7)));
  assert(!overflow.publish(command(7, BleVoiceCommandType::ready, 0)));
  assert(overflow.overflowed());
  assert(!overflow.accepting());
  assert(!overflow.publish(event(BleVoiceCallbackEventType::disconnect, 7)));
  assert(overflow.resetAfterOverflow());
  assert(!overflow.overflowed() && overflow.accepting());
  assert(overflow.publish(event(BleVoiceCallbackEventType::connect, 8)));
  return 0;
}
