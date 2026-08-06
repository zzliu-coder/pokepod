#include "BoardServices.h"

#include <SD_MMC.h>
#include <Wire.h>

namespace pokepod {
namespace {

Arduino_IIC *gTouch = nullptr;

void touchInterrupt() {
  if (gTouch != nullptr) {
    gTouch->IIC_Interrupt_Flag = true;
  }
}

}  // namespace

bool BoardServices::begin(Print &log) {
  Wire.begin(kI2cSda, kI2cScl, 400000);
  status_.ioExpander = beginIoExpander(log);
  status_.variant = detectVariant(log);
  status_.display = beginDisplay(log);
  status_.touch = beginTouch(log);
  status_.sdCard = beginSd(log);
  beginSensors(log);

  log.printf("{\"event\":\"board_ready\",\"variant\":\"%s\",\"display\":%s,\"touch\":%s,\"sd\":%s,\"rtc\":%s,\"imu\":%s,\"pmu\":%s}\n",
             variantName(status_.variant), status_.display ? "true" : "false",
             status_.touch ? "true" : "false", status_.sdCard ? "true" : "false",
             status_.rtc ? "true" : "false", status_.imu ? "true" : "false",
             status_.pmu ? "true" : "false");
  return status_.variant != BoardVariant::unknown && status_.display;
}

bool BoardServices::beginIoExpander(Print &log) {
  if (!expander_.begin(kIoExpanderAddress, &Wire)) {
    log.println("{\"event\":\"io_expander\",\"ok\":false}");
    return false;
  }

  for (uint8_t pin : {uint8_t(0), uint8_t(1), uint8_t(2), uint8_t(7)}) {
    expander_.pinMode(pin, OUTPUT);
  }
  expander_.digitalWrite(0, LOW);
  expander_.digitalWrite(1, LOW);
  expander_.digitalWrite(2, LOW);
  expander_.digitalWrite(7, HIGH);
  delay(20);
  expander_.digitalWrite(0, HIGH);
  expander_.digitalWrite(1, HIGH);
  expander_.digitalWrite(2, HIGH);
  delay(150);
  log.println("{\"event\":\"io_expander\",\"ok\":true}");
  return true;
}

BoardVariant BoardServices::detectVariant(Print &log) {
  const bool v2 = probe(kTouchV2Address);
  const bool v1 = probe(kTouchV1Address);
  const BoardVariant variant = boardVariantFromTouchProbes(v1, v2);
  log.printf("{\"event\":\"variant_probe\",\"i2c_0x15\":%s,\"i2c_0x38\":%s,\"variant\":\"%s\"}\n",
             v2 ? "true" : "false", v1 ? "true" : "false", variantName(variant));
  return variant;
}

bool BoardServices::probe(uint8_t address) {
  Wire.beginTransmission(address);
  return Wire.endTransmission() == 0;
}

bool BoardServices::beginDisplay(Print &log) {
  displayBus_ = new Arduino_ESP32QSPI(
      kLcdChipSelect, kLcdClock, kLcdData0, kLcdData1, kLcdData2, kLcdData3);
  if (displayBus_ == nullptr) return false;

  switch (status_.variant) {
    case BoardVariant::v1Sh8601Ft3168:
      display_ = new Arduino_SH8601(
          displayBus_, GFX_NOT_DEFINED, 0, kLcdWidth, kLcdHeight);
      break;
    case BoardVariant::v2Co5300Cst820:
      display_ = new Arduino_CO5300(
          displayBus_, GFX_NOT_DEFINED, 0, kLcdWidth, kLcdHeight, 16, 0, 0, 0);
      break;
    default:
      log.println("{\"event\":\"display\",\"ok\":false,\"reason\":\"unknown_variant\"}");
      return false;
  }

  const bool ok = display_ != nullptr && display_->begin();
  if (ok) {
    if (status_.variant == BoardVariant::v1Sh8601Ft3168) {
      static_cast<Arduino_SH8601 *>(display_)->setBrightness(180);
    } else {
      static_cast<Arduino_CO5300 *>(display_)->setBrightness(180);
    }
    display_->fillScreen(RGB565_BLACK);
  }
  log.printf("{\"event\":\"display\",\"ok\":%s}\n", ok ? "true" : "false");
  return ok;
}

bool BoardServices::beginTouch(Print &log) {
  touchBus_ = std::make_shared<Arduino_HWIIC>(kI2cSda, kI2cScl, &Wire);
  if (!touchBus_) return false;

  switch (status_.variant) {
    case BoardVariant::v1Sh8601Ft3168:
      touch_ = new Arduino_FT3x68(touchBus_, FT3168_DEVICE_ADDRESS,
                                  DRIVEBUS_DEFAULT_VALUE, kTouchInterruptPin, touchInterrupt);
      break;
    case BoardVariant::v2Co5300Cst820:
      touch_ = new Arduino_CST816x(touchBus_, CST816T_DEVICE_ADDRESS,
                                   DRIVEBUS_DEFAULT_VALUE, kTouchInterruptPin, touchInterrupt);
      break;
    default:
      return false;
  }
  gTouch = touch_;
  const bool ok = touch_ != nullptr && touch_->begin();
  log.printf("{\"event\":\"touch\",\"ok\":%s}\n", ok ? "true" : "false");
  return ok;
}

bool BoardServices::beginSd(Print &log) {
  SD_MMC.setPins(kSdClock, kSdCommand, kSdData0);
  const bool ok = SD_MMC.begin("/sdcard", true);
  log.printf("{\"event\":\"sd\",\"ok\":%s,\"size_mb\":%llu}\n",
             ok ? "true" : "false",
             ok ? static_cast<unsigned long long>(SD_MMC.cardSize() / (1024ULL * 1024ULL)) : 0ULL);
  return ok;
}

void BoardServices::beginSensors(Print &log) {
  status_.rtc = rtc_.begin(Wire, kI2cSda, kI2cScl);
  status_.imu = imu_.begin(Wire, QMI8658_L_SLAVE_ADDRESS, kI2cSda, kI2cScl);
  status_.pmu = pmu_.begin(Wire, AXP2101_SLAVE_ADDRESS, kI2cSda, kI2cScl);
  if (status_.pmu) {
    pmu_.enableBattDetection();
    pmu_.enableBattVoltageMeasure();
    pmu_.disableTSPinMeasure();
  }
  refreshSensors();
  log.printf("{\"event\":\"sensors\",\"rtc\":%s,\"imu\":%s,\"pmu\":%s,\"battery\":%d}\n",
             status_.rtc ? "true" : "false", status_.imu ? "true" : "false",
             status_.pmu ? "true" : "false", status_.batteryPercent);
}

void BoardServices::refreshSensors() {
  if (status_.pmu) status_.batteryPercent = pmu_.getBatteryPercent();
  if (status_.imu) status_.imuTemperatureC = imu_.getTemperature_C();
}

bool BoardServices::readTouch(int16_t &x, int16_t &y) {
  if (!status_.touch || touch_ == nullptr) return false;
  const int32_t fingers = touch_->IIC_Read_Device_Value(
      touch_->Arduino_IIC_Touch::Value_Information::TOUCH_FINGER_NUMBER);
  if (fingers <= 0) return false;
  x = static_cast<int16_t>(touch_->IIC_Read_Device_Value(
      touch_->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_X));
  y = static_cast<int16_t>(touch_->IIC_Read_Device_Value(
      touch_->Arduino_IIC_Touch::Value_Information::TOUCH_COORDINATE_Y));
  return x >= 0 && x < kLcdWidth && y >= 0 && y < kLcdHeight;
}

}  // namespace pokepod
