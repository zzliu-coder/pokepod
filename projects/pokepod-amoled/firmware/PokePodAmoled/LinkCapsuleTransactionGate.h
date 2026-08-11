#pragma once

#include "CapsuleTransaction.h"
#include "LinkTransferGate.h"

namespace pokepod {

// Operation-scoped adapter. Service initialization never consults a wireless
// window that has not been armed. beginOperation() is called only after a Link
// request owns the transaction, and disconnect() latches cancellation without
// affecting USB operations in another service instance.
class LinkCapsuleTransactionGate final : public CapsuleTransactionGate {
 public:
  void beginOperation(LinkTransferGate *gate) {
    gate_ = gate;
    cancelled_ = false;
  }
  void cancel() { cancelled_ = true; }
  void reset() {
    gate_ = nullptr;
    cancelled_ = false;
  }
  bool permits(uint32_t nowMs) override {
    return !cancelled_ && linkTransferPermitted(gate_, nowMs);
  }

 private:
  LinkTransferGate *gate_ = nullptr;
  bool cancelled_ = false;
};

}  // namespace pokepod
