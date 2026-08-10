#include "UsbLinkBridge.h"

#include "esp32-hal-tinyusb.h"

namespace pokepod {
namespace {

UsbCdcSessionState *activeCdcSession = nullptr;

void cdcEventCallback(void *, esp_event_base_t eventBase, int32_t eventId,
                      void *eventData) {
  if (activeCdcSession == nullptr ||
      eventBase != ARDUINO_USB_CDC_EVENTS) {
    return;
  }
  if (eventId == ARDUINO_USB_CDC_LINE_STATE_EVENT && eventData != nullptr) {
    const auto *data =
        static_cast<const arduino_usb_cdc_event_data_t *>(eventData);
    activeCdcSession->lineState(data->line_state.dtr);
  } else if (eventId == ARDUINO_USB_CDC_DISCONNECTED_EVENT) {
    activeCdcSession->disconnected();
  }
}

}  // namespace

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
  activeCdcSession = &cdcSession_;
  cdc_.onEvent(ARDUINO_USB_CDC_LINE_STATE_EVENT, cdcEventCallback);
  cdc_.onEvent(ARDUINO_USB_CDC_DISCONNECTED_EVENT, cdcEventCallback);
  cdc_.enableReboot(true);
  cdc_.begin(115200);
  started_ = USB.begin();
  return started_;
}

bool UsbLinkBridge::hostConnected() const {
  return started_ && static_cast<bool>(USB);
}

void UsbLinkBridge::discardHostSessionBuffers() {
  // USBCDC first copies TinyUSB RX packets into its own FreeRTOS queue. Drain
  // that public Stream queue as well as TinyUSB's lower FIFO so a complete old
  // request cannot be parsed after the next process opens the same device.
  while (cdc_.available() > 0) {
    (void)cdc_.read();
  }
  if (hostConnected()) {
    tud_cdc_read_flush();
    (void)tud_cdc_write_clear();
  }
}

}  // namespace pokepod
