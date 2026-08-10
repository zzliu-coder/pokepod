#include <assert.h>

#include "../PokePodAmoled/CapsuleCompatibilityPolicy.h"

using namespace pokepod;

int main() {
  CapsuleWireMetadata v1;
  v1.capsuleSchemaVersion = 1;
  v1.processingSchemaVersion = 1;
  v1.processingRevision = 2;
  v1.durationMs = 8000;
  v1.status = "queued";
  v1.audioFile = "audio.m4a";
  assert(capsuleRecordWritable(v1));

  CapsuleWireMetadata v2 = v1;
  v2.processingSchemaVersion = 2;
  v2.status = "recorded";
  v2.audioFile = "audio.wav";
  v2.audioFormat = "wav-pcm-s16le";
  v2.sampleRateHz = 16000;
  v2.channels = 1;
  v2.bitsPerSample = 16;
  assert(capsuleRecordWritable(v2));

  CapsuleWireMetadata future = v2;
  future.processingSchemaVersion = 3;
  assert(!capsuleRecordWritable(future));

  CapsuleWireMetadata unsafe = v2;
  unsafe.audioFile = "../audio.wav";
  assert(!capsuleRecordWritable(unsafe));

  CapsuleWireMetadata malformed = v2;
  malformed.processingRevision = -1;
  assert(!capsuleRecordWritable(malformed));
  malformed = v2;
  malformed.status = "future_state";
  assert(!capsuleRecordWritable(malformed));
  malformed = v2;
  malformed.bitsPerSample = 24;
  assert(!capsuleRecordWritable(malformed));
  return 0;
}
