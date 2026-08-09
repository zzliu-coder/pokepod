#pragma once

#include <stdint.h>

namespace pokepod {

enum class LinkTransport : uint8_t { none, usb, wifi };

class LinkServiceCoordinator {
 public:
  bool acquire(LinkTransport transport) {
    if (transport == LinkTransport::none) return false;
    if (owner_ != LinkTransport::none && owner_ != transport) return false;
    owner_ = transport;
    return true;
  }

  void release(LinkTransport transport) {
    if (owner_ == transport) owner_ = LinkTransport::none;
  }

  bool busyFor(LinkTransport transport) const {
    return owner_ != LinkTransport::none && owner_ != transport;
  }
  LinkTransport owner() const { return owner_; }

 private:
  LinkTransport owner_ = LinkTransport::none;
};

}  // namespace pokepod
