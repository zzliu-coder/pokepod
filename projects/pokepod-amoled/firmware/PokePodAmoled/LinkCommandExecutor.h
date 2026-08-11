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
    finalize,
    rollback,
    result,
    cleanup,
    finished,
  };

  enum class Action : uint8_t {
    none,
    checkpoint,
    preflightItem,
    applyItem,
    finalizeApply,
    rollbackFinalize,
    rollbackItem,
    persistResult,
    cleanup,
    finish,
  };

  struct Work {
    Action action = Action::none;
    size_t index = 0;
  };

  bool begin(size_t itemCount, bool needsFinalize = false) {
    if (active() || itemCount > kMaximumItems) return false;
    total_ = itemCount;
    cursor_ = 0;
    applied_ = 0;
    needsFinalize_ = needsFinalize;
    phase_ = itemCount == 0
        ? (needsFinalize ? Phase::finalize : Phase::result)
        : Phase::preflight;
    success_ = itemCount == 0;
    rollbackFailed_ = false;
    responseAllowed_ = true;
    deadlineObserved_ = false;
    awaitingCompletion_ = false;
    step_ = Step::ready;
    completedAction_ = Action::none;
    checkpointFailures_ = 0;
    cleanupFailures_ = 0;
    lastMutation_ = Action::none;
    preserveJournal_ = false;
    finalizeApplied_ = false;
    finalizeRollbackPending_ = false;
    resultFailures_ = 0;
    return true;
  }

  bool beginRejected() {
    if (!begin(0, false)) return false;
    success_ = false;
    return true;
  }

  bool resume(size_t itemCount, Phase phase, size_t cursor, size_t applied,
              bool success, bool rollbackFailed,
              bool needsFinalize = false, bool finalizeApplied = false) {
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
    step_ = Step::ready;
    completedAction_ = Action::none;
    checkpointFailures_ = 0;
    cleanupFailures_ = 0;
    lastMutation_ = Action::none;
    preserveJournal_ = false;
    needsFinalize_ = needsFinalize;
    finalizeApplied_ = finalizeApplied;
    finalizeRollbackPending_ = phase == Phase::rollback && finalizeApplied;
    resultFailures_ = 0;
    return true;
  }

  Work poll(bool foregroundPermitted) {
    if (awaitingCompletion_) return {};
    if (!foregroundPermitted && active()) {
      responseAllowed_ = false;
      deadlineObserved_ = true;
    }
    if (!foregroundPermitted &&
        (phase_ == Phase::preflight || phase_ == Phase::apply ||
         phase_ == Phase::finalize)) {
      success_ = false;
      enterRollbackOrResult();
    }

    Work work;
    if (step_ == Step::checkpointAfter) {
      work = {Action::checkpoint, cursor_};
      awaitingCompletion_ = true;
      completedAction_ = work.action;
      return work;
    }
    switch (phase_) {
      case Phase::preflight:
        work = {Action::preflightItem, cursor_};
        break;
      case Phase::apply:
        work = itemWork(Action::applyItem, cursor_);
        break;
      case Phase::finalize:
        work = itemWork(Action::finalizeApply, total_);
        break;
      case Phase::rollback:
        work = finalizeRollbackPending_
            ? itemWork(Action::rollbackFinalize, total_)
            : itemWork(Action::rollbackItem, cursor_ - 1);
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
    completedAction_ = work.action;
    return work;
  }

  void completeStep(bool ok) {
    if (!awaitingCompletion_) return;
    awaitingCompletion_ = false;
    const Action completed = completedAction_;
    completedAction_ = Action::none;
    if (completed == Action::checkpoint) {
      if (!ok) {
        if (++checkpointFailures_ < kMaximumCheckpointFailures) return;
        success_ = false;
        responseAllowed_ = false;
        if (step_ == Step::checkpointAfter &&
            (lastMutation_ == Action::applyItem ||
             lastMutation_ == Action::finalizeApply)) {
          enterRollbackOrResult();
        } else if (phase_ == Phase::apply || phase_ == Phase::finalize) {
          enterRollbackOrResult();
        } else {
          // A rollback whose durable post-checkpoint cannot be written leaves
          // the previous in-flight checkpoint as reboot authority. Local
          // cleanup may release runtime handles but must not erase the journal.
          rollbackFailed_ = true;
          preserveJournal_ = true;
          phase_ = Phase::cleanup;
          step_ = Step::ready;
        }
      } else {
        checkpointFailures_ = 0;
        step_ = step_ == Step::checkpointBefore
            ? Step::execute : Step::ready;
        if (step_ == Step::ready) lastMutation_ = Action::none;
      }
      return;
    }
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
          step_ = Step::checkpointAfter;
        }
        return;
      case Phase::apply:
        lastMutation_ = Action::applyItem;
        if (!ok) {
          // An item can mutate more than one artifact (for example rename a
          // capsule and then update its metadata).  A failed adapter step is
          // therefore conservatively treated as an uncertain applied item so
          // live rollback matches the power-cut recovery rule.
          if (applied_ < total_) ++applied_;
          success_ = false;
          enterRollbackOrResult();
          step_ = Step::checkpointAfter;
          return;
        }
        ++cursor_;
        ++applied_;
        if (cursor_ == total_) {
          success_ = true;
          phase_ = needsFinalize_ ? Phase::finalize : Phase::result;
        }
        step_ = Step::checkpointAfter;
        return;
      case Phase::finalize:
        lastMutation_ = Action::finalizeApply;
        if (!ok) {
          // Like an item adapter, finalize is a multi-poll operation whose
          // last primitive may have succeeded before a later observation
          // failed. Conservatively restore the final artifact.
          finalizeApplied_ = true;
          success_ = false;
          enterRollbackOrResult();
        } else {
          finalizeApplied_ = true;
          success_ = true;
          phase_ = Phase::result;
        }
        step_ = Step::checkpointAfter;
        return;
      case Phase::rollback:
        lastMutation_ = completed;
        if (completed == Action::rollbackFinalize) {
          finalizeRollbackPending_ = false;
          if (ok) {
            finalizeApplied_ = false;
          } else {
            rollbackFailed_ = true;
            preserveJournal_ = true;
          }
        } else {
          if (!ok) {
            rollbackFailed_ = true;
            preserveJournal_ = true;
          }
          if (cursor_ > 0) --cursor_;
        }
        if (!finalizeRollbackPending_ && cursor_ == 0) {
          phase_ = Phase::result;
        }
        step_ = Step::checkpointAfter;
        return;
      case Phase::result:
        if (ok) {
          resultFailures_ = 0;
          phase_ = Phase::cleanup;
        } else if (++resultFailures_ >= kMaximumResultFailures) {
          // A mutation without its result is still recoverable from the
          // durable journal. Release live resources after bounded retries,
          // retain that authority, and never acknowledge the upload.
          responseAllowed_ = false;
          preserveJournal_ = true;
          phase_ = Phase::cleanup;
        }
        return;
      case Phase::cleanup:
        if (ok) {
          cleanupFailures_ = 0;
          phase_ = Phase::finished;
        } else if (++cleanupFailures_ >= kMaximumCleanupFailures) {
          // Runtime resources must not remain wedged forever by a permanent
          // remove/rmdir failure. The durable journal remains boot recovery
          // authority; the service releases its live reservation at finish.
          preserveJournal_ = true;
          responseAllowed_ = false;
          phase_ = Phase::finished;
        }
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
    step_ = Step::ready;
    completedAction_ = Action::none;
    checkpointFailures_ = 0;
    cleanupFailures_ = 0;
    lastMutation_ = Action::none;
    preserveJournal_ = false;
    needsFinalize_ = false;
    finalizeApplied_ = false;
    finalizeRollbackPending_ = false;
    resultFailures_ = 0;
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
  bool checkpointMarksItemInFlight() const {
    return completedAction_ == Action::checkpoint &&
        step_ == Step::checkpointBefore;
  }
  bool preserveJournal() const { return preserveJournal_; }
  bool finalizeApplied() const { return finalizeApplied_; }

  static constexpr size_t kMaximumItems = 500;
  static constexpr size_t kMaximumBytesPerPoll = 16U * 1024U;

 private:
  enum class Step : uint8_t { ready, checkpointBefore, execute,
                              checkpointAfter };

  Work itemWork(Action action, size_t index) {
    if (step_ == Step::ready) step_ = Step::checkpointBefore;
    if (step_ == Step::checkpointBefore ||
        step_ == Step::checkpointAfter) {
      return {Action::checkpoint, index};
    }
    return {action, index};
  }

  void enterRollbackOrResult() {
    finalizeRollbackPending_ = finalizeApplied_;
    cursor_ = applied_;
    phase_ = cursor_ == 0 && !finalizeRollbackPending_
        ? Phase::result : Phase::rollback;
    step_ = Step::ready;
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
  Step step_ = Step::ready;
  Action completedAction_ = Action::none;
  uint8_t checkpointFailures_ = 0;
  uint8_t cleanupFailures_ = 0;
  Action lastMutation_ = Action::none;
  bool preserveJournal_ = false;
  bool needsFinalize_ = false;
  bool finalizeApplied_ = false;
  bool finalizeRollbackPending_ = false;
  uint8_t resultFailures_ = 0;
  static constexpr uint8_t kMaximumCheckpointFailures = 3;
  static constexpr uint8_t kMaximumCleanupFailures = 3;
  static constexpr uint8_t kMaximumResultFailures = 3;
};

// Wire adapter name retained for the Link service without making the core
// policy depend on a socket, request ID, or command JSON representation.
using LinkCommandExecutor = CapsuleBatchExecutor;

}  // namespace pokepod
