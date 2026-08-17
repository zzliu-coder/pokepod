#include "UsbLinkBridge.h"

#include <algorithm>

namespace pokepod {
namespace {

constexpr uint32_t kUsbLinkTxTimeoutMs = 2;
constexpr size_t kUsbLinkWriteSliceBytes = 64;

}  // namespace

UsbLinkBridge *UsbLinkBridge::activeBridge_ = nullptr;

void UsbLinkBridge::cdcEventCallback(void *, esp_event_base_t eventBase,
                                     int32_t eventId, void *eventData) {
  if (activeBridge_ == nullptr ||
      eventBase != ARDUINO_USB_CDC_EVENTS) {
    return;
  }
  if (eventId == ARDUINO_USB_CDC_LINE_STATE_EVENT && eventData != nullptr) {
    const auto *data =
        static_cast<const arduino_usb_cdc_event_data_t *>(eventData);
    activeBridge_->observeLineState(data->line_state.dtr,
                                    data->line_state.rts);
  } else if (eventId == ARDUINO_USB_CDC_DISCONNECTED_EVENT) {
    activeBridge_->observeDisconnected();
  }
}

void UsbLinkBridge::observeLineState(bool dtr, bool rts) {
  dtr_.store(dtr, std::memory_order_release);
  rts_.store(rts, std::memory_order_release);
  cdcSession_.lineState(dtr);
  if (!dtr) consecutiveWouldBlock_.store(0, std::memory_order_release);
}

void UsbLinkBridge::observeDisconnected() {
  dtr_.store(false, std::memory_order_release);
  rts_.store(false, std::memory_order_release);
  consecutiveWouldBlock_.store(0, std::memory_order_release);
  cdcSession_.disconnected();
}

bool UsbLinkBridge::begin(BoardVariant variant) {
  char serial[18];
  const uint64_t mac = ESP.getEfuseMac();
  snprintf(serial, sizeof(serial), "%04X%08X",
           static_cast<unsigned>((mac >> 32) & 0xffff),
           static_cast<unsigned>(mac & 0xffffffff));

  const char *productName = "PokePod";
  if (variant == BoardVariant::v1Sh8601Ft3168) productName = "PokePod V1";
  if (variant == BoardVariant::v2Co5300Cst820) productName = "PokePod V2";
  USB.manufacturerName("PokeCapsule");
  USB.productName(productName);
  USB.serialNumber(serial);
  cdcSession_.reset();
  activeBridge_ = this;
  cdc_.onEvent(ARDUINO_USB_CDC_LINE_STATE_EVENT,
               UsbLinkBridge::cdcEventCallback);
  cdc_.onEvent(ARDUINO_USB_CDC_DISCONNECTED_EVENT,
               UsbLinkBridge::cdcEventCallback);
  cdc_.enableReboot(true);
  // Link polling has a 2 ms wall-clock budget. Keep Arduino-ESP32's guarded
  // USBCDC write below that budget while still letting write() flush a full
  // TinyUSB FIFO. The framework default is 250 ms.
  cdc_.setTxTimeoutMs(kUsbLinkTxTimeoutMs);
  cdc_.begin(115200);
  started_ = USB.begin();
  return started_;
}

bool UsbLinkBridge::hostConnected() const {
  return started_ && static_cast<bool>(USB);
}

void UsbLinkBridge::discardHostSessionBuffers() {
  // Drain only Arduino-ESP32's public FreeRTOS RX queue. Calling TinyUSB FIFO
  // primitives from the main task races the USB task and can wedge the next
  // CDC session. The host protocol waits after DTR open, while the main loop
  // drains a closed session before accepting the next generation.
  while (cdc_.available() > 0) {
    (void)cdc_.read();
  }
}

LinkWriteAttempt UsbLinkBridge::writeSome(const uint8_t *data, size_t size) {
  writeAttempts_.fetch_add(1, std::memory_order_relaxed);
  if (!started_ || !hostSessionActive()) {
    writeDisconnected_.fetch_add(1, std::memory_order_relaxed);
    consecutiveWouldBlock_.store(0, std::memory_order_release);
    return LinkWriteAttempt{LinkWriteDisposition::disconnected, 0};
  }
  if (data == nullptr || size == 0) {
    return LinkWriteAttempt{LinkWriteDisposition::failed, 0};
  }

  // USBCDC::write() owns the framework tx_lock and explicitly flushes TinyUSB
  // when its FIFO is full. Calling availableForWrite() first can permanently
  // starve that flush path when the FIFO reports zero bytes available.
  const size_t wanted = std::min(size, kUsbLinkWriteSliceBytes);
  const size_t written = cdc_.write(data, wanted);
  lastWriteBytes_.store(static_cast<uint32_t>(written),
                        std::memory_order_release);
  if (written != 0) {
    writeProgress_.fetch_add(1, std::memory_order_relaxed);
    consecutiveWouldBlock_.store(0, std::memory_order_release);
    return LinkWriteAttempt{LinkWriteDisposition::progress, written};
  }
  if (!hostSessionActive()) {
    writeDisconnected_.fetch_add(1, std::memory_order_relaxed);
    consecutiveWouldBlock_.store(0, std::memory_order_release);
    return LinkWriteAttempt{LinkWriteDisposition::disconnected, 0};
  }
  writeWouldBlock_.fetch_add(1, std::memory_order_relaxed);
  consecutiveWouldBlock_.fetch_add(1, std::memory_order_relaxed);
  return LinkWriteAttempt{LinkWriteDisposition::wouldBlock, 0};
}

UsbLinkTransportSnapshot UsbLinkBridge::transportSnapshot() const {
  UsbLinkTransportSnapshot result;
  result.writeAttempts = writeAttempts_.load(std::memory_order_acquire);
  result.writeProgress = writeProgress_.load(std::memory_order_acquire);
  result.writeWouldBlock = writeWouldBlock_.load(std::memory_order_acquire);
  result.writeDisconnected =
      writeDisconnected_.load(std::memory_order_acquire);
  result.consecutiveWouldBlock =
      consecutiveWouldBlock_.load(std::memory_order_acquire);
  result.lastWriteBytes = lastWriteBytes_.load(std::memory_order_acquire);
  result.dtr = dtr_.load(std::memory_order_acquire);
  result.rts = rts_.load(std::memory_order_acquire);
  return result;
}

}  // namespace pokepod
