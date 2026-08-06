#include <cassert>
#include <cstring>

#include "../PokePodAmoled/FirmwarePolicy.h"
#include "../PokePodAmoled/WavFormat.h"

namespace {

uint16_t littleEndian16(const uint8_t *value) {
  return static_cast<uint16_t>(value[0]) |
         (static_cast<uint16_t>(value[1]) << 8);
}

uint32_t littleEndian32(const uint8_t *value) {
  return static_cast<uint32_t>(value[0]) |
         (static_cast<uint32_t>(value[1]) << 8) |
         (static_cast<uint32_t>(value[2]) << 16) |
         (static_cast<uint32_t>(value[3]) << 24);
}

}  // namespace

int main() {
  using namespace pokepod;

  static_assert(kAudioBytesPerChunk == 192);
  static_assert(kUsbAudioBytesPerChunk == 96);
  static_assert(kUsbAudioEndpointMaxPacketBytes == 98);
  const uint8_t stereo[] = {
      0x01, 0x02, 0x11, 0x12,
      0x03, 0x04, 0x13, 0x14,
  };
  uint8_t mono[4] = {};
  assert(pcm16StereoLeftToMono(stereo, sizeof(stereo), mono, sizeof(mono)) == 4);
  const uint8_t expectedMono[] = {0x01, 0x02, 0x03, 0x04};
  assert(std::memcmp(mono, expectedMono, sizeof(mono)) == 0);
  assert(pcm16StereoLeftToMono(nullptr, sizeof(stereo), mono, sizeof(mono)) == 0);
  constexpr uint8_t pcmSamples[] = {
      0x00, 0x00,  // 0
      0xff, 0x7f,  // 32767
      0x00, 0x80,  // -32768
      0xff, 0xff,  // -1
  };
  static_assert(pcm16PeakLittleEndian(pcmSamples, sizeof(pcmSamples)) == 32768);
  static_assert(pcm16PeakLittleEndian(pcmSamples, 1) == 0);
  static_assert(pcm16PeakLittleEndian(nullptr, 4) == 0);
  assert(boardVariantFromTouchProbes(true, false) ==
         BoardVariant::v1Sh8601Ft3168);
  assert(boardVariantFromTouchProbes(false, true) ==
         BoardVariant::v2Co5300Cst820);
  assert(boardVariantFromTouchProbes(false, false) == BoardVariant::unknown);
  assert(boardVariantFromTouchProbes(true, true) == BoardVariant::unknown);

  assert(touchActionAt(16, 246) == TouchAction::dictation);
  assert(touchActionAt(kDisplayWidth - 17, 327) == TouchAction::dictation);
  assert(touchActionAt(16, 342) == TouchAction::recording);
  assert(touchActionAt(kDisplayWidth - 17, 423) == TouchAction::recording);
  assert(touchActionAt(15, 246) == TouchAction::none);
  assert(touchActionAt(16, 328) == TouchAction::none);
  assert(touchActionAt(16, 424) == TouchAction::none);

  assert(deadlinePending(100, 200));
  assert(!deadlinePending(200, 200));
  assert(!deadlinePending(201, 200));
  assert(deadlinePending(UINT32_MAX - 2, 5));
  assert(!deadlinePending(6, 5));

  constexpr uint32_t oneSecondBytes = 32000;
  uint8_t header[kWavHeaderBytes];
  encodeWavHeader(header, oneSecondBytes);
  assert(std::memcmp(header, "RIFF", 4) == 0);
  assert(littleEndian32(header + 4) == 36 + oneSecondBytes);
  assert(std::memcmp(header + 8, "WAVEfmt ", 8) == 0);
  assert(littleEndian16(header + 20) == 1);
  assert(littleEndian16(header + 22) == 1);
  assert(littleEndian32(header + 24) == 16000);
  assert(littleEndian32(header + 28) == oneSecondBytes);
  assert(littleEndian16(header + 32) == 2);
  assert(littleEndian16(header + 34) == 16);
  assert(std::memcmp(header + 36, "data", 4) == 0);
  assert(littleEndian32(header + 40) == oneSecondBytes);
  uint32_t validatedBytes = 0;
  assert(validCapsuleWavHeader(header, sizeof(header),
                               sizeof(header) + oneSecondBytes,
                               validatedBytes));
  assert(validatedBytes == oneSecondBytes);
  header[22] = 2;
  assert(!validCapsuleWavHeader(header, sizeof(header),
                                sizeof(header) + oneSecondBytes,
                                validatedBytes));
  assert(audioDurationMs(oneSecondBytes) == 1000);
  assert(audioDurationMs(32) == 1);
  static_assert(kMaxCapsuleDurationMs == 58500);
  return 0;
}
