#pragma once

#include <stddef.h>
#include <stdint.h>

namespace pokepod {

// Cooperative capsule batch sequencing policy.  The caller performs exactly one
// returned action and calls completeStep() before the next poll.  Keeping the
// policy free of transport, filesystem, UI, and JSON types lets Link and the
// local capsule browser share the same transaction semantics.
class CapsuleBatchExecutor {
 public:
  enum class Phase : uint8_t {
    idle,
    preflight,
    apply,
    rollback,
    result,
    cleanup,
    finished,
  };

  enum class Action : uint8_t {
    none,
    preflightItem,
    applyItem,
    rollbackItem,
    persistResult,
    cleanup,
    finish,
  };

  struct Work {
    Action action = Action::none;
    size_t index = 0;
  };

  bool begin(size_t itemCount) {
    if (active() || itemCount > kMaximumItems) return false;
    total_ = itemCount;
    cursor_ = 0;
    applied_ = 0;
    phase_ = itemCount == 0 ? Phase::result : Phase::preflight;
    success_ = false;
    rollbackFailed_ = false;
    responseAllowed_ = true;
    deadlineObserved_ = false;
    awaitingCompletion_ = false;
    return true;
  }

  bool resume(size_t itemCount, Phase phase, size_t cursor, size_t applied,
              bool success, bool rollbackFailed) {
    if (active() || itemCount > kMaximumItems || cursor > itemCount ||
        applied > itemCount || phase == Phase::idle ||
        phase == Phase::finished) return false;
    total_ = itemCount;
    cursor_ = cursor;
    applied_ = applied;
    phase_ = phase;
    success_ = success;
    rollbackFailed_ = rollbackFailed;
    responseAllowed_ = false;
    deadlineObserved_ = false;
    awaitingCompletion_ = false;
    return true;
  }

  Work poll(bool foregroundPermitted) {
    if (awaitingCompletion_) return {};
    if (!foregroundPermitted && active()) {
      responseAllowed_ = false;
      deadlineObserved_ = true;
    }
    if (!foregroundPermitted &&
        (phase_ == Phase::preflight || phase_ == Phase::apply)) {
      success_ = false;
      enterRollbackOrResult();
    }

    Work work;
    switch (phase_) {
      case Phase::preflight:
        work = {Action::preflightItem, cursor_};
        break;
      case Phase::apply:
        work = {Action::applyItem, cursor_};
        break;
      case Phase::rollback:
        work = {Action::rollbackItem, cursor_ - 1};
        break;
      case Phase::result:
        work = {Action::persistResult, 0};
        break;
      case Phase::cleanup:
        work = {Action::cleanup, 0};
        break;
      case Phase::finished:
        work = {Action::finish, 0};
        break;
      case Phase::idle:
        break;
    }
    awaitingCompletion_ = work.action != Action::none;
    return work;
  }

  void completeStep(bool ok) {
    if (!awaitingCompletion_) return;
    awaitingCompletion_ = false;
    switch (phase_) {
      case Phase::preflight:
        if (!ok) {
          success_ = false;
          phase_ = Phase::result;
          return;
        }
        if (++cursor_ == total_) {
          cursor_ = 0;
          phase_ = Phase::apply;
        }
        return;
      case Phase::apply:
        if (!ok) {
          success_ = false;
          enterRollbackOrResult();
          return;
        }
        ++cursor_;
        ++applied_;
        if (cursor_ == total_) {
          success_ = true;
          phase_ = Phase::result;
        }
        return;
      case Phase::rollback:
        if (!ok) rollbackFailed_ = true;
        if (cursor_ > 0) --cursor_;
        if (cursor_ == 0) phase_ = Phase::result;
        return;
      case Phase::result:
        // A result write failure still enters cleanup; the durable command
        // file remains the recovery authority and no success response is sent.
        if (!ok) {
          success_ = false;
          responseAllowed_ = false;
        }
        phase_ = Phase::cleanup;
        return;
      case Phase::cleanup:
        if (ok) phase_ = Phase::finished;
        return;
      case Phase::finished:
        reset();
        return;
      case Phase::idle:
        return;
    }
  }

  void disconnect() { responseAllowed_ = false; }

  void reset() {
    phase_ = Phase::idle;
    total_ = cursor_ = applied_ = 0;
    success_ = false;
    rollbackFailed_ = false;
    responseAllowed_ = false;
    deadlineObserved_ = false;
    awaitingCompletion_ = false;
  }

  bool active() const { return phase_ != Phase::idle; }
  bool recovering() const {
    return phase_ == Phase::rollback || phase_ == Phase::cleanup;
  }
  Phase phase() const { return phase_; }
  size_t applied() const { return applied_; }
  size_t cursor() const { return cursor_; }
  size_t total() const { return total_; }
  bool success() const { return success_ && !rollbackFailed_; }
  bool rollbackFailed() const { return rollbackFailed_; }
  bool responseAllowed() const { return responseAllowed_; }
  bool deadlineObserved() const { return deadlineObserved_; }

  static constexpr size_t kMaximumItems = 500;
  static constexpr size_t kMaximumBytesPerPoll = 16U * 1024U;

 private:
  void enterRollbackOrResult() {
    cursor_ = applied_;
    phase_ = cursor_ == 0 ? Phase::result : Phase::rollback;
  }

  Phase phase_ = Phase::idle;
  size_t total_ = 0;
  size_t cursor_ = 0;
  size_t applied_ = 0;
  bool success_ = false;
  bool rollbackFailed_ = false;
  bool responseAllowed_ = false;
  bool deadlineObserved_ = false;
  bool awaitingCompletion_ = false;
};

// Wire adapter name retained for the Link service without making the core
// policy depend on a socket, request ID, or command JSON representation.
using LinkCommandExecutor = CapsuleBatchExecutor;

}  // namespace pokepod
