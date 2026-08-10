#include "BoardServices.h"

#include <SD_MMC.h>
#include <Wire.h>
#include <sys/time.h>
#include <time.h>

namespace pokepod {
namespace {

Arduino_IIC *gTouch = nullptr;
volatile bool gTouchInterruptPending = false;

void touchInterrupt() {
  gTouchInterruptPending = true;
  if (gTouch != nullptr) {
    gTouch->IIC_Interrupt_Flag = true;
  }
}

bool setSystemClock(const RTC_DateTime &value) {
  if (value.getYear() < 2024 || value.getYear() > 2099) return false;
  struct tm timeInfo = {};
  timeInfo.tm_year = value.getYear() - 1900;
  timeInfo.tm_mon = value.getMonth() - 1;
  timeInfo.tm_mday = value.getDay();
  timeInfo.tm_hour = value.getHour();
  timeInfo.tm_min = value.getMinute();
  timeInfo.tm_sec = value.getSecond();
  setenv("TZ", "UTC0", 1);
  tzset();
  const time_t epoch = mktime(&timeInfo);
  if (epoch < 1704067200) return false;
  timeval systemTime = {.tv_sec = epoch, .tv_usec = 0};
  return settimeofday(&systemTime, nullptr) == 0;
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
  expander_.pinMode(6, INPUT);
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
      static_cast<Arduino_SH8601 *>(display_)->setBrightness(
          kActiveScreenBrightness);
    } else {
      static_cast<Arduino_CO5300 *>(display_)->setBrightness(
          kActiveScreenBrightness);
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
    pmu_.enableVbusVoltageMeasure();
    pmu_.disableTSPinMeasure();
    pmu_.disableLongPressShutdown();
    pmu_.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu_.clearIrqStatus();
    pmu_.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ |
                   XPOWERS_AXP2101_PKEY_LONG_IRQ);
  }
  if (status_.imu) {
    imu_.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                             SensorQMI8658::ACC_ODR_125Hz,
                             SensorQMI8658::LPF_MODE_0);
    imu_.enableAccelerometer();
  }
  ensureRtcTime(log);
  refreshSensors();
  log.printf("{\"event\":\"sensors\",\"rtc\":%s,\"imu\":%s,\"pmu\":%s,\"battery\":%d,\"charging\":%s,\"vbus\":%s}\n",
             status_.rtc ? "true" : "false", status_.imu ? "true" : "false",
             status_.pmu ? "true" : "false", status_.batteryPercent,
             status_.charging ? "true" : "false",
             status_.vbusPresent ? "true" : "false");
}

void BoardServices::refreshSensors() {
  if (status_.pmu) {
    status_.batteryPercent = pmu_.getBatteryPercent();
    status_.charging = pmu_.isCharging();
    status_.vbusPresent = pmu_.isVbusIn();
  }
  if (status_.imu && !imuLowPower_) {
    status_.imuTemperatureC = imu_.getTemperature_C();
    imu_.getAccelerometer(status_.accelerationX, status_.accelerationY,
                          status_.accelerationZ);
  }
}

bool BoardServices::takeTouchInterrupt() {
  noInterrupts();
  const bool pending = gTouchInterruptPending;
  gTouchInterruptPending = false;
  interrupts();
  return pending;
}

bool BoardServices::configureScreenOffSensors(bool screenOff,
                                              bool raiseToWake,
                                              Print &log) {
  if (!status_.imu) return false;
  bool ok = true;
  if (!screenOff) {
    imu_.disableAccelerometer();
    ok = imu_.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                                  SensorQMI8658::ACC_ODR_125Hz,
                                  SensorQMI8658::LPF_MODE_0) == 0 &&
        imu_.enableAccelerometer();
    imuLowPower_ = false;
  } else if (raiseToWake) {
    ok = imu_.configWakeOnMotion(
        200, SensorQMI8658::ACC_ODR_LOWPOWER_21Hz,
        SensorQMI8658::INTERRUPT_PIN_1, 1, 0x08) == 0;
    imuLowPower_ = ok;
    imuInterruptBaseline_ = expander_.digitalRead(6);
  } else {
    ok = imu_.disableAccelerometer();
    imuLowPower_ = ok;
  }
  log.printf("{\"event\":\"imu_power\",\"ok\":%s,\"screen_off\":%s,\"wake_on_motion\":%s}\n",
             ok ? "true" : "false", screenOff ? "true" : "false",
             screenOff && raiseToWake ? "true" : "false");
  return ok;
}

bool BoardServices::pollMotionWake() {
  if (!status_.imu || !imuLowPower_) return false;
  const int level = expander_.digitalRead(6);
  if (level == imuInterruptBaseline_) return false;
  imu_.getIrqStatus();
  imuInterruptBaseline_ = level;
  return true;
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

PowerKeyEvent BoardServices::pollPowerKey() {
  if (!status_.pmu) return PowerKeyEvent::none;
  pmu_.getIrqStatus();
  PowerKeyEvent result = PowerKeyEvent::none;
  if (pmu_.isPekeyLongPressIrq()) result = PowerKeyEvent::longPress;
  else if (pmu_.isPekeyShortPressIrq()) result = PowerKeyEvent::shortPress;
  if (result != PowerKeyEvent::none) pmu_.clearIrqStatus();
  return result;
}

void BoardServices::setScreenOn(bool enabled) {
  if (!status_.display || status_.screenOn == enabled) return;
  if (enabled) display_->displayOn();
  setScreenBrightness(enabled ? kActiveScreenBrightness : 0);
  if (!enabled) display_->displayOff();
  status_.screenOn = enabled;
}

void BoardServices::setScreenBrightness(uint8_t brightness) {
  if (!status_.display) return;
  if (status_.variant == BoardVariant::v1Sh8601Ft3168) {
    static_cast<Arduino_SH8601 *>(display_)->setBrightness(brightness);
  } else if (status_.variant == BoardVariant::v2Co5300Cst820) {
    static_cast<Arduino_CO5300 *>(display_)->setBrightness(brightness);
  }
}

void BoardServices::safeShutdown() {
  setScreenOn(false);
  if (status_.pmu) pmu_.shutdown();
}

String BoardServices::utcNow() {
  struct tm timeInfo = {};
  const time_t systemTime = time(nullptr);
  if (systemTime >= 1704067200 && gmtime_r(&systemTime, &timeInfo) != nullptr) {
    char value[24];
    strftime(value, sizeof(value), "%Y-%m-%dT%H:%M:%SZ", &timeInfo);
    return String(value);
  }
  if (!status_.rtc || !rtc_.isClockIntegrityGuaranteed()) return String();
  const RTC_DateTime current = rtc_.getDateTime();
  if (current.getYear() < 2024 || current.getYear() > 2099 ||
      current.getMonth() < 1 || current.getMonth() > 12 ||
      current.getDay() < 1 || current.getDay() > 31) {
    return String();
  }
  char value[24];
  snprintf(value, sizeof(value), "%04u-%02u-%02uT%02u:%02u:%02uZ",
           current.getYear(), current.getMonth(), current.getDay(),
           current.getHour(), current.getMinute(), current.getSecond());
  return String(value);
}

bool BoardServices::setUtcEpoch(time_t epoch) {
  if (epoch < 1704067200) return false;
  timeval value = {.tv_sec = epoch, .tv_usec = 0};
  settimeofday(&value, nullptr);
  if (!status_.rtc) return true;
  struct tm timeInfo = {};
  if (gmtime_r(&epoch, &timeInfo) == nullptr) return false;
  rtc_.setDateTime(timeInfo);
  return true;
}

void BoardServices::ensureRtcTime(Print &log) {
  if (!status_.rtc) return;
  const RTC_DateTime current = rtc_.getDateTime();
  if (rtc_.isClockIntegrityGuaranteed() && current.getYear() >= 2024 &&
      current.getYear() <= 2099) {
    setSystemClock(current);
    return;
  }
  static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char *match = strstr(months, String(__DATE__).substring(0, 3).c_str());
  const uint8_t month = match == nullptr ? 1 :
      static_cast<uint8_t>((match - months) / 3 + 1);
  const uint8_t day = static_cast<uint8_t>(atoi(__DATE__ + 4));
  const uint16_t year = static_cast<uint16_t>(atoi(__DATE__ + 7));
  const uint8_t hour = static_cast<uint8_t>(atoi(__TIME__));
  const uint8_t minute = static_cast<uint8_t>(atoi(__TIME__ + 3));
  const uint8_t second = static_cast<uint8_t>(atoi(__TIME__ + 6));
  rtc_.setDateTime(year, month, day, hour, minute, second);
  setSystemClock(RTC_DateTime(year, month, day, hour, minute, second));
  log.println("{\"event\":\"rtc_bootstrap\",\"source\":\"firmware_build_time\"}");
}

}  // namespace pokepod
