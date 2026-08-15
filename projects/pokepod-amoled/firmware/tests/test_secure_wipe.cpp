#include <assert.h>
#include <stdint.h>
#include <string>

#include "../PokePodAmoled/DeviceSecretWipe.h"

using namespace pokepod;

namespace {
struct TestSettings {
  String wifiSsid;
  String wifiPassword;
  String secretId;
  String secretKey;
  String hotwordId;
};
}  // namespace

int main() {
  uint8_t bytes[] = {1, 2, 3, 4};
  secureWipeBytes(bytes, sizeof(bytes));
  for (uint8_t value : bytes) assert(value == 0);

  std::string standard = "task-local-secret";
  secureWipe(standard);
  assert(standard.empty());

  String arduino = "candidate-password";
  secureWipe(arduino);
  assert(arduino.isEmpty());

  TestSettings settings;
  settings.wifiSsid = "kept-ssid";
  settings.wifiPassword = "wifi-secret";
  settings.secretId = "cloud-id";
  settings.secretKey = "cloud-key";
  settings.hotwordId = "kept-hotword";
  secureWipeSecrets(settings);
  assert(settings.wifiSsid == String("kept-ssid"));
  assert(settings.hotwordId == String("kept-hotword"));
  assert(settings.wifiPassword.isEmpty());
  assert(settings.secretId.isEmpty());
  assert(settings.secretKey.isEmpty());
  return 0;
}
