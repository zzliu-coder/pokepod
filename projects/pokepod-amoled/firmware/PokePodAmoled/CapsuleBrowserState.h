#pragma once

#include <stddef.h>
#include <stdint.h>
#include <algorithm>
#include <string>
#include <vector>

namespace pokepod {

enum class CapsuleScope : uint8_t {
  inbox,
  favorites,
  pending,
  failed,
  archive,
  trash,
};

constexpr CapsuleScope nextCapsuleScope(CapsuleScope scope) {
  const uint8_t next = (static_cast<uint8_t>(scope) + 1) % 6;
  return static_cast<CapsuleScope>(next);
}

constexpr bool capsuleVisibleInScope(CapsuleScope scope, bool favorite,
                                     bool pending, bool failed,
                                     bool archived, bool trashed) {
  switch (scope) {
    case CapsuleScope::inbox: return !archived && !trashed;
    case CapsuleScope::favorites: return favorite && !trashed;
    case CapsuleScope::pending: return pending && !archived && !trashed;
    case CapsuleScope::failed: return failed && !archived && !trashed;
    case CapsuleScope::archive: return archived && !trashed;
    case CapsuleScope::trash: return trashed;
  }
  return false;
}

struct CapsuleRollbackCounts {
  size_t attempted = 0;
  size_t failed = 0;

  void record(bool success) {
    ++attempted;
    if (!success) ++failed;
  }
  bool fullyRolledBack() const { return attempted > 0 && failed == 0; }
};

class CapsuleBrowserState {
 public:
  static constexpr size_t kMaximumItems = 512;
  static constexpr uint32_t kLongPressMs = 500;
  static constexpr uint32_t kUndoMs = 5000;

  void setScope(CapsuleScope scope) {
    scope_ = scope;
    clearSelection();
    offset_ = 0;
  }
  CapsuleScope scope() const { return scope_; }
  void setOffset(size_t offset) { offset_ = offset; }
  size_t offset() const { return offset_; }
  bool longPressReady(uint32_t elapsedMs, int16_t maxX, int16_t maxY) const {
    const int16_t ax = maxX < 0 ? -maxX : maxX;
    const int16_t ay = maxY < 0 ? -maxY : maxY;
    return elapsedMs >= kLongPressMs && ax < 10 && ay < 10;
  }
  bool toggle(const char *capsuleId, bool mutableItem) {
    if (!mutableItem || capsuleId == nullptr || capsuleId[0] == '\0') {
      return false;
    }
    const std::string id(capsuleId);
    const auto existing = std::find(selectedIds_.begin(), selectedIds_.end(), id);
    if (existing == selectedIds_.end()) {
      if (selectedIds_.size() >= kMaximumItems) return false;
      selectedIds_.push_back(id);
    } else {
      selectedIds_.erase(existing);
    }
    selectionMode_ = !selectedIds_.empty();
    return true;
  }
  void clearSelection() {
    selectedIds_.clear();
    selectionMode_ = false;
  }
  bool selected(const char *capsuleId) const {
    if (capsuleId == nullptr) return false;
    return std::find(selectedIds_.begin(), selectedIds_.end(), capsuleId) !=
        selectedIds_.end();
  }
  void retainKnown(const std::vector<std::string> &knownIds) {
    selectedIds_.erase(
        std::remove_if(selectedIds_.begin(), selectedIds_.end(),
                       [&knownIds](const std::string &selectedId) {
                         return std::find(knownIds.begin(), knownIds.end(),
                                          selectedId) == knownIds.end();
                       }),
        selectedIds_.end());
    selectionMode_ = !selectedIds_.empty();
  }
  bool selectionMode() const { return selectionMode_; }
  size_t selectedCount() const { return selectedIds_.size(); }
  const std::vector<std::string> &selectedIds() const { return selectedIds_; }
  void focus(const char *capsuleId) {
    focusedId_ = capsuleId == nullptr ? std::string() : std::string(capsuleId);
  }
  void clearFocus() { focusedId_.clear(); }
  const std::string &focusedId() const { return focusedId_; }
  void retainFocused(const std::vector<std::string> &knownIds) {
    if (!focusedId_.empty() &&
        std::find(knownIds.begin(), knownIds.end(), focusedId_) ==
            knownIds.end()) {
      focusedId_.clear();
    }
  }
  bool rootSwipeLocked() const { return selectionMode_; }
  void beginUndo(uint32_t nowMs) { undoDeadlineMs_ = nowMs + kUndoMs; }
  bool undoAvailable(uint32_t nowMs) const {
    return static_cast<int32_t>(undoDeadlineMs_ - nowMs) > 0;
  }
  void clearUndo() { undoDeadlineMs_ = 0; }

 private:
  CapsuleScope scope_ = CapsuleScope::inbox;
  size_t offset_ = 0;
  std::vector<std::string> selectedIds_;
  std::string focusedId_;
  bool selectionMode_ = false;
  uint32_t undoDeadlineMs_ = 0;
};

}  // namespace pokepod
