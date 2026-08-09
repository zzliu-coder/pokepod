#include "WirelessSyncIdentity.h"

#include <cJSON.h>
#include <esp_random.h>
#include <mbedtls/ecp.h>
#include <mbedtls/md.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>
#include <mbedtls/x509_crt.h>
#include <time.h>

#include "WirelessSyncProtocol.h"

namespace pokepod {
namespace {

constexpr char kIdentityKey[] = "sync_identity";

int hardwareRandom(void *, unsigned char *output, size_t size) {
  esp_fill_random(output, size);
  return 0;
}

String jsonText(cJSON *root) {
  char *printed = cJSON_PrintUnformatted(root);
  const String value = printed == nullptr ? String() : String(printed);
  cJSON_free(printed);
  return value;
}

void formatIsoUtc(time_t value, char output[25]) {
  struct tm utc = {};
  gmtime_r(&value, &utc);
  strftime(output, 25, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

}  // namespace

bool WirelessSyncIdentity::begin(uint64_t hardwareId, Print &log) {
  log_ = &log;
  formatPokePodDeviceId(hardwareId, deviceId_);
  preferencesOpen_ = preferences_.begin("pokepod-sync", false);
  if (!preferencesOpen_) {
    log.println("{\"event\":\"wifi_sync_identity\",\"ok\":false,\"stage\":\"nvs_open\"}");
    return false;
  }
  const bool loaded = preferences_.getBytesLength(kIdentityKey) ==
          sizeof(stored_) &&
      preferences_.getBytes(kIdentityKey, &stored_, sizeof(stored_)) ==
          sizeof(stored_) && validateWirelessIdentityBlob(stored_);
  if (!loaded && (!generate() || !persist())) {
    log.println("{\"event\":\"wifi_sync_identity\",\"ok\":false,\"stage\":\"generate\"}");
    return false;
  }
  ready_ = computeCertificateFingerprint();
  log.printf("{\"event\":\"wifi_sync_identity\",\"ok\":%s,\"paired\":%s}\n",
             ready_ ? "true" : "false", paired() ? "true" : "false");
  return ready_;
}

bool WirelessSyncIdentity::generate() {
  stored_ = StoredWirelessSyncIdentity();
  if (!rotatePairing()) return false;

  mbedtls_pk_context key;
  mbedtls_x509write_cert certificate;
  mbedtls_pk_init(&key);
  mbedtls_x509write_crt_init(&certificate);
  bool ok = false;
  do {
    const mbedtls_pk_info_t *info =
        mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY);
    if (info == nullptr || mbedtls_pk_setup(&key, info) != 0) break;
    if (mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key),
                            hardwareRandom, nullptr) != 0) break;
    uint8_t serialBytes[16];
    esp_fill_random(serialBytes, sizeof(serialBytes));
    serialBytes[0] &= 0x7f;
    serialBytes[0] |= 1;
    mbedtls_x509write_crt_set_version(&certificate,
                                     MBEDTLS_X509_CRT_VERSION_3);
    mbedtls_x509write_crt_set_md_alg(&certificate, MBEDTLS_MD_SHA256);
    mbedtls_x509write_crt_set_subject_key(&certificate, &key);
    mbedtls_x509write_crt_set_issuer_key(&certificate, &key);
    if (mbedtls_x509write_crt_set_subject_name(
            &certificate, "CN=PokePod Wireless Sync,O=PokeCapsule") != 0 ||
        mbedtls_x509write_crt_set_issuer_name(
            &certificate, "CN=PokePod Wireless Sync,O=PokeCapsule") != 0 ||
        mbedtls_x509write_crt_set_serial_raw(
            &certificate, serialBytes, sizeof(serialBytes)) != 0 ||
        mbedtls_x509write_crt_set_validity(
            &certificate, "20240101000000", "20491231235959") != 0 ||
        mbedtls_x509write_crt_set_basic_constraints(&certificate, 0, -1) != 0 ||
        mbedtls_x509write_crt_set_key_usage(
            &certificate, MBEDTLS_X509_KU_DIGITAL_SIGNATURE |
                              MBEDTLS_X509_KU_KEY_AGREEMENT) != 0) {
      break;
    }

    uint8_t certificateBuffer[kWirelessCertificateCapacity] = {};
    const int certificateBytes = mbedtls_x509write_crt_der(
        &certificate, certificateBuffer, sizeof(certificateBuffer),
        hardwareRandom, nullptr);
    uint8_t keyBuffer[kWirelessPrivateKeyCapacity] = {};
    const int keyBytes = mbedtls_pk_write_key_der(
        &key, keyBuffer, sizeof(keyBuffer));
    if (certificateBytes <= 0 ||
        certificateBytes > static_cast<int>(sizeof(stored_.certificate)) ||
        keyBytes <= 0 || keyBytes > static_cast<int>(sizeof(stored_.privateKey))) {
      break;
    }
    stored_.certificateBytes = static_cast<uint16_t>(certificateBytes);
    stored_.privateKeyBytes = static_cast<uint16_t>(keyBytes);
    memcpy(stored_.certificate,
           certificateBuffer + sizeof(certificateBuffer) - certificateBytes,
           certificateBytes);
    memcpy(stored_.privateKey,
           keyBuffer + sizeof(keyBuffer) - keyBytes, keyBytes);
    ok = true;
  } while (false);
  mbedtls_x509write_crt_free(&certificate);
  mbedtls_pk_free(&key);
  return ok;
}

bool WirelessSyncIdentity::rotatePairing() {
  uint8_t uuid[16];
  esp_fill_random(uuid, sizeof(uuid));
  uuid[6] = static_cast<uint8_t>((uuid[6] & 0x0f) | 0x40);
  uuid[8] = static_cast<uint8_t>((uuid[8] & 0x3f) | 0x80);
  formatUuidBytes(uuid, stored_.pairingId);
  esp_fill_random(stored_.secret, sizeof(stored_.secret));
  stored_.paired = 0;
  return true;
}

bool WirelessSyncIdentity::persist() {
  if (!preferencesOpen_) return false;
  finalizeWirelessIdentityBlob(stored_);
  return preferences_.putBytes(kIdentityKey, &stored_, sizeof(stored_)) ==
      sizeof(stored_);
}

bool WirelessSyncIdentity::computeCertificateFingerprint() {
  if (!validateWirelessIdentityBlob(stored_)) return false;
  uint8_t digest[32];
  if (mbedtls_sha256(stored_.certificate, stored_.certificateBytes,
                     digest, false) != 0) {
    return false;
  }
  static constexpr char hex[] = "0123456789abcdef";
  for (size_t index = 0; index < sizeof(digest); ++index) {
    certificateSha256_[index * 2] = hex[digest[index] >> 4];
    certificateSha256_[index * 2 + 1] = hex[digest[index] & 0x0f];
  }
  certificateSha256_[64] = '\0';
  return true;
}

bool WirelessSyncIdentity::pairingBundle(bool rotate, String &json,
                                         String &error) {
  json = "";
  error = "";
  if (!ready_) {
    error = "wireless identity is unavailable";
    return false;
  }
  const time_t now = time(nullptr);
  if (now < 1704067200) {
    error = "set device time before wireless pairing";
    return false;
  }
  if (rotate) {
    const StoredWirelessSyncIdentity previous = stored_;
    if (!rotatePairing() || !persist()) {
      stored_ = previous;
      error = "pairing rotation failed";
      return false;
    }
  }
  if (!stored_.paired) {
    const StoredWirelessSyncIdentity previous = stored_;
    stored_.paired = 1;
    if (!persist()) {
      stored_ = previous;
      error = "pairing state save failed";
      return false;
    }
  }

  char expiresAt[25];
  formatIsoUtc(now + 120, expiresAt);
  cJSON *root = cJSON_CreateObject();
  cJSON_AddNumberToObject(root, "schemaVersion", 1);
  cJSON_AddStringToObject(root, "kind", "pairing-bundle");
  cJSON_AddStringToObject(root, "pairingId", stored_.pairingId);
  cJSON_AddStringToObject(root, "deviceId", deviceId_);
  cJSON_AddStringToObject(root, "displayName", "PokePod");
  cJSON_AddStringToObject(root, "platform", "pokepod");
  const std::string secret = base64UrlEncode(stored_.secret,
                                             sizeof(stored_.secret));
  cJSON_AddStringToObject(root, "secret", secret.c_str());
  cJSON_AddStringToObject(root, "certificateSha256", certificateSha256_);
  cJSON_AddStringToObject(root, "serviceType", kWirelessSyncServiceType);
  cJSON_AddStringToObject(root, "expiresAt", expiresAt);
  json = jsonText(root);
  cJSON_Delete(root);
  return !json.isEmpty();
}

}  // namespace pokepod
