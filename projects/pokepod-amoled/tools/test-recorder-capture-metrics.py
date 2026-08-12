#!/usr/bin/env python3
"""Production capture diagnostics must follow the realtime DSP session."""

from pathlib import Path


root = Path(__file__).parents[1]
firmware = root / "firmware" / "PokePodAmoled"
service = (firmware / "AudioCaptureService.h").read_text(encoding="utf-8")
runtime = (firmware / "AudioCaptureRuntime.h").read_text(encoding="utf-8")
wav_h = (firmware / "WavRecorder.h").read_text(encoding="utf-8")
wav = (firmware / "WavRecorder.cpp").read_text(encoding="utf-8")
app = (firmware / "PokePodApp.cpp").read_text(encoding="utf-8")
link = (firmware / "PokePodLinkService.cpp").read_text(encoding="utf-8")

assert "struct AudioCaptureFrontEndSnapshot" in service
assert "AudioCaptureFrontEndPublisher" in service
assert "sequence_.beginPublication(before + 1U)" in service
assert "sequence_.storeRelease(before + 2U)" in service
assert "frontEndPublisher_.publish(sessionId_, true, frontEnd_.metrics())" in service
assert "frontEndPublisher_.publish(sessionId_, false, frontEnd_.metrics())" in service
assert "AudioCaptureFrontEndSnapshot frontEndSnapshot() const" in runtime

assert "bool append(const uint8_t *data" not in wav_h
assert "bool WavRecorder::append(const uint8_t *data" not in wav
assert "AudioFrontEnd audioFrontEnd_" not in wav_h
assert "audioFrontEnd_.processStereo16" not in wav
assert "void observeAudioMetrics(uint32_t sessionId, uint32_t generation" in wav_h
assert "const AudioFrontEndMetrics &audio = audioMetrics_" in wav

assert "AudioCaptureFrontEndSnapshot lastLocalCaptureMetrics" in app
assert "captureRuntime.frontEndSnapshot()" in app
assert "recorder.observeAudioMetrics(snapshot.sessionId, snapshot.generation" in app
assert "observeCaptureMetrics();" in app
assert '\\\"capture_session_id\\\"' in app
assert '\\\"capture_metrics_generation\\\"' in app
assert "const AudioFrontEndMetrics frontEnd = captureSnapshot.asMetrics()" in app
assert '\\\"audio_capture_session_id\\\"' in app
assert '\\\"audio_capture_metrics_generation\\\"' in app
assert '\\\"audio_capture_active\\\"' in app
assert "const AudioFrontEndMetrics &frontEnd = recorder.audioMetrics()" not in app
overflow = app[app.index("if (!drainCapturedAudio(now))"):
               app.index("if (recorder.ownedBy(",
                         app.index("if (!drainCapturedAudio(now))"))]
assert "recorder.abortCapture" not in overflow
assert "requestCaptureStop(PendingCaptureStop::localCapsule" in overflow

local_finish = app[app.index("bool finishPendingCaptureStop() {"):
                   app.index("bool requestCaptureStop(",
                             app.index("bool finishPendingCaptureStop() {"))]
local_drain = local_finish.index("drainCapturedAudio(millis())")
local_snapshot = local_finish.index("observeCaptureMetrics()", local_drain)
local_stop = local_finish.index("recorder.stop(usb.log()", local_snapshot)
local_abort = local_finish.index("recorder.abortCapture(usb.log())",
                                 local_snapshot)
# Normal, queue-overflow and explicit abort all join this exact terminal path:
# stopped realtime task -> final drain -> final DSP snapshot -> stop/abort.
assert local_drain < local_snapshot < local_stop
assert local_snapshot < local_abort

advance = link[link.index("void PokePodLinkService::advanceLinkRecordingStop()"):
               link.index("bool PokePodLinkService::transferPermitted()")]
drain = advance.index("const bool drained = drainLinkCapture()")
snapshot = advance.index("captureRuntime_->frontEndSnapshot()", drain)
observe = advance.index("recorder_->observeAudioMetrics(", snapshot)
stop = advance.index("recorder_->stop(*log_", observe)
abort = advance.index("recorder_->abortCapture(*log_)", observe)
assert drain < snapshot < observe < stop
assert observe < abort
# Normal, disconnect and Link queue-overflow all use this single branch; the
# stop state chooses commit/abort only after the final session snapshot.
assert "suppressResponseAndAbort" in link

print("PASS test_recorder_capture_metrics")
