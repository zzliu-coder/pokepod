#pragma once

namespace pokepod {

// Coalesces multiple repository mutations into one visible-index publication.
// The storage mutation still happens immediately; only sorting/filtering and
// the public revision bump are deferred.
class DeferredPublish {
 public:
  void begin() {
    active_ = true;
    pending_ = false;
  }

  bool request() {
    if (!active_) return true;
    pending_ = true;
    return false;
  }

  bool finish() {
    const bool publish = pending_;
    active_ = false;
    pending_ = false;
    return publish;
  }

  bool active() const { return active_; }

 private:
  bool active_ = false;
  bool pending_ = false;
};

}  // namespace pokepod
