#include "AudioPipeline.h"

#include <Wire.h>
#include <esp_check.h>
#include <es8311.h>

#include "WavFormat.h"

namespace pokepod {

bool AudioPipeline::begin(Print &log) {
  pinMode(kSpeakerAmpPin, OUTPUT);
  // Waveshare's ES8311 example enables the board audio power path before
  // starting I2S. BLE and local WAV both consume the physical microphone, so no
  // playback samples are routed to this TX channel.
  digitalWrite(kSpeakerAmpPin, LOW);
  i2s_.setPins(kI2sBclk, kI2sWordSelect, kI2sDataOut, kI2sDataIn, kI2sMclk);
  available_ = startHardware(log);
  if (available_) stopHardware(log);
  log.printf("{\"event\":\"audio_ready\",\"ok\":%s,\"policy\":\"on_demand\"}\n",
             available_ ? "true" : "false");
  return available_;
}

bool AudioPipeline::startHardware(Print &log) {
  if (hardwareActive_) return true;
  if (!i2s_.begin(I2S_MODE_STD, kAudioSampleRate, I2S_DATA_BIT_WIDTH_16BIT,
                  I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    log.println("{\"event\":\"audio\",\"ok\":false,\"stage\":\"i2s\"}");
    return false;
  }
  // Read one 1 ms stereo I2S interval at a time. The shared AudioFrontEnd
  // selects the physical microphone slot and emits 16 kHz mono samples.
  i2s_.setTimeout(50);

  es8311_handle_t codec = es8311_create(0, ES8311_ADDRESS_0);
  if (codec == nullptr) {
    i2s_.end();
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
  if (error == ESP_OK) error = es8311_voice_volume_set(codec, 82, nullptr);
  if (error == ESP_OK) error = es8311_microphone_gain_set(codec, ES8311_MIC_GAIN_30DB);
  if (error != ESP_OK) {
    es8311_delete(codec);
    i2s_.end();
    log.printf("{\"event\":\"audio\",\"ok\":false,\"stage\":\"codec_init\",\"error\":%d}\n", error);
    return false;
  }
  codec_ = codec;
  hardwareActive_ = true;
  log.printf("{\"event\":\"audio\",\"ok\":true,\"sample_rate\":%lu,\"channels\":%u,\"microphone_gain_db\":30,\"frontend\":\"voice_v1\"}\n",
             static_cast<unsigned long>(kAudioSampleRate), kAudioChannels);
  return true;
}

bool AudioPipeline::startCapture(Print &log) {
  if (!available_) return false;
  return startHardware(log);
}

void AudioPipeline::stopHardware(Print &log) {
  if (!hardwareActive_ || playing_) return;
  digitalWrite(kSpeakerAmpPin, LOW);
  // Resetting the ES8311 before removing MCLK closes its ADC/DAC power path.
  Wire.beginTransmission(ES8311_ADDRESS_0);
  Wire.write(static_cast<uint8_t>(0x00));
  Wire.write(static_cast<uint8_t>(0x1F));
  Wire.endTransmission();
  i2s_.end();
  if (codec_ != nullptr) {
    es8311_delete(static_cast<es8311_handle_t>(codec_));
    codec_ = nullptr;
  }
  hardwareActive_ = false;
  log.println("{\"event\":\"audio_power\",\"active\":false}");
}

size_t AudioPipeline::read(uint8_t *buffer, size_t capacity) {
  if (!hardwareActive_ || buffer == nullptr || capacity == 0) return 0;
  const size_t bytes = i2s_.readBytes(reinterpret_cast<char *>(buffer), capacity);
  if (bytes == 0) {
    ++readFailures_;
    return 0;
  }

  bytesRead_ += bytes;
  const uint16_t peak = pcm16PeakLittleEndian(buffer, bytes);
  peakWindow_.observe(peak);
  return bytes;
}

bool AudioPipeline::startPlayback(fs::FS &fs, const String &path, Print &log) {
  if (!available_ || playing_ || !startHardware(log)) return false;
  File file = fs.open(path, FILE_READ);
  uint8_t header[kWavHeaderBytes];
  uint32_t dataBytes = 0;
  if (!file || file.isDirectory() ||
      file.read(header, sizeof(header)) != sizeof(header) ||
      !validCapsuleWavHeader(header, sizeof(header), file.size(), dataBytes)) {
    if (file) file.close();
    stopHardware(log);
    log.println("{\"event\":\"playback_error\",\"stage\":\"wav_header\"}");
    return false;
  }
  playbackFile_ = file;
  playbackRemaining_ = dataBytes;
  playing_ = true;
  digitalWrite(kSpeakerAmpPin, HIGH);
  log.printf("{\"event\":\"playback_started\",\"bytes\":%lu}\n",
             static_cast<unsigned long>(dataBytes));
  return true;
}

void AudioPipeline::pumpPlayback(Print &log) {
  if (!playing_) return;
  const size_t wanted = playbackRemaining_ < sizeof(playbackInput_)
      ? playbackRemaining_ : sizeof(playbackInput_);
  const size_t count = playbackFile_.read(playbackInput_, wanted);
  if (count == 0 || count % 2 != 0) {
    stopPlayback(log);
    return;
  }
  size_t output = 0;
  for (size_t offset = 0; offset < count; offset += 2) {
    for (uint8_t repeat = 0; repeat < 3; ++repeat) {
      playbackOutput_[output++] = playbackInput_[offset];
      playbackOutput_[output++] = playbackInput_[offset + 1];
      playbackOutput_[output++] = playbackInput_[offset];
      playbackOutput_[output++] = playbackInput_[offset + 1];
    }
  }
  if (i2s_.write(playbackOutput_, output) != output) {
    log.println("{\"event\":\"playback_error\",\"stage\":\"i2s_write\"}");
    stopPlayback(log);
    return;
  }
  playbackRemaining_ -= count;
  if (playbackRemaining_ == 0) stopPlayback(log);
}

void AudioPipeline::stopPlayback(Print &log) {
  if (!playing_) return;
  playbackFile_.close();
  playbackRemaining_ = 0;
  playing_ = false;
  digitalWrite(kSpeakerAmpPin, LOW);
  log.println("{\"event\":\"playback_stopped\"}");
  stopHardware(log);
}

}  // namespace pokepod
