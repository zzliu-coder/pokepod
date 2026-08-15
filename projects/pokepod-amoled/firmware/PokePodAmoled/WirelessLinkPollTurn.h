#pragma once

#include <stdint.h>

namespace pokepod {

// One WirelessSyncService::poll owns exactly one Link scheduling turn. An
// authenticated turn executes the full Link poll (which includes cleanup);
// every other exit executes cleanup alone. This prevents two independent
// Link budgets from advancing the same continuation in one outer Wi-Fi poll.
template <typename LinkService>
class WirelessLinkPollTurn {
 public:
  explicit WirelessLinkPollTurn(LinkService &link) : link_(link) {}
  ~WirelessLinkPollTurn() {
    if (!fullPollRan_) link_.pollDeferredCleanup();
  }

  WirelessLinkPollTurn(const WirelessLinkPollTurn &) = delete;
  WirelessLinkPollTurn &operator=(const WirelessLinkPollTurn &) = delete;

  void pollAuthenticated(uint32_t nowMs) {
    if (fullPollRan_) return;
    fullPollRan_ = true;
    link_.poll(nowMs);
  }

  bool fullPollRan() const { return fullPollRan_; }

 private:
  LinkService &link_;
  bool fullPollRan_ = false;
};

}  // namespace pokepod
