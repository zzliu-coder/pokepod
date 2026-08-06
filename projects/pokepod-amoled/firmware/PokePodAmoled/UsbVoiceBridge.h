#pragma once

#include <atomic>

#include <Arduino.h>
#include <USB.h>
#include <USBHIDKeyboard.h>
#include <USBAudioCard.h>

#include "BoardConfig.h"

namespace pokepod {

class UsbVoiceBridge {
 public:
  UsbVoiceBridge();

  bool begin(BoardVariant variant);
  bool sendDictationTrigger();
  uint16_t writeMicrophone(const uint8_t *data, uint16_t length);
  // Link v2 owns the CDC byte stream. Diagnostics must never be written to
  // that stream because one printable log line would corrupt a framed reply.
  Print &log() { return Serial; }
  Stream &stream() { return diagnostics_; }
  bool ready() const { return started_; }
  bool hostConnected() const;
  bool microphoneStreaming() const { return microphoneStreaming_.load(); }
  uint32_t microphoneOpenCount() const { return microphoneOpenCount_.load(); }
  uint32_t microphoneCloseCount() const { return microphoneCloseCount_.load(); }
  uint64_t microphoneBytesAttempted() const { return microphoneBytesAttempted_; }
  uint64_t microphoneBytesAccepted() const { return microphoneBytesAccepted_; }
  uint32_t microphoneShortWrites() const { return microphoneShortWrites_; }
  uint64_t microphoneUsbBytesSent() const;
  uint32_t microphoneUsbPacketsSent() const;
  uint32_t microphoneUsbZeroLengthPackets() const;

 private:
  static void onAudioEvent(void *arg, esp_event_base_t eventBase,
                           int32_t eventId, void *eventData);
  static UsbVoiceBridge *instance_;

  USBHIDKeyboard keyboard_;
  USBAudioCard microphone_;
  USBCDC diagnostics_;
  bool started_ = false;
  std::atomic<bool> microphoneStreaming_{false};
  std::atomic<uint32_t> microphoneOpenCount_{0};
  std::atomic<uint32_t> microphoneCloseCount_{0};
  uint64_t microphoneBytesAttempted_ = 0;
  uint64_t microphoneBytesAccepted_ = 0;
  uint32_t microphoneShortWrites_ = 0;
  uint8_t microphoneMonoBuffer_[kUsbAudioBytesPerChunk] = {};
};

}  // namespace pokepod
