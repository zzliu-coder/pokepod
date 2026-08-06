#include "Dashboard.h"

namespace pokepod {

void Dashboard::begin(Arduino_GFX *display) {
  display_ = display;
  hasDrawn_ = false;
  if (display_ != nullptr) display_->fillScreen(RGB565_BLACK);
}

void Dashboard::draw(const BoardStatus &status, bool audioReady, bool usbReady,
                     bool recording, uint32_t recordingMs, const char *message) {
  if (display_ == nullptr) return;
  const bool firstDraw = !hasDrawn_;
  if (firstDraw) {
    display_->fillScreen(RGB565_BLACK);
    display_->setTextColor(RGB565_WHITE);
    display_->setTextSize(3);
    display_->setCursor(22, 20);
    display_->println("PokePod");
    button(246, RGB565_BLUE, "MAC DICTATION", "BOOT or tap");
  }

  if (firstDraw || status.variant != lastVariant_) {
    display_->fillRect(12, 58, kLcdWidth - 24, 28, RGB565_BLACK);
    display_->setTextSize(2);
    display_->setTextColor(RGB565_CYAN);
    display_->setCursor(22, 62);
    display_->println(variantName(status.variant));
    lastVariant_ = status.variant;
  }

  if (firstDraw || audioReady != lastAudioReady_ || usbReady != lastUsbReady_) {
    display_->fillRect(12, 98, kLcdWidth - 24, 24, RGB565_BLACK);
    display_->setTextColor(RGB565_WHITE);
    display_->setTextSize(2);
    display_->setCursor(22, 102);
    display_->printf("USB MIC/HID  %s", usbReady && audioReady ? "READY" : "CHECK");
    lastAudioReady_ = audioReady;
    lastUsbReady_ = usbReady;
  }

  if (firstDraw || status.sdCard != lastSdReady_ || status.touch != lastTouchReady_) {
    display_->fillRect(12, 126, kLcdWidth - 24, 24, RGB565_BLACK);
    display_->setTextColor(RGB565_WHITE);
    display_->setTextSize(2);
    display_->setCursor(22, 130);
    display_->printf("SD %s  TOUCH %s", status.sdCard ? "OK" : "--", status.touch ? "OK" : "--");
    lastSdReady_ = status.sdCard;
    lastTouchReady_ = status.touch;
  }

  if (firstDraw || status.rtc != lastRtcReady_ || status.imu != lastImuReady_ ||
      status.pmu != lastPmuReady_) {
    display_->fillRect(12, 154, kLcdWidth - 24, 24, RGB565_BLACK);
    display_->setTextColor(RGB565_WHITE);
    display_->setTextSize(2);
    display_->setCursor(22, 158);
    display_->printf("RTC %s IMU %s PMU %s", status.rtc ? "OK" : "--",
                     status.imu ? "OK" : "--", status.pmu ? "OK" : "--");
    lastRtcReady_ = status.rtc;
    lastImuReady_ = status.imu;
    lastPmuReady_ = status.pmu;
  }

  if (firstDraw || status.batteryPercent != lastBatteryPercent_) {
    display_->fillRect(12, 182, kLcdWidth - 24, 24, RGB565_BLACK);
    display_->setTextColor(RGB565_WHITE);
    display_->setTextSize(2);
    display_->setCursor(22, 186);
    if (status.batteryPercent >= 0) display_->printf("BATTERY %d%%", status.batteryPercent);
    lastBatteryPercent_ = status.batteryPercent;
  }

  if (firstDraw || recording != lastRecording_) {
    button(342, recording ? RGB565_RED : RGB565_GREEN,
           recording ? "STOP RECORDING" : "CAPSULE RECORD",
           recording ? "writing WAV to SD" : "save WAV to SD");
    lastRecording_ = recording;
    lastRecordingSecond_ = UINT32_MAX;
  }

  const uint32_t recordingSecond = recordingMs / 1000;
  if (recording && recordingSecond != lastRecordingSecond_) {
    display_->fillRect(kLcdWidth - 86, 348, 70, 18, RGB565_RED);
    display_->setTextColor(RGB565_WHITE);
    display_->setTextSize(1);
    display_->setCursor(kLcdWidth - 82, 352);
    display_->printf("%lu sec", static_cast<unsigned long>(recordingSecond));
    lastRecordingSecond_ = recordingSecond;
  }

  const String visibleMessage = message == nullptr ? String() : String(message);
  if (firstDraw || visibleMessage != lastMessage_) {
    display_->fillRect(12, 216, kLcdWidth - 24, 28, RGB565_BLACK);
    if (!visibleMessage.isEmpty()) {
      display_->setCursor(18, 222);
      display_->setTextColor(RGB565_YELLOW);
      display_->setTextSize(1);
      display_->println(visibleMessage);
    }
    lastMessage_ = visibleMessage;
  }

  hasDrawn_ = true;
}

void Dashboard::button(int y, uint16_t color, const char *title, const char *subtitle) {
  display_->fillRoundRect(16, y, kLcdWidth - 32, 82, 14, color);
  display_->setTextColor(RGB565_WHITE);
  display_->setTextSize(2);
  display_->setCursor(36, y + 18);
  display_->println(title);
  display_->setTextSize(1);
  display_->setCursor(38, y + 52);
  display_->println(subtitle);
}

TouchAction Dashboard::actionAt(int16_t x, int16_t y) const {
  return touchActionAt(x, y);
}

}  // namespace pokepod
