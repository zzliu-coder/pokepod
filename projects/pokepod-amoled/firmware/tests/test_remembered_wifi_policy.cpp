#include <cassert>
#include <string>
#include <vector>

#include "../PokePodAmoled/RememberedWifiPolicy.h"

namespace {
struct Credential {
  std::string ssid;
  std::string password;
};
}

int main() {
  using namespace pokepod;
  std::vector<Credential> networks;
  rememberNetwork(networks, std::string("home"), std::string("one"), 5);
  rememberNetwork(networks, std::string("phone"), std::string("two"), 5);
  assert(networks.size() == 2);
  assert(networks[0].ssid == "phone" && networks[1].ssid == "home");

  rememberNetwork(networks, std::string("home"), std::string("updated"), 5);
  assert(networks.size() == 2);
  assert(networks[0].ssid == "home" && networks[0].password == "updated");

  for (int index = 0; index < 6; ++index) {
    rememberNetwork(networks, std::string("extra") + std::to_string(index),
                    std::string("pass"), 5);
  }
  assert(networks.size() == 5);
  assert(networks.front().ssid == "extra5");
  assert(networks.back().ssid == "extra1");
  assert(forgetNetwork(networks, std::string("extra3")));
  assert(!forgetNetwork(networks, std::string("missing")));

  std::vector<WifiCandidateScore> candidates = {
      {0, -30, false}, {1, -72, true}, {2, -48, true}, {3, -128, false}};
  rankWifiCandidates(candidates);
  assert(candidates[0].index == 2);
  assert(candidates[1].index == 1);
  assert(candidates[2].index == 0);
  assert(candidates[3].index == 3);
  return 0;
}
