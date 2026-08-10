#pragma once

#include <stddef.h>

namespace pokepod {

constexpr size_t kPlaybackReadAheadBytes = 1024;
constexpr size_t kPlaybackFeedBytes = 128;

inline size_t playbackReadSize(size_t fileRemaining) {
  return fileRemaining < kPlaybackReadAheadBytes
      ? fileRemaining : kPlaybackReadAheadBytes;
}

inline size_t playbackFeedSize(size_t bufferedRemaining) {
  size_t wanted = bufferedRemaining < kPlaybackFeedBytes
      ? bufferedRemaining : kPlaybackFeedBytes;
  return wanted - wanted % 2;
}

}  // namespace pokepod
