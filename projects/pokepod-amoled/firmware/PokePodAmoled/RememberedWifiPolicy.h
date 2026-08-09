#pragma once

#include <algorithm>
#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace pokepod {

template <typename Credential, typename Text>
inline void rememberNetwork(std::vector<Credential> &networks,
                            const Text &ssid, const Text &password,
                            size_t maximum) {
  networks.erase(std::remove_if(networks.begin(), networks.end(),
      [&ssid](const Credential &network) { return network.ssid == ssid; }),
      networks.end());
  networks.insert(networks.begin(), Credential{ssid, password});
  if (networks.size() > maximum) networks.resize(maximum);
}

template <typename Credential, typename Text>
inline bool forgetNetwork(std::vector<Credential> &networks,
                          const Text &ssid) {
  const size_t before = networks.size();
  networks.erase(std::remove_if(networks.begin(), networks.end(),
      [&ssid](const Credential &network) { return network.ssid == ssid; }),
      networks.end());
  return networks.size() != before;
}

struct WifiCandidateScore {
  uint8_t index = 0;
  int32_t rssi = -128;
  bool visible = false;
};

inline void rankWifiCandidates(std::vector<WifiCandidateScore> &candidates) {
  std::stable_sort(candidates.begin(), candidates.end(),
      [](const WifiCandidateScore &left, const WifiCandidateScore &right) {
        if (left.visible != right.visible) return left.visible > right.visible;
        return left.rssi > right.rssi;
      });
}

}  // namespace pokepod
