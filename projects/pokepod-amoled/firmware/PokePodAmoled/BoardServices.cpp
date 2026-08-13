#include "BoardServices.h"

#include <SD_MMC.h>
#include <Wire.h>
#include <driver/gpio.h>
#include <esp_attr.h>
#include <esp_sleep.h>
#include <sys/time.h>
#include <time.h>

#include "HardwareSafetyPolicy.h"
#include "TimePolicy.h"

namespace pokepod {
namespace {

volatile bool gTouchInterruptPending = false;

void IRAM_ATTR touchInterrupt() {
  // Keep the ISR independent from Arduino_DriveBus and I2C. The loop copies
  // the pending state into the touch driver's flag before reading the device.
  gTouchInterruptPending = true;
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
  if (pending && touch_ != nullptr) {
    touch_->IIC_Interrupt_Flag = true;
  }
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
    if (!status_.ioExpander) {
      const bool disabled = imu_.disableAccelerometer();
      imuLowPower_ = false;
      log.printf(
          "{\"event\":\"imu_power\",\"ok\":false,\"screen_off\":true,\"wake_on_motion\":true,\"reason\":\"io_expander_unavailable\",\"disabled\":%s}\n",
          disabled ? "true" : "false");
      return false;
    }
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
  if (!canPollMotionWake(status_.imu, imuLowPower_, status_.ioExpander)) {
    return false;
  }
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

void BoardServices::prepareForDeepSleep(bool keepTouchPowered, Print &log) {
  setScreenOn(false);
  (void)configureScreenOffSensors(true, false, log);
  if (status_.ioExpander) {
    // LCD reset and DSI power off. The touch controller remains powered only
    // when its direct GPIO21 interrupt is an armed deep-sleep wake source.
    expander_.digitalWrite(0, LOW);
    expander_.digitalWrite(1, LOW);
    expander_.digitalWrite(2, keepTouchPowered ? HIGH : LOW);
    expander_.digitalWrite(7, HIGH);
  }
  digitalWrite(kSpeakerAmpPin, LOW);
  log.printf("{\"event\":\"board_deep_sleep\",\"touch_powered\":%s}\n",
             keepTouchPowered ? "true" : "false");
}

[[noreturn]] void BoardServices::safeShutdown() {
  safeShutdown(Serial);
}

[[noreturn]] void BoardServices::safeShutdown(Print &log) {
  setScreenOn(false);
  digitalWrite(kSpeakerAmpPin, LOW);
  if (status_.pmu) {
    log.println("{\"event\":\"safe_shutdown\",\"stage\":\"pmu_request\"}");
    log.flush();
    pmu_.shutdown();
    // A successful PMU request removes power. Returning here means firmware
    // still executes, so continue into the deterministic fallback.
    delay(250);
  } else {
    log.println(
        "{\"event\":\"safe_shutdown\",\"stage\":\"pmu_unavailable\"}");
  }
  enterShutdownDeepSleepFallback(log);
}

[[noreturn]] void BoardServices::enterShutdownDeepSleepFallback(Print &log) {
  constexpr uint32_t kBootReleaseWaitMs = 2000;
  const uint32_t startedAt = millis();
  pinMode(kBootButtonPin, INPUT_PULLUP);
  while (digitalRead(kBootButtonPin) == LOW &&
         static_cast<uint32_t>(millis() - startedAt) <
             kBootReleaseWaitMs) {
    delay(10);
  }
  const bool bootLineReleased = digitalRead(kBootButtonPin) == HIGH;
  const ShutdownFallbackAction action = shutdownFallbackAction(
      status_.pmu, true, bootLineReleased);
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  if (action == ShutdownFallbackAction::deepSleepWithBootWake) {
    const uint64_t wakeMask = 1ULL << kBootButtonPin;
    const esp_err_t error = esp_sleep_enable_ext1_wakeup_io(
        wakeMask, ESP_EXT1_WAKEUP_ANY_LOW);
    if (error != ESP_OK) {
      log.printf(
          "{\"event\":\"safe_shutdown\",\"stage\":\"deep_sleep_arm\",\"ok\":false,\"error\":%ld}\n",
          static_cast<long>(error));
    } else {
      log.println(
          "{\"event\":\"safe_shutdown\",\"stage\":\"deep_sleep_fallback\",\"boot_wake\":true}");
    }
  } else {
    log.println(
        "{\"event\":\"safe_shutdown\",\"stage\":\"deep_sleep_fallback\",\"boot_wake\":false,\"reason\":\"boot_line_held\"}");
  }
  log.flush();
  esp_deep_sleep_start();
  while (true) delay(1000);
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
  if (epoch < kMinimumTrustedUtcEpoch) return false;
  timeval value = {.tv_sec = epoch, .tv_usec = 0};
  if (!systemClockUpdateSucceeded(epoch, settimeofday(&value, nullptr))) {
    return false;
  }
  if (!status_.rtc) return true;
  struct tm timeInfo = {};
  if (gmtime_r(&epoch, &timeInfo) == nullptr) return false;
  rtc_.setDateTime(timeInfo);
  const RTC_DateTime verified = rtc_.getDateTime();
  return verified.getYear() == static_cast<uint16_t>(timeInfo.tm_year + 1900) &&
      verified.getMonth() == static_cast<uint8_t>(timeInfo.tm_mon + 1) &&
      verified.getDay() == static_cast<uint8_t>(timeInfo.tm_mday) &&
      verified.getHour() == static_cast<uint8_t>(timeInfo.tm_hour) &&
      verified.getMinute() == static_cast<uint8_t>(timeInfo.tm_min) &&
      verified.getSecond() == static_cast<uint8_t>(timeInfo.tm_sec);
}

void BoardServices::ensureRtcTime(Print &log) {
  if (!status_.rtc) return;
  const RTC_DateTime current = rtc_.getDateTime();
  if (rtc_.isClockIntegrityGuaranteed() && current.getYear() >= 2024 &&
      current.getYear() <= 2099) {
    if (!setSystemClock(current)) {
      log.println(
          "{\"event\":\"rtc_bootstrap\",\"source\":\"rtc\",\"ok\":false,\"stage\":\"system_clock\"}");
    }
    return;
  }
#if defined(POKEPOD_BUILD_EPOCH_UTC)
  static_assert(POKEPOD_BUILD_EPOCH_UTC > 0,
                "POKEPOD_BUILD_EPOCH_UTC must be a positive Unix epoch");
  if (setUtcEpoch(static_cast<time_t>(POKEPOD_BUILD_EPOCH_UTC))) {
    log.println(
        "{\"event\":\"rtc_bootstrap\",\"source\":\"firmware_build_epoch_utc\"}");
  } else {
    log.println(
        "{\"event\":\"rtc_bootstrap\",\"source\":\"firmware_build_epoch_utc\",\"ok\":false}");
  }
#else
  int64_t utcEpoch = 0;
  if (buildLocalDateTimeToUtcEpoch(__DATE__, __TIME__,
                                   kChinaStandardTimeOffsetMinutes,
                                   utcEpoch) &&
      setUtcEpoch(static_cast<time_t>(utcEpoch))) {
    log.println(
        "{\"event\":\"rtc_bootstrap\",\"source\":\"firmware_build_time_local\",\"utc_offset_minutes\":480}");
  } else {
    log.println(
        "{\"event\":\"rtc_bootstrap\",\"source\":\"firmware_build_time_local\",\"ok\":false}");
  }
#endif
}

}  // namespace pokepod
