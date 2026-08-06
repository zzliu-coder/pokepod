#pragma once

#include <Arduino.h>
#include <FS.h>
#include <NetworkClientSecure.h>

#include "DeviceConfig.h"

namespace pokepod {

struct TencentAsrResult {
  bool ok = false;
  bool transient = false;
  String text;
  String code;
  String message;
  String requestId;
  uint32_t audioDurationMs = 0;
};

class TencentAsr {
 public:
  bool transcribe(fs::FS &fs, const String &audioPath,
                  const DeviceSettings &settings, TencentAsrResult &result,
                  Print &log);

 private:
  using StreamSink = bool (*)(void *, const uint8_t *, size_t);

  bool streamBase64(File &file, StreamSink sink, void *context);
  bool hashPayload(File &file, const String &prefix, const String &suffix,
                   uint8_t digest[32]);
  bool readHttpResponse(NetworkClientSecure &client, int &status,
                        String &body);
  String authorization(const DeviceSettings &settings, time_t timestamp,
                       const uint8_t payloadHash[32]);
  static String hex(const uint8_t *data, size_t length);
  static String jsonEscape(const String &value);
  static void sha256(const uint8_t *data, size_t length, uint8_t output[32]);
  static bool hmacSha256(const uint8_t *key, size_t keyLength,
                         const uint8_t *data, size_t dataLength,
                         uint8_t output[32]);
};

}  // namespace pokepod
