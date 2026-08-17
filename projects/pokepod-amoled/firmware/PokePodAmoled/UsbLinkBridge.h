#pragma once

#include <Arduino.h>
#include <USB.h>
#include <atomic>

#include "BoardConfig.h"
#include "LinkTransferStepper.h"
#include "UsbCdcSessionState.h"

namespace pokepod {

struct UsbLinkTransportSnapshot {
  uint32_t writeAttempts = 0;
  uint32_t writeProgress = 0;
  uint32_t writeWouldBlock = 0;
  uint32_t writeDisconnected = 0;
  uint32_t consecutiveWouldBlock = 0;
  uint32_t lastWriteBytes = 0;
  bool dtr = false;
  bool rts = false;
};

// USB is deliberately a CDC-only maintenance and synchronization link.
// Real-time voice is owned by BleVoiceService.
class UsbLinkBridge : public LinkWriteChannel {
 public:
  bool begin(BoardVariant variant);
  Print &log() { return Serial; }
  Stream &stream() { return cdc_; }
  bool ready() const { return started_; }
  // Compatibility alias for TinyUSB's mounted state. Do not use it as a CDC
  // session or sleep blocker.
  bool hostConnected() const;
  bool tinyUsbMounted() const { return hostConnected(); }
  // DTR tracks whether a desktop process currently owns the CDC session.
  bool hostSessionActive() const { return cdcSession_.active(); }
  bool cdcSessionActive() const { return hostSessionActive(); }
  UsbCdcSessionSnapshot hostSessionSnapshot() const {
    return cdcSession_.snapshot();
  }
  bool takeHostSessionClosed(uint32_t &generation) {
    return cdcSession_.takeClosed(generation);
  }
  // Drop receive bytes owned by a closed CDC session before Link v2 can poll
  // again. Transmit ownership is cancelled by PokePodLinkService first; never
  // clear TinyUSB's TX FIFO outside Arduino-ESP32's tx_lock.
  void discardHostSessionBuffers();
  LinkWriteAttempt writeSome(const uint8_t *data, size_t size) override;
  UsbLinkTransportSnapshot transportSnapshot() const;

 private:
  static void cdcEventCallback(void *, esp_event_base_t eventBase,
                               int32_t eventId, void *eventData);
  static UsbLinkBridge *activeBridge_;

  void observeLineState(bool dtr, bool rts);
  void observeDisconnected();

  USBCDC cdc_;
  UsbCdcSessionState cdcSession_;
  bool started_ = false;
  std::atomic<uint32_t> writeAttempts_{0};
  std::atomic<uint32_t> writeProgress_{0};
  std::atomic<uint32_t> writeWouldBlock_{0};
  std::atomic<uint32_t> writeDisconnected_{0};
  std::atomic<uint32_t> consecutiveWouldBlock_{0};
  std::atomic<uint32_t> lastWriteBytes_{0};
  std::atomic<bool> dtr_{false};
  std::atomic<bool> rts_{false};
};

}  // namespace pokepod
