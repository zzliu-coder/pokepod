#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_mac.h>
#include <esp_system.h>

#include "AudioPipeline.h"
#include "BoardConfig.h"
#include "BoardServices.h"
#include "ButtonDebouncer.h"
#include "Dashboard.h"
#include "UsbVoiceBridge.h"
#include "WavRecorder.h"

using namespace pokepod;

namespace {

BoardServices board;
AudioPipeline audio;
UsbVoiceBridge usb;
WavRecorder recorder;
Dashboard dashboard;
ButtonDebouncer bootButton;

uint8_t audioBuffer[kAudioBytesPerChunk];
bool touchLatched = false;
uint32_t lastTouchMs = 0;
uint32_t lastDashboardMs = 0;
uint32_t lastSensorMs = 0;
String command;
String transientMessage;
uint32_t transientUntilMs = 0;

String recordingId() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char value[48];
  snprintf(value, sizeof(value), "%02x%02x%02x%02x%02x%02x-%08lx",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
           static_cast<unsigned long>(esp_random()));
  return String(value);
}

void showMessage(const char *message, uint32_t durationMs = 1800) {
  transientMessage = message;
  transientUntilMs = millis() + durationMs;
}

void drawDashboard() {
  const char *message = deadlinePending(millis(), transientUntilMs)
      ? transientMessage.c_str() : nullptr;
  dashboard.draw(board.status(), audio.ready(), usb.ready(), recorder.recording(),
                 recorder.durationMs(), message);
}

void triggerDictation() {
  const bool sent = usb.sendDictationTrigger();
  showMessage(sent ? "Option-Z sent to macOS" : "USB HID is not ready");
  drawDashboard();
}

void toggleRecording() {
  if (recorder.recording()) {
    const bool ok = recorder.stop(usb.log());
    showMessage(ok ? "WAV committed on SD" : "Recording commit failed");
  } else if (!board.sdReady()) {
    showMessage("Insert a microSD card");
  } else if (!audio.ready()) {
    showMessage("Microphone is not ready");
  } else {
    const bool ok = recorder.start(SD_MMC, usb.log(), recordingId());
    showMessage(ok ? "Recording started" : "Recording start failed");
  }
  drawDashboard();
}

void emitStatus() {
  const BoardStatus &s = board.status();
  usb.log().printf(
      "{\"event\":\"status\",\"variant\":\"%s\",\"display\":%s,\"touch\":%s,\"sd\":%s,\"audio\":%s,\"usb\":%s,\"mic_streaming\":%s,\"mic_open_count\":%lu,\"mic_close_count\":%lu,\"audio_read_bytes\":%llu,\"audio_read_failures\":%lu,\"audio_peak\":%u,\"uac_attempted_bytes\":%llu,\"uac_accepted_bytes\":%llu,\"uac_short_writes\":%lu,\"uac_usb_bytes_sent\":%llu,\"uac_usb_packets_sent\":%lu,\"uac_usb_zero_packets\":%lu,\"recording\":%s,\"duration_ms\":%lu,\"battery\":%d}\n",
      variantName(s.variant), s.display ? "true" : "false", s.touch ? "true" : "false",
      s.sdCard ? "true" : "false", audio.ready() ? "true" : "false",
      usb.ready() ? "true" : "false", usb.microphoneStreaming() ? "true" : "false",
      static_cast<unsigned long>(usb.microphoneOpenCount()),
      static_cast<unsigned long>(usb.microphoneCloseCount()),
      static_cast<unsigned long long>(audio.bytesRead()),
      static_cast<unsigned long>(audio.readFailures()), audio.peakSample(),
      static_cast<unsigned long long>(usb.microphoneBytesAttempted()),
      static_cast<unsigned long long>(usb.microphoneBytesAccepted()),
      static_cast<unsigned long>(usb.microphoneShortWrites()),
      static_cast<unsigned long long>(usb.microphoneUsbBytesSent()),
      static_cast<unsigned long>(usb.microphoneUsbPacketsSent()),
      static_cast<unsigned long>(usb.microphoneUsbZeroLengthPackets()),
      recorder.recording() ? "true" : "false",
      static_cast<unsigned long>(recorder.durationMs()), s.batteryPercent);
}

void handleCommand(const String &line) {
  String normalized = line;
  normalized.trim();
  normalized.toLowerCase();
  if (normalized == "status") emitStatus();
  else if (normalized == "dictate") triggerDictation();
  else if (normalized == "record" && !recorder.recording()) toggleRecording();
  else if (normalized == "stop" && recorder.recording()) toggleRecording();
  else usb.log().println("{\"event\":\"command_error\",\"allowed\":[\"status\",\"dictate\",\"record\",\"stop\"]}");
}

void pollCommands() {
  Stream &stream = usb.stream();
  while (stream.available()) {
    const char c = static_cast<char>(stream.read());
    if (c == '\n' || c == '\r') {
      if (!command.isEmpty()) handleCommand(command);
      command = "";
    } else if (command.length() < 80 && c >= 0x20 && c <= 0x7e) {
      command += c;
    }
  }
}

void pollTouch() {
  int16_t x = 0;
  int16_t y = 0;
  const bool touched = board.readTouch(x, y);
  if (touched && !touchLatched) {
    touchLatched = true;
    switch (dashboard.actionAt(x, y)) {
      case TouchAction::dictation: triggerDictation(); break;
      case TouchAction::recording: toggleRecording(); break;
      default: break;
    }
  } else if (!touched) {
    touchLatched = false;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  pinMode(kBootButtonPin, INPUT_PULLUP);

  board.begin(Serial);
  audio.begin(Serial);
  const bool usbStarted = usb.begin(board.status().variant);
  dashboard.begin(board.display());
  showMessage(usbStarted ? "Tap BOOT for Mac dictation" : "USB startup failed", 3000);
  drawDashboard();
  emitStatus();
}

void loop() {
  const uint32_t now = millis();
  if (bootButton.update(digitalRead(kBootButtonPin) == LOW, now) && bootButton.pressedEdge()) {
    triggerDictation();
  }

  // Keep I2S sampling for health diagnostics, but only feed TinyUSB while the
  // host has selected the microphone streaming alternate interface. TinyUSB
  // clears its IN FIFO whenever that interface closes; writing concurrently
  // with the clear can leave CoreAudio receiving an endless series of ZLPs.
  if (audio.ready() || recorder.recording()) {
    size_t bytes = audio.read(audioBuffer, sizeof(audioBuffer));
    if (bytes > 0) {
      if (usb.microphoneStreaming()) {
        usb.writeMicrophone(audioBuffer, static_cast<uint16_t>(bytes));
      }
      if (recorder.recording()) recorder.append(audioBuffer, bytes, usb.log());
    }
  }

  pollCommands();

  // The ESP32-S3 full-speed USB controller is sensitive to interrupt latency
  // during isochronous microphone transfers. Touch, sensor and display I/O are
  // user-interface work, so defer them while CoreAudio owns the mic stream.
  // Audio capture and CDC diagnostics remain active.
  const bool microphoneStreaming = usb.microphoneStreaming();
  if (!microphoneStreaming && now - lastTouchMs >= 10) {
    lastTouchMs = now;
    pollTouch();
  }

  if (!microphoneStreaming && now - lastSensorMs >= 1000) {
    lastSensorMs = now;
    board.refreshSensors();
  }
  if (!microphoneStreaming && now - lastDashboardMs >= 1000) {
    lastDashboardMs = now;
    drawDashboard();
  }
}
