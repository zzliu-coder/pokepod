#pragma once

#include <Arduino.h>
#include <USB.h>

#include "BoardConfig.h"
#include "UsbCdcSessionState.h"

namespace pokepod {

// USB is deliberately a CDC-only maintenance and synchronization link.
// Real-time voice is owned by BleVoiceService.
class UsbLinkBridge {
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
  // Drop bytes owned by a closed CDC session before Link v2 can poll again.
  void discardHostSessionBuffers();

 private:
  USBCDC cdc_;
  UsbCdcSessionState cdcSession_;
  bool started_ = false;
};

}  // namespace pokepod
