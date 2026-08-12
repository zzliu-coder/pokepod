#include "AudioPipeline.h"

#include <Wire.h>
#include <esp_check.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <utility>
#include <es8311.h>

#include "AudioI2sRoute.h"
#include "PlaybackPcm.h"
#include "WavFormat.h"

namespace pokepod {

bool AudioPipeline::begin(BoardVariant variant, Print &log) {
  boardProfile_ = audioBoardProfile(variant);
  if (!boardProfile_.valid()) {
    log.println("{\"event\":\"audio_ready\",\"ok\":false,\"stage\":\"profile\"}");
    return false;
  }
  pinMode(kSpeakerAmpPin, OUTPUT);
  // Waveshare's ES8311 example enables the board audio power path before
  // starting I2S. BLE and local WAV both consume the physical microphone, so no
  // playback samples are routed to this TX channel.
  digitalWrite(kSpeakerAmpPin, LOW);
  available_ = startHardware(HardwareMode::capture, kAudioSampleRate, log);
  if (available_) stopHardware(log);
  log.printf("{\"event\":\"audio_ready\",\"ok\":%s,\"policy\":\"on_demand\"}\n",
             available_ ? "true" : "false");
  return available_;
}

bool AudioPipeline::startHardware(HardwareMode mode, uint32_t sampleRate,
                                  Print &log) {
  if (hardwareActive_) {
    return hardwareMode_ == mode && hardwareSampleRate_ == sampleRate;
  }
  lastHardwareError_ = "none";
  const AudioI2sRoute route = mode == HardwareMode::playback
      ? AudioI2sRoute::playback : AudioI2sRoute::capture;
  const AudioI2sDataPins dataPins = audioI2sDataPins(
      route, static_cast<int8_t>(kI2sDataOut),
      static_cast<int8_t>(kI2sDataIn));
  // Capture owns RX only and playback owns TX only. Allocating both directions
  // wastes a second DMA ring and can fail after a TLS transcription fragments
  // internal memory, even though the requested direction still fits.
  i2s_.setPins(kI2sBclk, kI2sWordSelect, dataPins.dataOut,
               dataPins.dataIn, kI2sMclk);
  if (!i2s_.begin(I2S_MODE_STD, sampleRate, I2S_DATA_BIT_WIDTH_16BIT,
                  I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    lastHardwareError_ = "i2s_begin";
    log.println("{\"event\":\"audio\",\"ok\":false,\"stage\":\"i2s\"}");
    return false;
  }
  // ESP_I2S applies one Stream timeout to the complete hardware session; it
  // does not accept a per-read timeout. The capture adapter and stop budget
  // therefore share this exact fixed value.
  i2s_.setTimeout(kAudioCaptureReadTimeoutMs);

  es8311_handle_t codec = es8311_create(0, ES8311_ADDRESS_0);
  if (codec == nullptr) {
    i2s_.end();
    lastHardwareError_ = "codec_create";
    log.println("{\"event\":\"audio\",\"ok\":false,\"stage\":\"codec_create\"}");
    return false;
  }
  const es8311_clock_config_t clock = {
      .mclk_inverted = false,
      .sclk_inverted = false,
      .mclk_from_mclk_pin = true,
      .mclk_frequency = static_cast<int>(sampleRate * 256),
      .sample_frequency = static_cast<int>(sampleRate),
  };
  esp_err_t error = es8311_init(codec, &clock, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
  if (error == ESP_OK) {
    error = es8311_sample_frequency_config(codec, clock.mclk_frequency,
                                           clock.sample_frequency);
  }
  const AudioCodecPolicy codecPolicy = audioCodecPolicy(
      mode == HardwareMode::capture ? AudioCodecPath::capture
                                    : AudioCodecPath::playback);
  if (error == ESP_OK && codecPolicy.configureMicrophone) {
    error = es8311_microphone_config(codec, false);
  }
  if (error == ESP_OK && codecPolicy.configureMicrophone) {
    error = es8311_microphone_gain_set(codec, ES8311_MIC_GAIN_30DB);
  }
  if (error == ESP_OK && codecPolicy.configureOutput) {
    error = es8311_voice_volume_set(codec, 85, nullptr);
  }
  if (error == ESP_OK) error = es8311_voice_mute(codec, codecPolicy.muteOutput);
  if (error != ESP_OK) {
    es8311_delete(codec);
    i2s_.end();
    lastHardwareError_ = "codec_init";
    log.printf("{\"event\":\"audio\",\"ok\":false,\"stage\":\"codec_init\",\"error\":%d}\n", error);
    return false;
  }
  codec_ = codec;
  hardwareActive_ = true;
  hardwareMode_ = mode;
  hardwareSampleRate_ = sampleRate;
  log.printf("{\"event\":\"audio\",\"ok\":true,\"mode\":\"%s\",\"sample_rate\":%lu,\"channels\":%u,\"dma_direction\":\"%s\",\"microphone_configured\":%s,\"microphone_gain_db\":%u,\"dsp_profile\":\"%s\"}\n",
             mode == HardwareMode::playback ? "playback" : "capture",
             static_cast<unsigned long>(sampleRate), kAudioChannels,
             mode == HardwareMode::playback ? "tx" : "rx",
             codecPolicy.configureMicrophone ? "true" : "false",
             codecPolicy.configureMicrophone ? boardProfile_.microphoneGainDb : 0,
             audioDspProfileName(boardProfile_.dsp));
  return true;
}

bool AudioPipeline::startCapture(Print &log) {
  if (!available_) return false;
  return startHardware(HardwareMode::capture, kAudioSampleRate, log);
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
  hardwareMode_ = HardwareMode::none;
  hardwareSampleRate_ = 0;
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

size_t AudioPipeline::readCaptureRealtime(uint8_t *buffer, size_t capacity) {
  if (!hardwareActive_ || hardwareMode_ != HardwareMode::capture ||
      buffer == nullptr || capacity == 0) {
    return 0;
  }
  return i2s_.readBytes(reinterpret_cast<char *>(buffer), capacity);
}

void AudioPipeline::observeCapturedMono(const int16_t *samples, size_t count) {
  if (samples == nullptr || count == 0) return;
  uint16_t peak = 0;
  for (size_t index = 0; index < count; ++index) {
    const int32_t sample = samples[index];
    const uint16_t magnitude = static_cast<uint16_t>(
        sample < 0 ? (sample == INT16_MIN ? 32768 : -sample) : sample);
    if (magnitude > peak) peak = magnitude;
  }
  peakWindow_.observe(peak);
}

bool AudioPipeline::startPlayback(fs::FS &fs, const String &path, Print &log) {
  if (!available_ || playing_ || playbackCleanup_.pending()) return false;
  lastPlaybackError_ = "none";
  StorageReservation storage = StorageCoordinator::instance().reserve(
      StorageOwner::audioPlayback, StorageAccess::read, 50);
  if (!storage) {
    lastPlaybackError_ = "storage_busy";
    ++playbackStartFailures_;
    log.println("{\"event\":\"playback_error\",\"stage\":\"storage_busy\"}");
    return false;
  }
  playbackHeapLargestBeforeStart_ = heap_caps_get_largest_free_block(
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  // A completed capture may leave the shared clock alive until the end of the
  // current loop.  The UI has already established that nobody owns the mic,
  // so close that idle capture mode before switching the codec to playback.
  if (hardwareActive_) stopHardware(log);
  if (hardwareActive_) return false;
  StorageIoLease openIo = StorageCoordinator::instance().acquireIo(
      StorageOwner::audioPlayback, StorageAccess::read, 50);
  if (!openIo) {
    lastPlaybackError_ = "storage_busy";
    ++playbackStartFailures_;
    return false;
  }
  File file = fs.open(path, FILE_READ);
  uint8_t header[kWavHeaderBytes];
  uint32_t dataBytes = 0;
  if (!file || file.isDirectory() ||
      file.read(header, sizeof(header)) != sizeof(header) ||
      !validCapsuleWavHeader(header, sizeof(header), file.size(), dataBytes)) {
    if (file) file.close();
    lastPlaybackError_ = "wav_header";
    ++playbackStartFailures_;
    log.println("{\"event\":\"playback_error\",\"stage\":\"wav_header\"}");
    return false;
  }
  openIo.release();
  playbackFile_ = std::move(file);
  playbackReservation_ = std::move(storage);
  if (!startHardware(HardwareMode::playback, kCapsuleSampleRate, log)) {
    (void)playbackCleanup_.begin(
        playbackFile_, nullptr, "", playbackReservation_,
        StorageOwner::audioPlayback, StorageAccess::read);
    playbackCleanupLogPending_ = true;
    (void)pollPlaybackCleanup(log);
    lastPlaybackError_ = lastHardwareError_;
    ++playbackStartFailures_;
    log.println("{\"event\":\"playback_error\",\"stage\":\"hardware\"}");
    return false;
  }
  playbackFileRemaining_ = dataBytes;
  playbackBufferedBytes_ = 0;
  playbackBufferOffset_ = 0;
  playbackFileReadCount_ = 0;
  playbackPumpCount_ = 0;
  playbackMaxFileReadUs_ = 0;
  playing_ = true;
  digitalWrite(kSpeakerAmpPin, HIGH);
  // Give the board amplifier a short, deterministic settling interval before
  // the first speech sample.  This is paid once per playback, not per frame.
  delay(15);
  log.printf("{\"event\":\"playback_started\",\"bytes\":%lu}\n",
             static_cast<unsigned long>(dataBytes));
  return true;
}

void AudioPipeline::pumpPlayback(Print &log) {
  if (!playing_) return;
  ++playbackPumpCount_;
  if (playbackBufferOffset_ >= playbackBufferedBytes_) {
    if (playbackFileRemaining_ == 0) {
      stopPlayback(log);
      return;
    }
    const size_t wanted = playbackReadSize(playbackFileRemaining_);
    const int64_t startedUs = esp_timer_get_time();
    StorageIoLease readIo = StorageCoordinator::instance().acquireIo(
        StorageOwner::audioPlayback, StorageAccess::read, 5);
    if (!readIo) return;
    const size_t count = playbackFile_.read(playbackInput_, wanted);
    const uint32_t elapsedUs = static_cast<uint32_t>(
        esp_timer_get_time() - startedUs);
    if (elapsedUs > playbackMaxFileReadUs_) playbackMaxFileReadUs_ = elapsedUs;
    ++playbackFileReadCount_;
    if (count == 0 || count % 2 != 0) {
      lastPlaybackError_ = "file_read";
      log.println("{\"event\":\"playback_error\",\"stage\":\"file_read\"}");
      stopPlayback(log);
      return;
    }
    playbackFileRemaining_ -= count;
    playbackBufferedBytes_ = count;
    playbackBufferOffset_ = 0;
  }
  const size_t count = playbackFeedSize(
      playbackBufferedBytes_ - playbackBufferOffset_);
  if (count == 0) {
    lastPlaybackError_ = "pcm_format";
    stopPlayback(log);
    return;
  }
  const size_t output = mono16LittleEndianToStereo16LittleEndian(
      playbackInput_ + playbackBufferOffset_, count,
      playbackOutput_, sizeof(playbackOutput_));
  if (output == 0) {
    lastPlaybackError_ = "pcm_format";
    log.println("{\"event\":\"playback_error\",\"stage\":\"pcm_format\"}");
    stopPlayback(log);
    return;
  }
  if (i2s_.write(playbackOutput_, output) != output) {
    lastPlaybackError_ = "i2s_write";
    log.println("{\"event\":\"playback_error\",\"stage\":\"i2s_write\"}");
    stopPlayback(log);
    return;
  }
  playbackBufferOffset_ += count;
  if (playbackFileRemaining_ == 0 &&
      playbackBufferOffset_ >= playbackBufferedBytes_) stopPlayback(log);
}

void AudioPipeline::stopPlayback(Print &log) {
  if (!playing_) return;
  playbackFileRemaining_ = 0;
  playbackBufferedBytes_ = 0;
  playbackBufferOffset_ = 0;
  playing_ = false;
  digitalWrite(kSpeakerAmpPin, LOW);
  stopHardware(log);
  (void)playbackCleanup_.begin(
      playbackFile_, nullptr, "", playbackReservation_,
      StorageOwner::audioPlayback, StorageAccess::read);
  playbackCleanupLogPending_ = true;
  (void)pollPlaybackCleanup(log);
}

bool AudioPipeline::pollPlaybackCleanup(Print &log) {
  if (!playbackCleanup_.pending()) return true;
  if (!playbackCleanup_.poll()) return false;
  if (playbackCleanupLogPending_) {
    log.println("{\"event\":\"playback_stopped\"}");
    playbackCleanupLogPending_ = false;
  }
  return true;
}

}  // namespace pokepod
