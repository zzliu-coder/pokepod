#include "UsbLinkBridge.h"

namespace pokepod {

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
  cdc_.enableReboot(true);
  cdc_.begin(115200);
  started_ = USB.begin();
  return started_;
}

bool UsbLinkBridge::hostConnected() const {
  return started_ && static_cast<bool>(USB);
}

}  // namespace pokepod
