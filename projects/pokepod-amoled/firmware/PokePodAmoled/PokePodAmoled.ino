#include <Arduino.h>
#include <SD_MMC.h>
#include <esp_mac.h>
#include <esp_system.h>

#include "AudioPipeline.h"
#include "BoardConfig.h"
#include "BoardServices.h"
#include "ButtonDebouncer.h"
#include "ButtonPolicy.h"
#include "CapsuleLibrary.h"
#include "CapsulePolicy.h"
#include "Dashboard.h"
#include "DeviceConfig.h"
#include "ProvisioningPortal.h"
#include "PokePodLinkService.h"
#include "RaiseToWakePolicy.h"
#include "TencentWorker.h"
#include "UsbVoiceBridge.h"
#include "WavRecorder.h"
#include "WifiController.h"

using namespace pokepod;

namespace {

BoardServices board;
AudioPipeline audio;
UsbVoiceBridge usb;
WavRecorder recorder;
CapsuleLibrary capsuleLibrary;
Dashboard dashboard;
ButtonDebouncer bootButton;
DeviceConfig deviceConfig;
WifiController wifi;
TencentWorker tencentWorker;
ProvisioningPortal provisioningPortal;
PokePodLinkService linkService;
RaiseToWakePolicy raiseToWake;

uint8_t audioBuffer[kAudioBytesPerChunk];
bool touchLatched = false;
bool touchDictationHolding = false;
bool bootDictationHolding = false;
bool dictationUiActive = false;
int16_t touchStartX = 0;
int16_t touchStartY = 0;
int16_t touchLastX = 0;
int16_t touchLastY = 0;
uint32_t lastTouchMs = 0;
uint32_t lastDashboardMs = 0;
uint32_t lastSensorMs = 0;
uint32_t bootPressedAtMs = 0;
bool rtcSyncedFromNetwork = false;
String transientMessage;
uint32_t transientUntilMs = 0;

String recordingId() {
  uint8_t randomBytes[16];
  for (size_t offset = 0; offset < sizeof(randomBytes); offset += 4) {
    const uint32_t random = esp_random();
    memcpy(randomBytes + offset, &random, sizeof(random));
  }
  char value[37];
  formatUuidV4(randomBytes, value);
  return String(value);
}

void showMessage(const char *message, uint32_t durationMs = 1800) {
  transientMessage = message;
  transientUntilMs = millis() + durationMs;
}

void drawDashboard() {
  DashboardView view;
  view.board = &board.status();
  view.library = &capsuleLibrary;
  view.settings = &deviceConfig.settings();
  view.audioReady = audio.ready();
  view.usbReady = usb.ready();
  view.hostConnected = usb.hostConnected();
  view.dictationHolding = dictationUiActive;
  view.recording = recorder.recording();
  view.transcribing = tencentWorker.working();
  view.playing = audio.playing();
  view.provisioning = provisioningPortal.active();
  view.recordingMs = recorder.durationMs();
  view.audioPeak = audio.peakSample();
  view.wifiPhase = provisioningPortal.active()
      ? WifiPhase::provisioning : wifi.phase();
  view.wifiRssi = wifi.rssi();
  view.portalSsid = provisioningPortal.ssid();
  view.portalPassword = provisioningPortal.password();
  if (deadlinePending(millis(), transientUntilMs)) view.message = transientMessage;
  dashboard.draw(view);
}

bool startDictationHold() {
  if (!usb.hostConnected()) {
    showMessage("USB 键盘尚未就绪");
    drawDashboard();
    return false;
  }
  dictationUiActive = true;
  showMessage("按住说话，松开结束", 60000);
  drawDashboard();
  const bool sent = usb.beginDictationHold();
  if (!sent) {
    dictationUiActive = false;
    showMessage("USB 键盘尚未就绪");
    drawDashboard();
  }
  return sent;
}

bool stopDictationHold() {
  const bool sent = usb.endDictationHold();
  dictationUiActive = false;
  showMessage("微信语音输入已结束");
  drawDashboard();
  return sent;
}

void toggleRecording() {
  if (recorder.recording()) {
    const bool ok = recorder.stop(usb.log());
    if (ok) capsuleLibrary.scan();
    showMessage(ok ? "胶囊已进入转写队列" : "录音提交失败");
  } else if (!board.sdReady()) {
    showMessage("请插入 microSD 卡");
  } else if (!audio.ready()) {
    showMessage("麦克风尚未就绪");
  } else {
    if (audio.playing()) audio.stopPlayback(usb.log());
    tencentWorker.wake();
    const bool ok = recorder.start(usb.log(), recordingId(), board.utcNow());
    showMessage(ok ? "开始录音" : "录音启动失败");
  }
  drawDashboard();
}

void emitStatus() {
  const BoardStatus &s = board.status();
  usb.log().printf(
      "{\"event\":\"status\",\"variant\":\"%s\",\"display\":%s,\"touch\":%s,\"sd\":%s,\"audio\":%s,\"usb\":%s,\"host_connected\":%s,\"mic_streaming\":%s,\"mic_open_count\":%lu,\"mic_close_count\":%lu,\"audio_read_bytes\":%llu,\"audio_read_failures\":%lu,\"audio_peak\":%u,\"uac_attempted_bytes\":%llu,\"uac_accepted_bytes\":%llu,\"uac_short_writes\":%lu,\"uac_usb_bytes_sent\":%llu,\"uac_usb_packets_sent\":%lu,\"uac_usb_zero_packets\":%lu,\"recording\":%s,\"duration_ms\":%lu,\"battery\":%d,\"charging\":%s,\"vbus\":%s,\"wifi\":\"%s\",\"wifi_rssi\":%ld,\"pending_capsules\":%u,\"tencent_configured\":%s,\"transcribing\":%s}\n",
      variantName(s.variant), s.display ? "true" : "false", s.touch ? "true" : "false",
      s.sdCard ? "true" : "false", audio.ready() ? "true" : "false",
      usb.ready() ? "true" : "false", usb.hostConnected() ? "true" : "false",
      usb.microphoneStreaming() ? "true" : "false",
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
      static_cast<unsigned long>(recorder.durationMs()), s.batteryPercent,
      s.charging ? "true" : "false", s.vbusPresent ? "true" : "false",
      wifi.phaseName(), static_cast<long>(wifi.rssi()),
      static_cast<unsigned>(capsuleLibrary.pendingCount()),
      deviceConfig.hasTencent() ? "true" : "false",
      tencentWorker.working() ? "true" : "false");
}

void pollTouch() {
  int16_t x = 0;
  int16_t y = 0;
  const bool touched = board.readTouch(x, y);
  if (touched && !touchLatched) {
    touchLatched = true;
    touchStartX = touchLastX = x;
    touchStartY = touchLastY = y;
    if (dashboard.actionAt(x, y, usb.hostConnected()) ==
        UiAction::wechatDictation) {
      touchDictationHolding = startDictationHold();
    }
  } else if (touched) {
    touchLastX = x;
    touchLastY = y;
  } else if (!touched) {
    if (!touchLatched) return;
    touchLatched = false;
    if (touchDictationHolding) {
      touchDictationHolding = false;
      stopDictationHold();
      return;
    }
    const int16_t deltaX = touchLastX - touchStartX;
    const int16_t deltaY = touchLastY - touchStartY;
    if (abs(deltaX) >= 60 && abs(deltaX) > abs(deltaY)) {
      dashboard.swipeHorizontal(deltaX, recorder.recording());
      drawDashboard();
      return;
    }
    if (abs(deltaY) >= 45 && abs(deltaY) > abs(deltaX)) {
      dashboard.swipeVertical(deltaY, capsuleLibrary);
      drawDashboard();
      return;
    }
    const UiAction action = dashboard.actionAt(
        touchStartX, touchStartY, usb.hostConnected());
    if (action == UiAction::capsuleRecord) toggleRecording();
    else if (action == UiAction::wechatDictation) return;
    else if (action == UiAction::openCapsule) {
      dashboard.openCapsuleAt(touchStartY, capsuleLibrary);
      drawDashboard();
    } else if (action == UiAction::back) {
      dashboard.back();
      drawDashboard();
    } else if (action == UiAction::wifiToggle) {
      const bool enabled = !deviceConfig.settings().wifiEnabled;
      if (deviceConfig.setWifiEnabled(enabled, usb.log())) {
        wifi.configurationChanged();
        if (enabled) tencentWorker.wake();
        showMessage(enabled ? "Wi-Fi 已开启" : "Wi-Fi 已关闭");
      }
      dashboard.invalidate();
      drawDashboard();
    } else if (action == UiAction::openProvisioning) {
      if (provisioningPortal.begin(deviceConfig, usb.log())) {
        showMessage("手机连接屏幕上的热点");
      } else {
        showMessage("配网热点启动失败");
      }
      dashboard.invalidate();
      drawDashboard();
    } else if (action == UiAction::raiseToWakeToggle) {
      const bool enabled = !deviceConfig.settings().raiseToWake;
      if (deviceConfig.setRaiseToWake(enabled, usb.log())) {
        showMessage(enabled ? "抬起亮屏已开启" : "抬起亮屏已关闭");
      }
      dashboard.invalidate();
      drawDashboard();
    } else {
      const CapsuleSummary *selected = dashboard.selected(capsuleLibrary);
      if (selected == nullptr) return;
      const String id = selected->id;
      if (action == UiAction::favorite) {
        capsuleLibrary.toggleFavorite(id);
        dashboard.invalidate();
      } else if (action == UiAction::archive) {
        capsuleLibrary.archive(id);
        dashboard.back();
      } else if (action == UiAction::retry) {
        capsuleLibrary.requeue(id);
        tencentWorker.wake();
        showMessage("已重新加入转写队列");
      } else if (action == UiAction::play) {
        if (audio.playing()) {
          audio.stopPlayback(usb.log());
          showMessage("已停止播放");
        } else if (usb.microphoneStreaming() || recorder.recording()) {
          showMessage("麦克风使用中，暂时无法播放");
        } else if (!safeCapsuleFileName(selected->audioFile.c_str()) ||
                   !audio.startPlayback(
                       SD_MMC, selected->directory + "/" + selected->audioFile,
                       usb.log())) {
          showMessage("音频播放失败");
        } else {
          showMessage("正在播放");
        }
      }
      drawDashboard();
    }
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
  deviceConfig.begin(usb.log());
  if (board.sdReady() && recorder.begin(SD_MMC, usb.log())) {
    recorder.recoverInterrupted(usb.log(), board.utcNow());
    capsuleLibrary.begin(SD_MMC, usb.log());
    tencentWorker.begin(SD_MMC, capsuleLibrary, deviceConfig, usb.log());
  }
  wifi.begin(deviceConfig, usb.log());
  linkService.begin(usb.stream(), SD_MMC, board, audio, usb, dashboard,
                    capsuleLibrary, recorder,
                    deviceConfig, wifi, tencentWorker,
                    startDictationHold, stopDictationHold, usb.log());
  dashboard.begin(board.display(), board.sdReady() ? &SD_MMC : nullptr);
  showMessage(usbStarted ? "BOOT 可录胶囊；连接 Mac 后可语音输入"
                         : "USB 启动失败",
              3000);
  drawDashboard();
  emitStatus();
}

void loop() {
  const uint32_t now = millis();

  // A CDC upload can otherwise overrun TinyUSB while a full-screen AMOLED
  // redraw or an SD/network task owns the main loop. Once a binary request has
  // started, drain it before doing any optional UI or sensor work.
  if (linkService.receivingBinary()) {
    linkService.poll(now);
    return;
  }
  if (bootButton.update(digitalRead(kBootButtonPin) == LOW, now)) {
    if (bootButton.pressedEdge()) {
      bootPressedAtMs = now;
      if (bootPressStartsDictation(usb.hostConnected())) {
        bootDictationHolding = startDictationHold();
      }
    } else if (bootButton.releasedEdge()) {
      if (bootDictationHolding) {
        bootDictationHolding = false;
        stopDictationHold();
        return;
      }
      const BootGestureAction action = bootGestureAction(
          usb.hostConnected(), now - bootPressedAtMs);
      if (action == BootGestureAction::dictationRelease) stopDictationHold();
      if (action == BootGestureAction::capsuleToggle) toggleRecording();
    }
  }

  // Keep I2S sampling for health diagnostics, but only feed TinyUSB while the
  // host has selected the microphone streaming alternate interface. TinyUSB
  // clears its IN FIFO whenever that interface closes; writing concurrently
  // with the clear can leave CoreAudio receiving an endless series of ZLPs.
  if (usb.microphoneStreaming() && audio.playing()) {
    audio.stopPlayback(usb.log());
  }
  if (audio.playing()) {
    audio.pumpPlayback(usb.log());
  } else if (audio.ready() || recorder.recording()) {
    size_t bytes = audio.read(audioBuffer, sizeof(audioBuffer));
    if (bytes > 0) {
      if (usb.microphoneStreaming()) {
        usb.writeMicrophone(audioBuffer, static_cast<uint16_t>(bytes));
      }
      if (recorder.recording()) {
        const bool wasRecording = recorder.recording();
        recorder.append(audioBuffer, bytes, usb.log());
        if (wasRecording && !recorder.recording()) capsuleLibrary.scan();
      }
    }
  }

  linkService.poll(now);
  if (linkService.receivingBinary()) return;

  // The ESP32-S3 full-speed USB controller is sensitive to interrupt latency
  // during isochronous microphone transfers. Touch, sensor and display I/O are
  // user-interface work, so defer them while CoreAudio owns the mic stream.
  // Audio capture and CDC diagnostics remain active.
  const bool microphoneStreaming = usb.microphoneStreaming();
  if (!microphoneStreaming) {
    provisioningPortal.loop(now);
    if (provisioningPortal.takeConfigurationChanged()) {
      wifi.configurationChanged();
      tencentWorker.wake();
      dashboard.invalidate();
    }
    const bool networkWork = capsuleLibrary.pendingCount() > 0 &&
        deviceConfig.hasTencent() && !tencentWorker.waitingForWake();
    wifi.loop(now, recorder.recording(), networkWork,
              board.status().charging, provisioningPortal.active());
    if (!rtcSyncedFromNetwork && wifi.networkTimeSynchronized()) {
      rtcSyncedFromNetwork = board.setUtcEpoch(time(nullptr));
    }
    tencentWorker.loop(now, wifi.connected(), wifi.timeReady(),
                       recorder.recording(), board.status().charging);
  }
  if ((!microphoneStreaming || touchDictationHolding) &&
      now - lastTouchMs >= 10) {
    lastTouchMs = now;
    pollTouch();
  }

  const uint32_t sensorIntervalMs = board.status().screenOn ? 500 : 100;
  if (!microphoneStreaming && now - lastSensorMs >= sensorIntervalMs) {
    lastSensorMs = now;
    board.refreshSensors();
    const BoardStatus &status = board.status();
    if (raiseToWake.update(now, deviceConfig.settings().raiseToWake,
                           status.screenOn, status.accelerationX,
                           status.accelerationY, status.accelerationZ)) {
      board.setScreenOn(true);
      dashboard.invalidate();
    }
    const PowerKeyEvent powerKey = board.pollPowerKey();
    if (powerKey == PowerKeyEvent::shortPress) {
      board.setScreenOn(!board.status().screenOn);
    } else if (powerKey == PowerKeyEvent::longPress) {
      if (recorder.recording()) recorder.stop(usb.log());
      if (audio.playing()) audio.stopPlayback(usb.log());
      board.safeShutdown();
    }
  }
  if (!microphoneStreaming && now - lastDashboardMs >= 1000) {
    lastDashboardMs = now;
    drawDashboard();
  }
}
