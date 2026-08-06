#pragma once

#include <Arduino.h>

#include "FirmwarePolicy.h"

namespace pokepod {

constexpr int kBootButtonPin = 0;
constexpr int kI2cSda = 15;
constexpr int kI2cScl = 14;
constexpr int kTouchInterruptPin = 21;

constexpr int kLcdData0 = 4;
constexpr int kLcdData1 = 5;
constexpr int kLcdData2 = 6;
constexpr int kLcdData3 = 7;
constexpr int kLcdClock = 11;
constexpr int kLcdChipSelect = 12;
constexpr int kLcdWidth = kDisplayWidth;
constexpr int kLcdHeight = kDisplayHeight;

constexpr int kI2sMclk = 16;
constexpr int kI2sBclk = 9;
constexpr int kI2sDataIn = 10;
constexpr int kI2sDataOut = 8;
constexpr int kI2sWordSelect = 45;
constexpr int kSpeakerAmpPin = 46;

constexpr int kSdClock = 2;
constexpr int kSdCommand = 1;
constexpr int kSdData0 = 3;

constexpr uint8_t kIoExpanderAddress = 0x20;
constexpr uint8_t kTouchV2Address = 0x15;
constexpr uint8_t kTouchV1Address = 0x38;

constexpr uint32_t kMaxRecordingMs = 60000;

}  // namespace pokepod
