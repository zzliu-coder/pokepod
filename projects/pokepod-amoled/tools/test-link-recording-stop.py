#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware/PokePodAmoled"
SERVICE = (FIRMWARE / "PokePodLinkService.cpp").read_text()
HEADER = (FIRMWARE / "PokePodLinkService.h").read_text()
LINK_DISPATCHER = (FIRMWARE / "LinkCommandDispatcher.cpp").read_text()
SESSION_H = (FIRMWARE / "LinkRecordingSession.h").read_text()
SESSION = (FIRMWARE / "LinkRecordingSession.cpp").read_text()
STATE = (FIRMWARE / "LinkRecordingStop.h").read_text()
CAPTURE_DISPATCHER = (FIRMWARE / "AudioCaptureDispatcher.h").read_text()

# The service owns exactly one extracted session; the old parallel state and
# old service-local implementations must not survive the extraction.
assert '#include "LinkRecordingSession.h"' in HEADER
assert "LinkRecordingSession recordingSession_" in HEADER
for legacy in (
    "requestLinkRecordingStop", "advanceLinkRecordingStart",
    "advanceLinkRecordingStop", "LinkRecordingStop linkRecordingStop_",
    "LinkRecordingStart linkRecordingStart_", "linkOwnedRecording_",
):
    assert legacy not in HEADER
    assert legacy not in SERVICE

begin = SERVICE[SERVICE.index("bool PokePodLinkService::begin("):
                SERVICE.index("uint32_t PokePodLinkService::activateConnectionGeneration")]
assert "recordingSession_.begin(" in begin

disconnect = SERVICE[SERVICE.index("void PokePodLinkService::disconnect()"):
                     SERVICE.index("void PokePodLinkService::requestQuiesce()")]
assert "recordingSession_.disconnect(operation_, transactionGate_)" in disconnect
assert "captureRouter_->release" not in disconnect
session_disconnect = SESSION[SESSION.index("void LinkRecordingSession::disconnect("):
                             SESSION.index("bool LinkRecordingSession::recordingActive() const")]
assert "suppressResponseAndAbort" in session_disconnect
assert "requestStop(0, false, false" in session_disconnect
assert "transactionGate.cancel()" in session_disconnect

advance = SESSION[SESSION.index("LinkRecordingEvent LinkRecordingSession::advanceStop("):
                  SESSION.index("LinkRecordingEvent LinkRecordingSession::poll(")]
assert "captureRuntime_->pollFinalize" in advance
assert "captureRuntime_->running()" in advance
assert advance.index("captureRuntime_->running()") < advance.index(
    "captureRouter_->release")
assert "recorder_->pollFinalize(*log_, millis(), recordingGate)" in advance
assert "recorder_->operationActive()" in advance
assert "recorder_->takeTerminalResult(outcome)" in advance
assert "outcome.success()" in advance
assert "transport == LinkTransport::wifi || !sessionActive || quiesceRequested" in advance
assert advance.index("recorder_->takeTerminalResult(outcome)") < advance.index(
    "captureRouter_->release")
assert "recorder_->pollCleanup" not in advance
assert "operation.releaseResource(LinkOperationResource::router)" in advance
assert "operation.releaseResource(LinkOperationResource::transaction)" in advance
assert "rememberCompleted" not in advance
assert "releaseRequestLease" not in advance
final_drain = advance.index("captureDispatcher_->drain(")
final_snapshot = advance.index("captureRuntime_->frontEndSnapshot()", final_drain)
final_observe = advance.index("recorder_->observeAudioMetrics(", final_snapshot)
recorder_stop = advance.index("recorder_->stop(*log_", final_observe)
recorder_abort = advance.index("recorder_->abortCapture(*log_)", final_observe)
assert final_drain < final_snapshot < final_observe < recorder_stop
assert final_observe < recorder_abort
assert "!recorder_->captureFailureLatched()" in advance
assert "drainLinkCapture" not in SERVICE + SESSION
assert "while (source.pop(frame))" in CAPTURE_DISPATCHER

assert "awaitCaptureFinalize" in STATE
assert "awaitRecorderTerminal" in STATE
assert "suppressResponseAndAbort" in STATE

start_dispatch = LINK_DISPATCHER[LINK_DISPATCHER.index('} else if (strcmp(operation, "record") == 0)'):
                                    LINK_DISPATCHER.index('} else if (strcmp(operation, "stop") == 0)')]
assert "RecorderOperationOwner::linkWifi" in start_dispatch
assert "RecorderOperationOwner::linkUsb" in start_dispatch
assert "recordingSession_.requestStart(" in start_dispatch
assert "recorder_->requestStart" not in start_dispatch

request_start = SESSION[SESSION.index("LinkRecordingRequestResult LinkRecordingSession::requestStart("):
                        SESSION.index("bool LinkRecordingSession::requestStop(")]
assert "transactionGate.beginOperation(transferGate)" in request_start
assert "recorder_->requestStart" in request_start
assert "captureRouter_->available()" in request_start
assert "captureRouter_->acquire(AudioCaptureOwner::localCapsule)" in request_start

start_advance = SESSION[SESSION.index("LinkRecordingEvent LinkRecordingSession::advanceStart("):
                        SESSION.index("LinkRecordingEvent LinkRecordingSession::advanceStop(")]
assert "recorder_->pollStart" in start_advance
assert "RecorderStartPollResult::pending" in start_advance
assert "RecorderStartPollResult::started" in start_advance
assert "linkTransferPermitted" in start_advance
assert start_advance.index("RecorderStartPollResult::started") < start_advance.index(
    "captureRuntime_->start")
assert "LinkRecordingEventKind::startReady" in start_advance
assert "operation.releaseResource(LinkOperationResource::router)" in start_advance
assert "operation.releaseResource(LinkOperationResource::transaction)" in start_advance

process = LINK_DISPATCHER[LINK_DISPATCHER.index("void PokePodLinkService::processRequest("): ]
assert "recordingSession_.ownsRequest(requestId)" in process
assert "admitLinkOperation(requestId)" in process
assert "sendBusy(requestId);" in process
assert process.index("admitLinkOperation(requestId)") < process.index("cJSON_ParseWithLength")
assert process.index("handleImmediate(requestId, root)") < process.rindex(
    "recordingSession_.ownsRequest(requestId)")

poll = SERVICE[SERVICE.index("void PokePodLinkService::pollDeferredCleanup()"):
               SERVICE.index("void PokePodLinkService::poll(uint32_t nowMs)")]
assert "recordingSession_.poll(" in poll
assert "handleLinkRecordingEvent" in poll

print("PASS link_recording_stop_contract")
