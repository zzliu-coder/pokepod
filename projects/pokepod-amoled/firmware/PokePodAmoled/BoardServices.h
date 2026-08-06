#pragma once

#include <Arduino.h>
#include <Arduino_DriveBus_Library.h>
#include <Arduino_GFX_Library.h>
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
  int batteryPercent = -1;
  float imuTemperatureC = NAN;
};

class BoardServices {
 public:
  bool begin(Print &log);
  void refreshSensors();
  bool readTouch(int16_t &x, int16_t &y);

  Arduino_GFX *display() const { return display_; }
  const BoardStatus &status() const { return status_; }
  bool sdReady() const { return status_.sdCard; }

 private:
  bool beginIoExpander(Print &log);
  BoardVariant detectVariant(Print &log);
  bool beginDisplay(Print &log);
  bool beginTouch(Print &log);
  bool beginSd(Print &log);
  void beginSensors(Print &log);
  bool probe(uint8_t address);

  BoardStatus status_;
  Adafruit_XCA9554 expander_;
  Arduino_DataBus *displayBus_ = nullptr;
  Arduino_GFX *display_ = nullptr;
  std::shared_ptr<Arduino_IIC_DriveBus> touchBus_;
  Arduino_IIC *touch_ = nullptr;
  SensorPCF85063 rtc_;
  SensorQMI8658 imu_;
  XPowersAXP2101 pmu_;
};

}  // namespace pokepod
