#pragma once

#include <Arduino.h>

#include <algorithm>
#include <cstdlib>
#include <new>
#include <stdint.h>
#include <string.h>
#include <vector>

#ifdef ARDUINO
#include <esp_heap_caps.h>
#endif

namespace pokepod {

constexpr size_t kLinkManifestMaximumFiles = 2048;
constexpr size_t kLinkManifestMaximumReadBytes = 16U * 1024U;

enum class LinkManifestMode : uint8_t {
  none,
  recursiveRead,
  fingerprint,
};

enum class LinkManifestPhase : uint8_t {
  idle,
  scanning,
  sorting,
  processing,
  hashing,
  complete,
  failed,
  cancelled,
};

struct LinkManifestItem {
  String path;
  size_t length = 0;
  String sha256;
};

template <typename T>
class LinkManifestPsramAllocator {
 public:
  using value_type = T;

  LinkManifestPsramAllocator() noexcept = default;
  template <typename U>
  LinkManifestPsramAllocator(
      const LinkManifestPsramAllocator<U> &) noexcept {}

  T *allocate(size_t count) {
    if (count > static_cast<size_t>(-1) / sizeof(T)) std::abort();
#ifdef ARDUINO
    void *memory = heap_caps_malloc(count * sizeof(T),
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    void *memory = std::malloc(count * sizeof(T));
#endif
    if (memory == nullptr) std::abort();
    return static_cast<T *>(memory);
  }

  void deallocate(T *value, size_t) noexcept {
#ifdef ARDUINO
    heap_caps_free(value);
#else
    std::free(value);
#endif
  }

  template <typename U>
  struct rebind { using other = LinkManifestPsramAllocator<U>; };
};

template <typename T, typename U>
bool operator==(const LinkManifestPsramAllocator<T> &,
                const LinkManifestPsramAllocator<U> &) {
  return true;
}

template <typename T, typename U>
bool operator!=(const LinkManifestPsramAllocator<T> &,
                const LinkManifestPsramAllocator<U> &) {
  return false;
}

class LinkManifestPath {
 public:
  LinkManifestPath() = default;
  explicit LinkManifestPath(const String &value) { assign(value); }
  LinkManifestPath(const LinkManifestPath &) = delete;
  LinkManifestPath &operator=(const LinkManifestPath &) = delete;
  LinkManifestPath(LinkManifestPath &&other) noexcept
      : value_(other.value_), length_(other.length_) {
    other.value_ = nullptr;
    other.length_ = 0;
  }
  LinkManifestPath &operator=(LinkManifestPath &&other) noexcept {
    if (this != &other) {
      release();
      value_ = other.value_;
      length_ = other.length_;
      other.value_ = nullptr;
      other.length_ = 0;
    }
    return *this;
  }
  ~LinkManifestPath() { release(); }

  bool assign(const String &value) {
    release();
    if (value.isEmpty()) return false;
#ifdef ARDUINO
    value_ = static_cast<char *>(heap_caps_malloc(
        value.length() + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
#else
    value_ = static_cast<char *>(std::malloc(value.length() + 1));
#endif
    if (value_ == nullptr) return false;
    memcpy(value_, value.c_str(), value.length() + 1);
    length_ = value.length();
    return true;
  }

  const char *c_str() const { return value_ == nullptr ? "" : value_; }
  size_t length() const { return length_; }
  bool isEmpty() const { return length_ == 0; }
  bool endsWith(const char *suffix) const {
    if (suffix == nullptr) return false;
    const size_t suffixLength = strlen(suffix);
    return suffixLength <= length_ &&
        memcmp(c_str() + length_ - suffixLength, suffix, suffixLength) == 0;
  }

 private:
  void release() {
    if (value_ == nullptr) return;
#ifdef ARDUINO
    heap_caps_free(value_);
#else
    std::free(value_);
#endif
    value_ = nullptr;
    length_ = 0;
  }

  char *value_ = nullptr;
  size_t length_ = 0;
};

// Pure, bounded orchestration for the expensive pre-send side of Link v2.
// Filesystem handles and the SHA-256 context remain owned by the service, but
// every directory entry, merge-sort move and <=16 KiB file read is admitted by
// this state machine. This makes the production path cancellable between all
// physical storage operations without changing the wire schema.
class LinkManifestStepper {
 public:
  bool begin(LinkManifestMode mode, size_t cursor = 0,
             size_t pageFiles = 12) {
    if (active() || mode == LinkManifestMode::none || pageFiles == 0) {
      return false;
    }
#ifdef ARDUINO
    // The two service instances share 8 MiB PSRAM. Capacity is released by
    // reset(), and the coordinator permits only one active manifest. Check the
    // largest block before vector reserve so allocation failure is reported at
    // the request boundary instead of reaching the allocator's fatal fallback.
    constexpr size_t kMinimumManifestPsram = 48U * 1024U;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) <
        kMinimumManifestPsram) {
      return false;
    }
#endif
    paths_.clear();
    order_.clear();
    scratch_.clear();
    items_.clear();
    paths_.reserve(kLinkManifestMaximumFiles);
    order_.reserve(kLinkManifestMaximumFiles);
    scratch_.reserve(kLinkManifestMaximumFiles);
    items_.reserve(pageFiles);
    mode_ = mode;
    phase_ = LinkManifestPhase::scanning;
    cursor_ = cursor;
    pageFiles_ = pageFiles;
    processIndex_ = 0;
    processEnd_ = 0;
    currentLength_ = 0;
    currentHashedBytes_ = 0;
    fingerprint_ = 1469598103934665603ULL;
    return true;
  }

  bool addPath(const String &path) {
    if (phase_ != LinkManifestPhase::scanning || path.isEmpty() ||
        paths_.size() >= kLinkManifestMaximumFiles) {
      fail();
      return false;
    }
    LinkManifestPath stored(path);
    if (stored.isEmpty()) {
      fail();
      return false;
    }
    paths_.push_back(std::move(stored));
    order_.push_back(paths_.size() - 1);
    return true;
  }

  bool finishScan() {
    if (phase_ != LinkManifestPhase::scanning) return false;
    if (mode_ == LinkManifestMode::recursiveRead && cursor_ > paths_.size()) {
      fail();
      return false;
    }
    if (paths_.size() <= 1) {
      finishSort();
      return true;
    }
    scratch_.resize(order_.size());
    sortWidth_ = 1;
    sortLeft_ = 0;
    prepareMergeRun();
    phase_ = LinkManifestPhase::sorting;
    return true;
  }

  // Returns the number of individual merge moves performed. The caller fixes
  // a small budget per poll, so sorting a large directory cannot monopolize the
  // UI loop.
  size_t stepSort(size_t moveBudget) {
    if (phase_ != LinkManifestPhase::sorting || moveBudget == 0) return 0;
    size_t moved = 0;
    const size_t count = order_.size();
    while (phase_ == LinkManifestPhase::sorting && moved < moveBudget) {
      if (sortLeft_ >= count) {
        order_.swap(scratch_);
        sortWidth_ *= 2;
        if (sortWidth_ >= count) {
          finishSort();
          break;
        }
        sortLeft_ = 0;
        prepareMergeRun();
        continue;
      }

      size_t selected = 0;
      if (sortI_ >= sortMid_) {
        selected = order_[sortJ_++];
      } else if (sortJ_ >= sortRight_) {
        selected = order_[sortI_++];
      } else if (pathLessOrEqual(order_[sortI_], order_[sortJ_])) {
        selected = order_[sortI_++];
      } else {
        selected = order_[sortJ_++];
      }
      scratch_[sortOut_++] = selected;
      ++moved;
      if (sortOut_ >= sortRight_) {
        sortLeft_ += sortWidth_ * 2;
        prepareMergeRun();
      }
    }
    return moved;
  }

  bool currentPath(String &path) const {
    if (phase_ != LinkManifestPhase::processing ||
        processIndex_ >= processEnd_) {
      return false;
    }
    path = paths_[order_[processIndex_]].c_str();
    return true;
  }

  bool currentIsMetadata() const {
    return phase_ == LinkManifestPhase::processing &&
        processIndex_ < processEnd_ &&
        metadataPath(paths_[order_[processIndex_]]);
  }

  bool beginCurrentFile(size_t length) {
    if (phase_ != LinkManifestPhase::processing ||
        processIndex_ >= processEnd_) {
      return false;
    }
    currentLength_ = length;
    currentHashedBytes_ = 0;
    phase_ = LinkManifestPhase::hashing;
    if (mode_ == LinkManifestMode::fingerprint) {
      const LinkManifestPath &path = paths_[order_[processIndex_]];
      fingerprint_ = fnvUpdate(
          fingerprint_, reinterpret_cast<const uint8_t *>(path.c_str()),
          path.length());
    }
    return true;
  }

  bool acceptHashBytes(const uint8_t *bytes, size_t count) {
    if (phase_ != LinkManifestPhase::hashing ||
        count > kLinkManifestMaximumReadBytes ||
        currentHashedBytes_ + count > currentLength_ ||
        (count != 0 && bytes == nullptr)) {
      fail();
      return false;
    }
    if (mode_ == LinkManifestMode::fingerprint && count != 0) {
      fingerprint_ = fnvUpdate(fingerprint_, bytes, count);
    }
    currentHashedBytes_ += count;
    return true;
  }

  bool finishCurrentFile(const char *sha256 = nullptr) {
    if (phase_ != LinkManifestPhase::hashing ||
        currentHashedBytes_ != currentLength_) {
      fail();
      return false;
    }
    if (mode_ == LinkManifestMode::recursiveRead) {
      LinkManifestItem item;
      item.path = paths_[order_[processIndex_]].c_str();
      item.length = currentLength_;
      if (sha256 != nullptr) item.sha256 = sha256;
      items_.push_back(item);
    }
    advanceCurrent();
    return true;
  }

  bool finishCurrentWithoutHash(size_t length) {
    if (phase_ != LinkManifestPhase::processing ||
        mode_ != LinkManifestMode::recursiveRead) {
      return false;
    }
    LinkManifestItem item;
    item.path = paths_[order_[processIndex_]].c_str();
    item.length = length;
    items_.push_back(item);
    advanceCurrent();
    return true;
  }

  bool skipCurrent() {
    if (phase_ != LinkManifestPhase::processing) return false;
    advanceCurrent();
    return true;
  }

  void cancel() {
    if (active()) phase_ = LinkManifestPhase::cancelled;
  }

  void fail() { phase_ = LinkManifestPhase::failed; }

  void reset() {
    mode_ = LinkManifestMode::none;
    phase_ = LinkManifestPhase::idle;
    decltype(paths_) emptyPaths;
    decltype(order_) emptyOrder;
    decltype(scratch_) emptyScratch;
    decltype(items_) emptyItems;
    paths_.swap(emptyPaths);
    order_.swap(emptyOrder);
    scratch_.swap(emptyScratch);
    items_.swap(emptyItems);
    cursor_ = pageFiles_ = processIndex_ = processEnd_ = 0;
    currentLength_ = currentHashedBytes_ = 0;
  }

  bool active() const {
    return phase_ == LinkManifestPhase::scanning ||
        phase_ == LinkManifestPhase::sorting ||
        phase_ == LinkManifestPhase::processing ||
        phase_ == LinkManifestPhase::hashing;
  }
  bool complete() const { return phase_ == LinkManifestPhase::complete; }
  bool failed() const { return phase_ == LinkManifestPhase::failed; }
  bool cancelled() const { return phase_ == LinkManifestPhase::cancelled; }
  LinkManifestMode mode() const { return mode_; }
  LinkManifestPhase phase() const { return phase_; }
  size_t fileCount() const { return paths_.size(); }
  size_t cursor() const { return cursor_; }
  size_t nextCursor() const { return processEnd_; }
  bool hasNextPage() const {
    return mode_ == LinkManifestMode::recursiveRead &&
        processEnd_ < paths_.size();
  }
  size_t currentHashedBytes() const { return currentHashedBytes_; }
  const std::vector<LinkManifestItem> &items() const { return items_; }
  uint64_t fingerprint() const { return fingerprint_; }

 private:
  bool pathLessOrEqual(size_t left, size_t right) const {
    return strcmp(paths_[left].c_str(), paths_[right].c_str()) <= 0;
  }

  static bool metadataPath(const LinkManifestPath &path) {
    return path.endsWith("/capsule.json") ||
        path.endsWith("/processing.json") || path.endsWith("/raw.txt") ||
        path.endsWith("/polished.md") || path.endsWith("/final.md") ||
        path.endsWith("/trash.json");
  }

  static uint64_t fnvUpdate(uint64_t hash, const uint8_t *bytes,
                            size_t count) {
    for (size_t index = 0; index < count; ++index) {
      hash ^= bytes[index];
      hash *= 1099511628211ULL;
    }
    return hash;
  }

  void prepareMergeRun() {
    const size_t count = order_.size();
    sortMid_ = std::min(count, sortLeft_ + sortWidth_);
    sortRight_ = std::min(count, sortLeft_ + sortWidth_ * 2);
    sortI_ = sortLeft_;
    sortJ_ = sortMid_;
    sortOut_ = sortLeft_;
  }

  void finishSort() {
    processIndex_ = mode_ == LinkManifestMode::recursiveRead ? cursor_ : 0;
    processEnd_ = mode_ == LinkManifestMode::recursiveRead
        ? std::min(paths_.size(), cursor_ + pageFiles_)
        : paths_.size();
    phase_ = processIndex_ >= processEnd_
        ? LinkManifestPhase::complete
        : LinkManifestPhase::processing;
  }

  void advanceCurrent() {
    ++processIndex_;
    currentLength_ = 0;
    currentHashedBytes_ = 0;
    phase_ = processIndex_ >= processEnd_
        ? LinkManifestPhase::complete
        : LinkManifestPhase::processing;
  }

  LinkManifestMode mode_ = LinkManifestMode::none;
  LinkManifestPhase phase_ = LinkManifestPhase::idle;
  std::vector<LinkManifestPath,
              LinkManifestPsramAllocator<LinkManifestPath>> paths_;
  std::vector<size_t, LinkManifestPsramAllocator<size_t>> order_;
  std::vector<size_t, LinkManifestPsramAllocator<size_t>> scratch_;
  std::vector<LinkManifestItem> items_;
  size_t cursor_ = 0;
  size_t pageFiles_ = 0;
  size_t processIndex_ = 0;
  size_t processEnd_ = 0;
  size_t currentLength_ = 0;
  size_t currentHashedBytes_ = 0;
  size_t sortWidth_ = 0;
  size_t sortLeft_ = 0;
  size_t sortMid_ = 0;
  size_t sortRight_ = 0;
  size_t sortI_ = 0;
  size_t sortJ_ = 0;
  size_t sortOut_ = 0;
  uint64_t fingerprint_ = 1469598103934665603ULL;
};

}  // namespace pokepod
