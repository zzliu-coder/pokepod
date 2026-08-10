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
  // Physical USB mount drives the persistent Mac/cable icon.
  bool hostConnected() const;
  // DTR tracks whether a desktop process currently owns the CDC session.
  bool hostSessionActive() const { return cdcSession_.active(); }
  bool takeHostSessionClosed() { return cdcSession_.takeClosed(); }
  // Drop bytes owned by a closed CDC session before Link v2 can poll again.
  void discardHostSessionBuffers();

 private:
  USBCDC cdc_;
  UsbCdcSessionState cdcSession_;
  bool started_ = false;
};

}  // namespace pokepod
