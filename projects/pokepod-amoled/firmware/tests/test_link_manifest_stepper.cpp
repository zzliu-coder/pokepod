#include <assert.h>
#include <stdint.h>
#include <stdio.h>

#include <array>
#include <string>

#include "../PokePodAmoled/LinkManifestStepper.h"
#include "../PokePodAmoled/LinkTransferGate.h"

using namespace pokepod;

namespace {

constexpr size_t kSlowAudioBytes = 1800U * 1024U;

void finishSort(LinkManifestStepper &stepper) {
  while (stepper.phase() == LinkManifestPhase::sorting) {
    const size_t moved = stepper.stepSort(32);
    assert(moved <= 32);
  }
}

void seedSixtyFourAudioFiles(LinkManifestStepper &stepper) {
  char path[96];
  for (int index = 63; index >= 0; --index) {
    snprintf(path, sizeof(path), "Inbox/id-%02d/audio.wav", index);
    assert(stepper.addPath(String(path)));
  }
  assert(stepper.finishScan());
  finishSort(stepper);
}

void driveCurrentAudio(LinkManifestStepper &stepper,
                       size_t &maximumRead, size_t &polls) {
  String path;
  assert(stepper.currentPath(path));
  assert(path.endsWith("audio.wav"));
  assert(stepper.beginCurrentFile(kSlowAudioBytes));
  std::array<uint8_t, kLinkManifestMaximumReadBytes> bytes{};
  while (stepper.phase() == LinkManifestPhase::hashing) {
    const size_t remaining = kSlowAudioBytes - stepper.currentHashedBytes();
    const size_t count =
        std::min(remaining, kLinkManifestMaximumReadBytes);
    maximumRead = std::max(maximumRead, count);
    assert(stepper.acceptHashBytes(bytes.data(), count));
    ++polls;
    if (stepper.currentHashedBytes() == kSlowAudioBytes) {
      assert(stepper.finishCurrentFile(
          "0000000000000000000000000000000000000000000000000000000000000000"));
    }
  }
}

void testLargeSlowDirectoryIsAlwaysBounded() {
  LinkManifestStepper stepper;
  assert(stepper.begin(LinkManifestMode::recursiveRead, 0, 12));
  seedSixtyFourAudioFiles(stepper);
  size_t maximumRead = 0;
  size_t polls = 0;
  while (stepper.active()) driveCurrentAudio(stepper, maximumRead, polls);
  assert(stepper.complete());
  assert(stepper.items().size() == 12);
  assert(stepper.hasNextPage());
  assert(stepper.nextCursor() == 12);
  assert(maximumRead == kLinkManifestMaximumReadBytes);
  assert(polls > 1300);
  assert(stepper.items().front().path == String("Inbox/id-00/audio.wav"));
}

void testFiveHundredTwelvePathIndexSortsIncrementally() {
  LinkManifestStepper stepper;
  assert(stepper.begin(LinkManifestMode::recursiveRead, 500, 12));
  char path[96];
  for (int index = 511; index >= 0; --index) {
    snprintf(path, sizeof(path), "Inbox/id-%03d/processing.json", index);
    assert(stepper.addPath(String(path)));
  }
  assert(stepper.finishScan());
  size_t sortPolls = 0;
  while (stepper.phase() == LinkManifestPhase::sorting) {
    assert(stepper.stepSort(16) <= 16);
    ++sortPolls;
  }
  assert(sortPolls > 100);
  String current;
  assert(stepper.currentPath(current));
  assert(current == String("Inbox/id-500/processing.json"));
}

void testCapacityOverflowFailsWithoutPartialManifest() {
  LinkManifestStepper stepper;
  assert(stepper.begin(LinkManifestMode::recursiveRead));
  char path[96];
  for (size_t index = 0; index < kLinkManifestMaximumFiles; ++index) {
    snprintf(path, sizeof(path), "Inbox/id-%04zu/capsule.json", index);
    assert(stepper.addPath(String(path)));
  }
  assert(!stepper.addPath("Inbox/overflow/capsule.json"));
  assert(stepper.failed());
  assert(!stepper.complete());
  assert(stepper.items().empty());
}

void testFingerprintOnlyHashesMetadata() {
  LinkManifestStepper stepper;
  assert(stepper.begin(LinkManifestMode::fingerprint));
  assert(stepper.addPath("Inbox/b/audio.wav"));
  assert(stepper.addPath("Inbox/a/raw.txt"));
  assert(stepper.addPath("Inbox/a/capsule.json"));
  assert(stepper.finishScan());
  finishSort(stepper);

  const uint8_t bytes[] = {'{', '}', '\n'};
  size_t hashed = 0;
  while (stepper.active()) {
    if (!stepper.currentIsMetadata()) {
      assert(stepper.skipCurrent());
      continue;
    }
    assert(stepper.beginCurrentFile(sizeof(bytes)));
    assert(stepper.acceptHashBytes(bytes, sizeof(bytes)));
    hashed += sizeof(bytes);
    assert(stepper.finishCurrentFile());
  }
  assert(stepper.complete());
  assert(hashed == sizeof(bytes) * 2);
  assert(stepper.fingerprint() != 1469598103934665603ULL);
}

void testWiFiDeadlineCancelsAtExactlyFiveMinutes() {
  LinkManifestStepper stepper;
  assert(stepper.begin(LinkManifestMode::recursiveRead, 0, 12));
  seedSixtyFourAudioFiles(stepper);
  AbsoluteLinkDeadlineGate gate;
  gate.arm(0, 300000);
  String path;
  assert(stepper.currentPath(path));
  assert(stepper.beginCurrentFile(kSlowAudioBytes));
  std::array<uint8_t, kLinkManifestMaximumReadBytes> bytes{};
  uint32_t nowMs = 299000;
  while (nowMs < 300000) {
    assert(gate.permits(nowMs));
    assert(stepper.acceptHashBytes(bytes.data(), bytes.size()));
    nowMs += 100;
  }
  assert(!gate.permits(300000));
  stepper.cancel();
  assert(stepper.cancelled());
  assert(stepper.currentHashedBytes() < kSlowAudioBytes);
}

void testSlowShortReadsMakeProgressWithoutExpandingBudget() {
  LinkManifestStepper stepper;
  assert(stepper.begin(LinkManifestMode::recursiveRead));
  assert(stepper.addPath("Inbox/a/audio.wav"));
  assert(stepper.finishScan());
  assert(stepper.beginCurrentFile(4097));
  std::array<uint8_t, 257> bytes{};
  size_t polls = 0;
  while (stepper.phase() == LinkManifestPhase::hashing) {
    const size_t remaining = 4097 - stepper.currentHashedBytes();
    const size_t count = std::min(remaining, bytes.size());
    assert(stepper.acceptHashBytes(bytes.data(), count));
    ++polls;
    if (stepper.currentHashedBytes() == 4097) {
      assert(stepper.finishCurrentFile(
          "0000000000000000000000000000000000000000000000000000000000000000"));
    }
  }
  assert(polls == 16);
  assert(stepper.complete());
}

void testDisconnectCancelsAndSecondSessionStartsClean() {
  LinkManifestStepper stepper;
  assert(stepper.begin(LinkManifestMode::recursiveRead));
  assert(stepper.addPath("Inbox/a/audio.wav"));
  stepper.cancel();
  assert(stepper.cancelled());
  stepper.reset();
  assert(stepper.begin(LinkManifestMode::fingerprint));
  assert(stepper.addPath("Inbox/b/raw.txt"));
  assert(stepper.finishScan());
  assert(stepper.phase() == LinkManifestPhase::processing);
}

void testUsbHasNoWirelessDeadline() {
  LinkManifestStepper stepper;
  assert(stepper.begin(LinkManifestMode::recursiveRead));
  assert(stepper.addPath("Inbox/a/audio.wav"));
  assert(stepper.finishScan());
  String path;
  assert(stepper.currentPath(path));
  assert(stepper.beginCurrentFile(kSlowAudioBytes));
  std::array<uint8_t, kLinkManifestMaximumReadBytes> bytes{};
  uint32_t simulatedNowMs = 299000;
  while (stepper.phase() == LinkManifestPhase::hashing) {
    const size_t remaining = kSlowAudioBytes - stepper.currentHashedBytes();
    const size_t count =
        std::min(remaining, kLinkManifestMaximumReadBytes);
    assert(stepper.acceptHashBytes(bytes.data(), count));
    simulatedNowMs += 1000;
    if (stepper.currentHashedBytes() == kSlowAudioBytes) {
      assert(stepper.finishCurrentFile(
          "0000000000000000000000000000000000000000000000000000000000000000"));
    }
  }
  assert(simulatedNowMs > 300000);
  assert(stepper.complete());
}

}  // namespace

int main() {
  testLargeSlowDirectoryIsAlwaysBounded();
  testFiveHundredTwelvePathIndexSortsIncrementally();
  testCapacityOverflowFailsWithoutPartialManifest();
  testFingerprintOnlyHashesMetadata();
  testWiFiDeadlineCancelsAtExactlyFiveMinutes();
  testSlowShortReadsMakeProgressWithoutExpandingBudget();
  testDisconnectCancelsAndSecondSessionStartsClean();
  testUsbHasNoWirelessDeadline();
  return 0;
}
