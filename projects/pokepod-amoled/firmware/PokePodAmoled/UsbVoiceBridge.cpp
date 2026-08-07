#include "UsbVoiceBridge.h"

namespace {

std::atomic<uint64_t> microphoneUsbBytesSent{0};
std::atomic<uint32_t> microphoneUsbPacketsSent{0};
std::atomic<uint32_t> microphoneUsbZeroLengthPackets{0};

}  // namespace

extern "C" bool tud_audio_tx_done_isr(uint8_t, uint16_t bytesSent,
                                      uint8_t, uint8_t, uint8_t) {
  microphoneUsbBytesSent.fetch_add(bytesSent, std::memory_order_relaxed);
  microphoneUsbPacketsSent.fetch_add(1, std::memory_order_relaxed);
  if (bytesSent == 0) {
    microphoneUsbZeroLengthPackets.fetch_add(1, std::memory_order_relaxed);
  }
  return true;
}

namespace pokepod {

UsbVoiceBridge *UsbVoiceBridge::instance_ = nullptr;

UsbVoiceBridge::UsbVoiceBridge()
    : microphone_(kAudioSampleRate, UAC_BPS_16,
                  UAC_SPK_NONE, UAC_MIC_MONO) {
  instance_ = this;
}

bool UsbVoiceBridge::begin(BoardVariant variant) {
  char serial[18];
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(serial, sizeof(serial), "%04X%08X",
           static_cast<unsigned>((mac >> 32) & 0xffff),
           static_cast<unsigned>(mac & 0xffffffff));

  const char *productName = "PokePod Voice";
  if (variant == BoardVariant::v1Sh8601Ft3168) productName = "PokePod V1 Voice";
  if (variant == BoardVariant::v2Co5300Cst820) productName = "PokePod V2 Voice";

  USB.manufacturerName("PokeCapsule");
  USB.productName(productName);
  USB.serialNumber(serial);
  diagnostics_.enableReboot(true);
  diagnostics_.begin(115200);
  keyboard_.begin();
  microphone_.onEvent(ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT, onAudioEvent);
  if (!microphone_.begin()) return false;
  started_ = USB.begin();
  return started_;
}

void UsbVoiceBridge::onAudioEvent(void *, esp_event_base_t eventBase,
                                  int32_t eventId, void *eventData) {
  if (instance_ == nullptr || eventBase != ARDUINO_USB_AUDIO_CARD_EVENTS ||
      eventId != ARDUINO_USB_AUDIO_CARD_INTERFACE_ENABLE_EVENT || eventData == nullptr) {
    return;
  }
  const auto *data = static_cast<arduino_usb_audio_card_event_data_t *>(eventData);
  if (data->interface_enable.interface == UAC_INTERFACE_MIC) {
    if (data->interface_enable.enable &&
        !instance_->microphoneStreaming_.exchange(true)) {
      instance_->microphoneOpenCount_.fetch_add(1);
    } else if (!data->interface_enable.enable) {
      if (instance_->microphoneStreaming_.exchange(false)) {
        instance_->microphoneCloseCount_.fetch_add(1);
      }
    }
  }
}

bool UsbVoiceBridge::beginDictationHold() {
  if (!hostConnected()) return false;
  if (dictationHeld_) return true;

  // Send the chord as one boot-keyboard report. Building it through separate
  // press() calls caused macOS to receive only the modifier transitions on
  // this composite HID/UAC/CDC device.
  KeyReport released = {};
  KeyReport optionZ = {};
  optionZ.modifiers = 1U << 2;  // HID left Alt/Option modifier.
  optionZ.keys[0] = 0x1d;      // HID keyboard Z usage.
  keyboard_.sendReport(&released);
  delay(20);
  keyboard_.sendReport(&optionZ);
  delay(40);
  dictationHeld_ = true;
  Serial.println("{\"event\":\"dictation_started\",\"shortcut\":\"OPTION_Z\"}");
  return true;
}

bool UsbVoiceBridge::endDictationHold() {
  KeyReport released = {};
  keyboard_.sendReport(&released);
  delay(40);
  const bool wasHeld = dictationHeld_;
  dictationHeld_ = false;
  if (wasHeld) Serial.println("{\"event\":\"dictation_stopped\"}");
  return true;
}

bool UsbVoiceBridge::hostConnected() const {
  return started_ && static_cast<bool>(USB);
}

uint16_t UsbVoiceBridge::writeMicrophone(const uint8_t *data, uint16_t length) {
  if (!started_) return 0;
  const uint16_t monoLength = static_cast<uint16_t>(pcm16StereoLeftToMono(
      data, length, microphoneMonoBuffer_, sizeof(microphoneMonoBuffer_)));
  microphoneBytesAttempted_ += monoLength;
  const uint16_t accepted = microphone_.write(microphoneMonoBuffer_, monoLength);
  microphoneBytesAccepted_ += accepted;
  if (accepted != monoLength) ++microphoneShortWrites_;
  return accepted;
}

uint64_t UsbVoiceBridge::microphoneUsbBytesSent() const {
  return ::microphoneUsbBytesSent.load(std::memory_order_relaxed);
}

uint32_t UsbVoiceBridge::microphoneUsbPacketsSent() const {
  return ::microphoneUsbPacketsSent.load(std::memory_order_relaxed);
}

uint32_t UsbVoiceBridge::microphoneUsbZeroLengthPackets() const {
  return ::microphoneUsbZeroLengthPackets.load(std::memory_order_relaxed);
}

}  // namespace pokepod
