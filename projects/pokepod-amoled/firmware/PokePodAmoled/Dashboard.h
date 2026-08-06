#pragma once

#include <Arduino.h>
#include <Arduino_GFX_Library.h>

#include "BoardServices.h"

namespace pokepod {

class Dashboard {
 public:
  void begin(Arduino_GFX *display);
  void draw(const BoardStatus &status, bool audioReady, bool usbReady,
            bool recording, uint32_t recordingMs, const char *message = nullptr);
  TouchAction actionAt(int16_t x, int16_t y) const;

 private:
  void button(int y, uint16_t color, const char *title, const char *subtitle);
  Arduino_GFX *display_ = nullptr;
  bool hasDrawn_ = false;
  BoardVariant lastVariant_ = BoardVariant::unknown;
  bool lastAudioReady_ = false;
  bool lastUsbReady_ = false;
  bool lastSdReady_ = false;
  bool lastTouchReady_ = false;
  bool lastRtcReady_ = false;
  bool lastImuReady_ = false;
  bool lastPmuReady_ = false;
  int lastBatteryPercent_ = -1000;
  bool lastRecording_ = false;
  uint32_t lastRecordingSecond_ = UINT32_MAX;
  String lastMessage_;
};

}  // namespace pokepod
