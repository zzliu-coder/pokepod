#include "TencentAsr.h"

#include <NetworkClientSecure.h>
#include <Network.h>
#include <cJSON.h>
#include <esp_heap_caps.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>
#include <time.h>

#include "Base64Stream.h"
#include "DeferredFileCleanup.h"
#include "StorageCoordinator.h"
#include "Tc3Policy.h"
#include "TencentRootCa.h"

namespace pokepod {
namespace {

constexpr char kHost[] = "asr.tencentcloudapi.com";
constexpr char kContentType[] = "application/json; charset=utf-8";
constexpr char kAction[] = "SentenceRecognition";
constexpr char kActionLower[] = "sentencerecognition";
constexpr char kVersion[] = "2019-06-14";
constexpr char kService[] = "asr";
constexpr size_t kMaxEncodedAudioBytes = 3UL * 1024UL * 1024UL;
constexpr size_t kMaxResponseBytes = 32768;
constexpr uint32_t kSocketIoTimeoutMs = 8000;

bool deadlineExpired(uint32_t deadlineMs) {
  return static_cast<int32_t>(millis() - deadlineMs) >= 0;
}

bool cancelled(const TencentAsrControl *control) {
  return control != nullptr && control->cancelled();
}

void markCancelled(TencentAsrResult &result,
                   const TencentAsrControl *control) {
  result.ok = false;
  result.transient = false;
  result.code = "CANCELLED";
  result.message = "转写已取消";
  if (control != nullptr) control->setStage(TencentAsrStage::cancelled);
}

bool writeAll(NetworkClientSecure &client, const uint8_t *data, size_t length,
              uint32_t deadlineMs, const TencentAsrControl *control) {
  size_t written = 0;
  while (written < length) {
    if (cancelled(control) || deadlineExpired(deadlineMs)) return false;
    const size_t chunk = client.write(data + written, length - written);
    if (chunk == 0) return false;
    written += chunk;
  }
  return true;
}

bool readExact(NetworkClientSecure &client, String &target, size_t length,
               uint32_t deadlineMs, const TencentAsrControl *control) {
  while (length > 0 && target.length() < kMaxResponseBytes) {
    if (cancelled(control)) return false;
    if (client.available()) {
      const int value = client.read();
      if (value < 0) continue;
      target += static_cast<char>(value);
      --length;
    } else if (!client.connected() ||
               static_cast<int32_t>(millis() - deadlineMs) >= 0) {
      return false;
    } else {
      delay(1);
    }
  }
  return length == 0;
}

String readLine(NetworkClientSecure &client, uint32_t deadlineMs,
                const TencentAsrControl *control) {
  String value;
  while (value.length() < 2048) {
    if (cancelled(control)) break;
    if (client.available()) {
      const int next = client.read();
      if (next < 0) continue;
      if (next == '\n') break;
      if (next != '\r') value += static_cast<char>(next);
    } else if (!client.connected() ||
               static_cast<int32_t>(millis() - deadlineMs) >= 0) {
      break;
    } else {
      delay(1);
    }
  }
  return value;
}

}  // namespace

bool TencentAsr::transcribe(fs::FS &fs, const String &audioPath,
                            const DeviceSettings &settings,
                            TencentAsrResult &result, Print &log,
                            const TencentAsrControl *control) {
  const uint32_t startedAtMs = millis();
  result = TencentAsrResult();
  if (control != nullptr) control->setStage(TencentAsrStage::validating);
  if (cancelled(control)) {
    markCancelled(result, control);
    return false;
  }
  if (settings.secretId.isEmpty() || settings.secretKey.isEmpty()) {
    result.code = "CONFIG_MISSING";
    result.message = "腾讯云密钥未配置";
    return false;
  }
  const time_t timestamp = time(nullptr);
  if (timestamp < 1704067200) {
    result.transient = true;
    result.code = "TIME_NOT_SYNCED";
    result.message = "设备时间尚未同步";
    return false;
  }
  StorageReservation storageRead = StorageCoordinator::instance().reserve(
      StorageOwner::tencentRead, StorageAccess::read, 100);
  if (!storageRead) {
    result.transient = true;
    result.code = "STORAGE_BUSY";
    result.message = "SD 卡正在使用";
    return false;
  }
  File audio;
  size_t audioBytes = 0;
  DeferredFileCleanup audioCleanup;
  bool audioCleanupStarted = false;
  auto closeAudio = [&]() {
    // The worker must not return while an open File can be closed by its local
    // destructor outside the tencentRead IO lease.  Other owners hold physical
    // IO only for bounded chunks, so retry here while retaining storageRead.
    if (!audioCleanupStarted) {
      audioCleanupStarted = audioCleanup.begin(
          audio, nullptr, "", storageRead, StorageOwner::tencentRead,
          StorageAccess::read);
    }
    while (audioCleanup.pending()) {
      if (!audioCleanup.poll()) delay(1);
    }
  };
  {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        StorageOwner::tencentRead, StorageAccess::read, 1000);
    if (!lease) {
      result.transient = true;
      result.code = "STORAGE_BUSY";
      result.message = "SD 卡正在使用";
      return false;
    }
    audio = fs.open(audioPath, FILE_READ);
    if (audio && !audio.isDirectory()) audioBytes = audio.size();
  }
  if (!audio || audio.isDirectory() || audioBytes <= 44) {
    closeAudio();
    result.code = "AUDIO_INVALID";
    result.message = "胶囊音频缺失或为空";
    return false;
  }
  const size_t encodedBytes = base64EncodedLength(audioBytes);
  if (encodedBytes > kMaxEncodedAudioBytes) {
    closeAudio();
    result.code = "AUDIO_TOO_LARGE";
    result.message = "音频超过腾讯一句话识别限制";
    return false;
  }

  String prefix =
      "{\"EngSerViceType\":\"16k_zh\",\"SourceType\":1,\"VoiceFormat\":\"wav\",\"Data\":\"";
  String suffix = "\",\"DataLen\":" + String(audioBytes) +
      ",\"WordInfo\":0,\"FilterDirty\":0,\"FilterModal\":1,"
      "\"FilterPunc\":0,\"ConvertNumMode\":1";
  if (!settings.hotwordId.isEmpty()) {
    suffix += ",\"HotwordId\":\"" + jsonEscape(settings.hotwordId) + "\"";
  }
  suffix += "}";

  uint8_t payloadHash[32];
  if (control != nullptr) control->setStage(TencentAsrStage::hashing);
  if (!hashPayload(audio, prefix, suffix, payloadHash, control)) {
    closeAudio();
    if (cancelled(control)) {
      markCancelled(result, control);
      return false;
    }
    result.transient = true;
    result.code = "AUDIO_READ_FAILED";
    result.message = "读取音频失败";
    return false;
  }
  result.hashElapsedMs = millis() - startedAtMs;
  log.printf("{\"event\":\"tencent_asr_stage\",\"stage\":\"hashed\",\"elapsed_ms\":%lu,\"audio_bytes\":%lu}\n",
             static_cast<unsigned long>(millis() - startedAtMs),
             static_cast<unsigned long>(audioBytes));
  const String auth = authorization(settings, timestamp, payloadHash);
  bool seekOk = false;
  {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        StorageOwner::tencentRead, StorageAccess::read, 1000);
    seekOk = lease && audio.seek(0);
  }
  if (auth.isEmpty() || !seekOk) {
    closeAudio();
    if (cancelled(control)) {
      markCancelled(result, control);
      return false;
    }
    result.code = "SIGNATURE_FAILED";
    result.message = "请求签名失败";
    return false;
  }

  IPAddress resolvedAddress;
  if (control != nullptr) control->setStage(TencentAsrStage::resolving);
  if (cancelled(control)) {
    closeAudio();
    markCancelled(result, control);
    return false;
  }
  if (!Network.hostByName(kHost, resolvedAddress)) {
    closeAudio();
    if (cancelled(control)) {
      markCancelled(result, control);
      return false;
    }
    result.transient = true;
    result.code = "DNS_FAILED";
    result.message = "腾讯云域名解析失败";
    log.println("{\"event\":\"tencent_asr_network\",\"stage\":\"dns\",\"ok\":false}");
    return false;
  }

  result.internalHeapFreeBeforeTls = heap_caps_get_free_size(
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  result.internalHeapLargestBeforeTls = heap_caps_get_largest_free_block(
      MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  result.psramFreeBeforeTls = heap_caps_get_free_size(
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  log.printf(
      "{\"event\":\"tencent_asr_network\",\"stage\":\"tls_begin\",\"internal_free\":%lu,\"internal_largest\":%lu,\"psram_free\":%lu}\n",
      static_cast<unsigned long>(result.internalHeapFreeBeforeTls),
      static_cast<unsigned long>(result.internalHeapLargestBeforeTls),
      static_cast<unsigned long>(result.psramFreeBeforeTls));

  NetworkClientSecure client;
  client.setCACert(kTencentRootCa);
  client.setHandshakeTimeout(15);
  client.setTimeout(kSocketIoTimeoutMs);
  if (control != nullptr) control->setStage(TencentAsrStage::connecting);
  if (cancelled(control)) {
    closeAudio();
    markCancelled(result, control);
    return false;
  }
  if (!client.connect(kHost, 443, 15000)) {
    char networkError[160] = {};
    result.networkError = client.lastError(networkError, sizeof(networkError));
    result.networkErrorDetail = networkError;
    closeAudio();
    if (cancelled(control)) {
      markCancelled(result, control);
      return false;
    }
    result.transient = true;
    result.code = "NETWORK_CONNECT_FAILED";
    result.message = "腾讯云 TLS 连接失败";
    log.printf(
        "{\"event\":\"tencent_asr_network\",\"stage\":\"tls\",\"ok\":false,\"error\":%ld}\n",
        static_cast<long>(result.networkError));
    return false;
  }
  if (cancelled(control)) {
    client.stop();
    closeAudio();
    markCancelled(result, control);
    return false;
  }
  result.connectElapsedMs = millis() - startedAtMs;
  log.printf("{\"event\":\"tencent_asr_stage\",\"stage\":\"connected\",\"elapsed_ms\":%lu}\n",
             static_cast<unsigned long>(millis() - startedAtMs));
  const size_t contentLength = prefix.length() + encodedBytes + suffix.length();
  String headers;
  headers.reserve(768);
  headers += "POST / HTTP/1.1\r\nHost: ";
  headers += kHost;
  headers += "\r\nContent-Type: ";
  headers += kContentType;
  headers += "\r\nX-TC-Action: ";
  headers += kAction;
  headers += "\r\nX-TC-Version: ";
  headers += kVersion;
  headers += "\r\nX-TC-Timestamp: ";
  headers += static_cast<unsigned long long>(timestamp);
  headers += "\r\nAuthorization: ";
  headers += auth;
  headers += "\r\nContent-Length: ";
  headers += contentLength;
  headers += "\r\nConnection: close\r\n\r\n";

  const uint32_t uploadDeadlineMs = millis() +
      tencentUploadDeadlineMs(encodedBytes);
  if (control != nullptr) control->setStage(TencentAsrStage::uploading);
  bool sent = writeAll(client,
      reinterpret_cast<const uint8_t *>(headers.c_str()), headers.length(),
      uploadDeadlineMs, control) &&
      writeAll(client, reinterpret_cast<const uint8_t *>(prefix.c_str()),
               prefix.length(), uploadDeadlineMs, control);
  struct ClientSinkContext {
    NetworkClientSecure *client;
    uint32_t deadlineMs;
    const TencentAsrControl *control;
  } sinkContext = {&client, uploadDeadlineMs, control};
  auto clientSink = [](void *context, const uint8_t *data, size_t length) {
    ClientSinkContext *sink = static_cast<ClientSinkContext *>(context);
    return writeAll(*sink->client, data, length, sink->deadlineMs,
                    sink->control);
  };
  if (sent) sent = streamBase64(audio, clientSink, &sinkContext, control);
  if (sent) sent = writeAll(client,
      reinterpret_cast<const uint8_t *>(suffix.c_str()), suffix.length(),
      uploadDeadlineMs, control);
  closeAudio();
  if (!sent) {
    client.stop();
    if (cancelled(control)) {
      markCancelled(result, control);
      return false;
    }
    result.transient = true;
    result.code = deadlineExpired(uploadDeadlineMs)
        ? "NETWORK_WRITE_TIMEOUT" : "NETWORK_WRITE_FAILED";
    result.message = deadlineExpired(uploadDeadlineMs)
        ? "上传音频超时" : "上传音频中断";
    return false;
  }
  result.uploadElapsedMs = millis() - startedAtMs;
  log.printf("{\"event\":\"tencent_asr_stage\",\"stage\":\"uploaded\",\"elapsed_ms\":%lu,\"encoded_bytes\":%lu}\n",
             static_cast<unsigned long>(millis() - startedAtMs),
             static_cast<unsigned long>(encodedBytes));

  int httpStatus = 0;
  String body;
  if (control != nullptr) control->setStage(TencentAsrStage::waitingResponse);
  if (!readHttpResponse(client, httpStatus, body, control)) {
    client.stop();
    if (cancelled(control)) {
      markCancelled(result, control);
      return false;
    }
    result.transient = true;
    result.code = "NETWORK_READ_FAILED";
    result.message = "腾讯云响应不完整";
    return false;
  }
  client.stop();
  result.totalElapsedMs = millis() - startedAtMs;
  log.printf("{\"event\":\"tencent_asr_stage\",\"stage\":\"responded\",\"elapsed_ms\":%lu,\"http_status\":%d}\n",
             static_cast<unsigned long>(millis() - startedAtMs), httpStatus);

  if (control != nullptr) control->setStage(TencentAsrStage::parsing);
  if (cancelled(control)) {
    markCancelled(result, control);
    return false;
  }
  cJSON *root = cJSON_ParseWithLength(body.c_str(), body.length());
  cJSON *response = root == nullptr ? nullptr :
      cJSON_GetObjectItemCaseSensitive(root, "Response");
  cJSON *error = response == nullptr ? nullptr :
      cJSON_GetObjectItemCaseSensitive(response, "Error");
  cJSON *requestId = response == nullptr ? nullptr :
      cJSON_GetObjectItemCaseSensitive(response, "RequestId");
  if (cJSON_IsString(requestId)) result.requestId = requestId->valuestring;
  if (error != nullptr) {
    cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "Code");
    cJSON *message = cJSON_GetObjectItemCaseSensitive(error, "Message");
    result.code = cJSON_IsString(code) ? code->valuestring : "TENCENT_ERROR";
    result.message = cJSON_IsString(message) ? message->valuestring : "腾讯云识别失败";
    result.transient = result.code.startsWith("InternalError") ||
        result.code.startsWith("RequestLimitExceeded") ||
        result.code.startsWith("ResourceUnavailable");
  } else if (httpStatus == 200 && response != nullptr) {
    cJSON *text = cJSON_GetObjectItemCaseSensitive(response, "Result");
    cJSON *duration = cJSON_GetObjectItemCaseSensitive(response, "AudioDuration");
    if (cJSON_IsString(text) && text->valuestring[0] != '\0') {
      result.ok = true;
      result.text = text->valuestring;
      if (cJSON_IsNumber(duration) && duration->valuedouble >= 0) {
        result.audioDurationMs = static_cast<uint32_t>(duration->valuedouble);
      }
    } else {
      result.code = "EMPTY_RESULT";
      result.message = "腾讯云未返回文字";
    }
  } else {
    result.transient = httpStatus >= 500 || httpStatus == 429;
    result.code = "HTTP_" + String(httpStatus);
    result.message = "腾讯云 HTTP 请求失败";
  }
  cJSON_Delete(root);
  log.printf("{\"event\":\"tencent_asr\",\"ok\":%s,\"code\":\"%s\",\"request_id\":\"%s\"}\n",
             result.ok ? "true" : "false", result.code.c_str(),
             result.requestId.c_str());
  if (control != nullptr) control->setStage(TencentAsrStage::completed);
  return result.ok;
}

bool TencentAsr::streamBase64(File &file, StreamSink sink, void *context,
                              const TencentAsrControl *control) {
  if (!file || sink == nullptr) return false;
  if (cancelled(control)) return false;
  {
    StorageIoLease lease = StorageCoordinator::instance().acquireIo(
        StorageOwner::tencentRead, StorageAccess::read, 1000);
    if (!lease || !file.seek(0)) return false;
  }
  Base64StreamEncoder encoder;
  uint8_t input[768];
  while (true) {
    if (cancelled(control)) return false;
    int bytes = 0;
    {
      StorageIoLease lease = StorageCoordinator::instance().acquireIo(
          StorageOwner::tencentRead, StorageAccess::read, 1000);
      if (!lease) return false;
      if (!file.available()) break;
      bytes = file.read(input, sizeof(input));
    }
    if (bytes <= 0) return false;
    auto adapter = [sink, context](const uint8_t *data, size_t length) {
      return sink(context, data, length);
    };
    if (!encoder.append(input, static_cast<size_t>(bytes), adapter)) return false;
  }
  auto adapter = [sink, context](const uint8_t *data, size_t length) {
    return sink(context, data, length);
  };
  return encoder.finish(adapter);
}

bool TencentAsr::hashPayload(File &file, const String &prefix,
                             const String &suffix, uint8_t digest[32],
                             const TencentAsrControl *control) {
  mbedtls_sha256_context context;
  mbedtls_sha256_init(&context);
  bool ok = mbedtls_sha256_starts(&context, 0) == 0 &&
      mbedtls_sha256_update(&context,
          reinterpret_cast<const uint8_t *>(prefix.c_str()), prefix.length()) == 0;
  auto hashSink = [](void *opaque, const uint8_t *data, size_t length) {
    return mbedtls_sha256_update(
        static_cast<mbedtls_sha256_context *>(opaque), data, length) == 0;
  };
  if (ok && !cancelled(control)) {
    ok = streamBase64(file, hashSink, &context, control);
  }
  if (ok) ok = mbedtls_sha256_update(&context,
      reinterpret_cast<const uint8_t *>(suffix.c_str()), suffix.length()) == 0;
  if (ok) ok = mbedtls_sha256_finish(&context, digest) == 0;
  mbedtls_sha256_free(&context);
  return ok;
}

String TencentAsr::authorization(const DeviceSettings &settings,
                                 time_t timestamp,
                                 const uint8_t payloadHash[32]) {
  struct tm utc = {};
  if (gmtime_r(&timestamp, &utc) == nullptr) return String();
  char date[11];
  strftime(date, sizeof(date), "%Y-%m-%d", &utc);
  const String canonical = tc3CanonicalRequest<String>(
      "POST", "/", "", kContentType, kHost, kActionLower,
      hex(payloadHash, 32));
  uint8_t canonicalHash[32];
  sha256(reinterpret_cast<const uint8_t *>(canonical.c_str()),
         canonical.length(), canonicalHash);

  const String scope = tc3CredentialScope<String>(date, kService);
  const String timestampText(static_cast<unsigned long long>(timestamp));
  const String toSign = tc3StringToSign<String>(
      timestampText.c_str(), scope, hex(canonicalHash, 32));
  const String initialKey = "TC3" + settings.secretKey;
  uint8_t secretDate[32];
  uint8_t secretService[32];
  uint8_t secretSigning[32];
  uint8_t signature[32];
  if (!hmacSha256(reinterpret_cast<const uint8_t *>(initialKey.c_str()),
                  initialKey.length(), reinterpret_cast<const uint8_t *>(date),
                  strlen(date), secretDate) ||
      !hmacSha256(secretDate, sizeof(secretDate),
                  reinterpret_cast<const uint8_t *>(kService), strlen(kService),
                  secretService) ||
      !hmacSha256(secretService, sizeof(secretService),
                  reinterpret_cast<const uint8_t *>("tc3_request"), 11,
                  secretSigning) ||
      !hmacSha256(secretSigning, sizeof(secretSigning),
                  reinterpret_cast<const uint8_t *>(toSign.c_str()),
                  toSign.length(), signature)) {
    return String();
  }
  return "TC3-HMAC-SHA256 Credential=" + settings.secretId + "/" + scope +
      ", SignedHeaders=content-type;host;x-tc-action, Signature=" +
      hex(signature, sizeof(signature));
}

bool TencentAsr::readHttpResponse(NetworkClientSecure &client, int &status,
                                  String &body,
                                  const TencentAsrControl *control) {
  const uint32_t deadline = millis() + 30000;
  const String statusLine = readLine(client, deadline, control);
  if (cancelled(control)) return false;
  const int firstSpace = statusLine.indexOf(' ');
  status = firstSpace < 0 ? 0 : statusLine.substring(firstSpace + 1).toInt();
  size_t contentLength = 0;
  bool chunked = false;
  while (true) {
    const String line = readLine(client, deadline, control);
    if (cancelled(control)) return false;
    if (line.isEmpty()) break;
    String lower = line;
    lower.toLowerCase();
    if (lower.startsWith("content-length:")) {
      contentLength = line.substring(line.indexOf(':') + 1).toInt();
    } else if (lower.startsWith("transfer-encoding:") &&
               lower.indexOf("chunked") >= 0) {
      chunked = true;
    }
  }
  body.reserve(contentLength > 0 && contentLength < kMaxResponseBytes
      ? contentLength + 1 : 2048);
  if (chunked) {
    while (body.length() < kMaxResponseBytes) {
      const String sizeLine = readLine(client, deadline, control);
      if (cancelled(control)) return false;
      const size_t chunkSize = strtoul(sizeLine.c_str(), nullptr, 16);
      if (chunkSize == 0) {
        readLine(client, deadline, control);
        break;
      }
      if (body.length() + chunkSize > kMaxResponseBytes ||
          !readExact(client, body, chunkSize, deadline, control)) return false;
      readLine(client, deadline, control);
    }
    return !body.isEmpty();
  }
  if (contentLength > kMaxResponseBytes) return false;
  if (contentLength > 0) {
    return readExact(client, body, contentLength, deadline, control);
  }
  while ((client.connected() || client.available()) &&
         body.length() < kMaxResponseBytes &&
         static_cast<int32_t>(millis() - deadline) < 0) {
    if (cancelled(control)) return false;
    if (client.available()) body += static_cast<char>(client.read());
    else delay(1);
  }
  return !body.isEmpty();
}

String TencentAsr::hex(const uint8_t *data, size_t length) {
  static constexpr char digits[] = "0123456789abcdef";
  String value;
  value.reserve(length * 2);
  for (size_t index = 0; index < length; ++index) {
    value += digits[data[index] >> 4];
    value += digits[data[index] & 0x0f];
  }
  return value;
}

String TencentAsr::jsonEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length());
  for (size_t index = 0; index < value.length(); ++index) {
    const char c = value[index];
    if (c == '"' || c == '\\') escaped += '\\';
    if (static_cast<uint8_t>(c) >= 0x20) escaped += c;
  }
  return escaped;
}

void TencentAsr::sha256(const uint8_t *data, size_t length,
                        uint8_t output[32]) {
  mbedtls_sha256(data, length, output, 0);
}

bool TencentAsr::hmacSha256(const uint8_t *key, size_t keyLength,
                            const uint8_t *data, size_t dataLength,
                            uint8_t output[32]) {
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  return info != nullptr &&
      mbedtls_md_hmac(info, key, keyLength, data, dataLength, output) == 0;
}

}  // namespace pokepod
