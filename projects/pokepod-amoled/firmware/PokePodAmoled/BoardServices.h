#pragma once

#include <Arduino.h>
#include <Arduino_DriveBus_Library.h>
#include "PokePodGfx.h"
#include <Adafruit_XCA9554.h>
#include <SensorPCF85063.hpp>
#include <SensorQMI8658.hpp>
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

#include "BoardConfig.h"

namespace pokepod {

struct BoardStatus {
  BoardVariant variant = BoardVariant::unknown;
  bool ioExpander = false;
  bool display = false;
  bool touch = false;
  bool sdCard = false;
  bool rtc = false;
  bool imu = false;
  bool pmu = false;
  bool charging = false;
  bool vbusPresent = false;
  bool screenOn = true;
  int batteryPercent = -1;
  float imuTemperatureC = NAN;
  float accelerationX = NAN;
  float accelerationY = NAN;
  float accelerationZ = NAN;
};

enum class PowerKeyEvent {
  none,
  shortPress,
  longPress,
};

class BoardServices {
 public:
  static constexpr uint8_t kActiveScreenBrightness = 160;
  static constexpr uint8_t kDimScreenBrightness = 60;
  bool begin(Print &log);
  void refreshSensors();
  bool readTouch(int16_t &x, int16_t &y);
  bool takeTouchInterrupt();
  bool configureScreenOffSensors(bool screenOff, bool raiseToWake, Print &log);
  bool pollMotionWake();
  PowerKeyEvent pollPowerKey();
  void setScreenOn(bool enabled);
  void setScreenBrightness(uint8_t brightness);
  void prepareForDeepSleep(bool keepTouchPowered, Print &log);
  [[noreturn]] void safeShutdown();
  [[noreturn]] void safeShutdown(Print &log);
  void endSdMount();
  String utcNow();
  bool setUtcEpoch(time_t epoch);

  // GPIO21 from the touch controller is the direct low-power wake source.
  // IMU motion is routed through the I/O expander and is polled separately.
  bool lowPowerWakeSourcesReady() const { return status_.touch; }

  Arduino_GFX *display() const { return display_; }
  const BoardStatus &status() const { return status_; }
  bool sdReady() const { return status_.sdCard; }
  uint32_t sdMountGeneration() const {
    return status_.sdCard ? sdMountGeneration_ : 0U;
  }

 private:
  bool beginIoExpander(Print &log);
  BoardVariant detectVariant(Print &log);
  bool beginDisplay(Print &log);
  bool beginTouch(Print &log);
  bool beginSd(Print &log);
  void beginSensors(Print &log);
  void ensureRtcTime(Print &log);
  bool probe(uint8_t address);
  [[noreturn]] void enterShutdownDeepSleepFallback(Print &log);

  BoardStatus status_;
  Adafruit_XCA9554 expander_;
  Arduino_DataBus *displayBus_ = nullptr;
  Arduino_GFX *display_ = nullptr;
  std::shared_ptr<Arduino_IIC_DriveBus> touchBus_;
  Arduino_IIC *touch_ = nullptr;
  SensorPCF85063 rtc_;
  SensorQMI8658 imu_;
  XPowersAXP2101 pmu_;
  bool imuLowPower_ = false;
  int imuInterruptBaseline_ = HIGH;
  uint32_t sdMountGeneration_ = 0;
};

}  // namespace pokepod
