#include <cassert>
#include <string>

#include "../PokePodAmoled/Tc3Policy.h"

int main() {
  using namespace pokepod;
  static_assert(tencentUploadDeadlineMs(0) == 20000);
  static_assert(tencentUploadDeadlineMs(230400) == 20000);
  static_assert(tencentUploadDeadlineMs(2496000) == 107000);
  static_assert(tencentUploadDeadlineMs(3 * 1024 * 1024) == 120000);
  const std::string payloadHash =
      "35e9c5b0e3ae67532d3c9f17ead6c90222632e5b1ff7f6e89887f1398934f064";
  const std::string canonical = tc3CanonicalRequest<std::string>(
      "POST", "/", "", "application/json; charset=utf-8",
      "cvm.tencentcloudapi.com", "describeinstances", payloadHash);
  assert(canonical ==
      "POST\n/\n\n"
      "content-type:application/json; charset=utf-8\n"
      "host:cvm.tencentcloudapi.com\n"
      "x-tc-action:describeinstances\n\n"
      "content-type;host;x-tc-action\n"
      "35e9c5b0e3ae67532d3c9f17ead6c90222632e5b1ff7f6e89887f1398934f064");

  const std::string scope =
      tc3CredentialScope<std::string>("2019-02-25", "cvm");
  assert(scope == "2019-02-25/cvm/tc3_request");
  const std::string canonicalHash =
      "7019a55be8395899b900fb5564e4200d984910f34794a27cb3fb7d10ff6a1e84";
  assert(tc3StringToSign<std::string>("1551113065", scope, canonicalHash) ==
      "TC3-HMAC-SHA256\n1551113065\n2019-02-25/cvm/tc3_request\n"
      "7019a55be8395899b900fb5564e4200d984910f34794a27cb3fb7d10ff6a1e84");
  return 0;
}
