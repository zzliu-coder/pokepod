#include "AudioPipeline.h"

#include <Wire.h>
#include <esp_check.h>
#include <es8311.h>

namespace pokepod {

bool AudioPipeline::begin(Print &log) {
  pinMode(kSpeakerAmpPin, OUTPUT);
  // Waveshare's ES8311 example enables the board audio power path before
  // starting I2S. USB exposes a microphone-only UAC interface, so no host
  // playback samples are routed to this TX channel.
  digitalWrite(kSpeakerAmpPin, HIGH);
  i2s_.setPins(kI2sBclk, kI2sWordSelect, kI2sDataOut, kI2sDataIn, kI2sMclk);
  if (!i2s_.begin(I2S_MODE_STD, kAudioSampleRate, I2S_DATA_BIT_WIDTH_16BIT,
                  I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    log.println("{\"event\":\"audio\",\"ok\":false,\"stage\":\"i2s\"}");
    return false;
  }
  // Read one 1 ms stereo I2S interval at a time. USB later selects the physical
  // microphone channel and emits a 96-byte mono packet; SD keeps both I2S slots.
  i2s_.setTimeout(50);

  es8311_handle_t codec = es8311_create(0, ES8311_ADDRESS_0);
  if (codec == nullptr) {
    log.println("{\"event\":\"audio\",\"ok\":false,\"stage\":\"codec_create\"}");
    return false;
  }
  const es8311_clock_config_t clock = {
      .mclk_inverted = false,
      .sclk_inverted = false,
      .mclk_from_mclk_pin = true,
      .mclk_frequency = static_cast<int>(kAudioSampleRate * 256),
      .sample_frequency = static_cast<int>(kAudioSampleRate),
  };
  esp_err_t error = es8311_init(codec, &clock, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
  if (error == ESP_OK) {
    error = es8311_sample_frequency_config(codec, clock.mclk_frequency,
                                           clock.sample_frequency);
  }
  if (error == ESP_OK) error = es8311_microphone_config(codec, false);
  if (error == ESP_OK) error = es8311_microphone_gain_set(codec, ES8311_MIC_GAIN_30DB);
  if (error != ESP_OK) {
    log.printf("{\"event\":\"audio\",\"ok\":false,\"stage\":\"codec_init\",\"error\":%d}\n", error);
    return false;
  }
  ready_ = true;
  log.printf("{\"event\":\"audio\",\"ok\":true,\"sample_rate\":%lu,\"channels\":%u}\n",
             static_cast<unsigned long>(kAudioSampleRate), kAudioChannels);
  return true;
}

size_t AudioPipeline::read(uint8_t *buffer, size_t capacity) {
  if (!ready_ || buffer == nullptr || capacity == 0) return 0;
  const size_t bytes = i2s_.readBytes(reinterpret_cast<char *>(buffer), capacity);
  if (bytes == 0) {
    ++readFailures_;
    return 0;
  }

  bytesRead_ += bytes;
  const uint16_t peak = pcm16PeakLittleEndian(buffer, bytes);
  if (peak > peakSample_) peakSample_ = peak;
  return bytes;
}

}  // namespace pokepod
