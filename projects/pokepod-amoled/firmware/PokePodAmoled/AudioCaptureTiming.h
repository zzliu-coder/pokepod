#pragma once

#include <stdint.h>

namespace pokepod {

// Arduino-ESP32 ESP_I2S exposes one session-level Stream timeout rather than a
// per-read timeout. Keep the hardware setup, capture adapter and shutdown
// budget derived from this single source of truth.
constexpr uint32_t kAudioCaptureReadTimeoutMs = 50U;
// ES8311 clock/ADC startup can yield immediate zero-byte reads before the
// first DMA block.  Treat only this bounded interval as hardware warm-up; a
// source that remains empty becomes a real failure before the BLE ready
// deadline.
constexpr uint32_t kAudioCaptureWarmupMs = 150U;
static_assert(kAudioCaptureWarmupMs < 400U,
              "capture warm-up must fit inside the host ready watchdog");
static_assert(kAudioCaptureReadTimeoutMs >= 20U,
              "I2S timeout must cover one 20 ms capture frame");
constexpr uint32_t kAudioCaptureStopReadWindows = 10U;
constexpr uint32_t kAudioCaptureStopTimeoutMs =
    kAudioCaptureReadTimeoutMs * kAudioCaptureStopReadWindows;
static_assert(kAudioCaptureStopTimeoutMs == 500U,
              "capture shutdown budget changed");

}  // namespace pokepod
