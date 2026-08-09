#pragma once

#include <Arduino.h>
#include <USB.h>

#include "BoardConfig.h"

namespace pokepod {

// USB is deliberately a CDC-only maintenance and synchronization link.
// Real-time voice is owned by BleVoiceService.
class UsbLinkBridge {
 public:
  bool begin(BoardVariant variant);
  Print &log() { return Serial; }
  Stream &stream() { return cdc_; }
  bool ready() const { return started_; }
  bool hostConnected() const;

 private:
  USBCDC cdc_;
  bool started_ = false;
};

}  // namespace pokepod
