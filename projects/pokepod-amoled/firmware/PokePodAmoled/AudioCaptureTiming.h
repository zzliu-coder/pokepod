#pragma once

#include <stdint.h>

namespace pokepod {

// Arduino-ESP32 ESP_I2S exposes one session-level Stream timeout rather than a
// per-read timeout. Keep the hardware setup, capture adapter and shutdown
// budget derived from this single source of truth.
constexpr uint32_t kAudioCaptureReadTimeoutMs = 50U;
static_assert(kAudioCaptureReadTimeoutMs >= 20U,
              "I2S timeout must cover one 20 ms capture frame");
constexpr uint32_t kAudioCaptureStopReadWindows = 10U;
constexpr uint32_t kAudioCaptureStopTimeoutMs =
    kAudioCaptureReadTimeoutMs * kAudioCaptureStopReadWindows;
static_assert(kAudioCaptureStopTimeoutMs == 500U,
              "capture shutdown budget changed");

}  // namespace pokepod
