#!/usr/bin/env python3

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CPP = (ROOT / "firmware/PokePodAmoled/PokePodLinkService.cpp").read_text()
HEADER = (ROOT / "firmware/PokePodAmoled/PokePodLinkService.h").read_text()
STATE = (ROOT / "firmware/PokePodAmoled/LinkRecordingStop.h").read_text()

assert "requestLinkRecordingStop" in HEADER
assert "advanceLinkRecordingStop" in HEADER
assert "LinkRecordingStop linkRecordingStop_" in HEADER
assert "stopLinkRecording" not in HEADER

disconnect = CPP[CPP.index("void PokePodLinkService::disconnect()"):
                 CPP.index("void PokePodLinkService::pollDeferredCleanup()")]
assert "suppressResponseAndAbort" in disconnect
assert "requestLinkRecordingStop(0, false, false)" in disconnect
assert "captureRouter_->release" not in disconnect

advance = CPP[CPP.index("void PokePodLinkService::advanceLinkRecordingStop()"):
              CPP.index("bool PokePodLinkService::transferPermitted()")]
assert "captureRuntime_->pollFinalize" in advance
assert "captureRuntime_->running()" in advance
assert advance.index("captureRuntime_->running()") < advance.index(
    "captureRouter_->release")
assert "recorder_->pollFinalize(*log_, millis(), recordingGate)" in advance
assert "recorder_->operationActive()" in advance
assert "recorder_->takeTerminalResult(outcome)" in advance
assert "outcome.success()" in advance
assert "transport_ == LinkTransport::wifi || !sessionActive_ || quiesceRequested_" in advance
assert advance.index("recorder_->takeTerminalResult(outcome)") < advance.index(
    "captureRouter_->release")
assert "recorder_->pollCleanup" not in advance
assert "operation_.releaseResource(LinkOperationResource::router)" in advance
assert "operation_.releaseResource(LinkOperationResource::transaction)" in advance
assert "rememberCompleted(requestId)" not in advance
assert "releaseRequestLease()" not in advance
final_drain = advance.index("const bool drained = drainLinkCapture()")
final_snapshot = advance.index("captureRuntime_->frontEndSnapshot()", final_drain)
final_observe = advance.index("recorder_->observeAudioMetrics(", final_snapshot)
recorder_stop = advance.index("recorder_->stop(*log_", final_observe)
recorder_abort = advance.index("recorder_->abortCapture(*log_)", final_observe)
assert final_drain < final_snapshot < final_observe < recorder_stop
assert final_observe < recorder_abort

assert "awaitCaptureFinalize" in STATE
assert "awaitRecorderTerminal" in STATE
assert "suppressResponseAndAbort" in STATE

start = CPP[CPP.index('} else if (strcmp(operation, "record") == 0)'):
            CPP.index('} else if (strcmp(operation, "stop") == 0)')]
assert "RecorderOperationOwner::linkWifi" in start
assert "RecorderOperationOwner::linkUsb" in start
assert "transactionGate_.beginOperation(transferGate_)" in start
assert "recorder_->requestStart" in start
assert "recorder_->start(" not in start

start_advance = CPP[CPP.index("void PokePodLinkService::advanceLinkRecordingStart()"):
                    CPP.index("void PokePodLinkService::advanceLinkRecordingStop()")]
assert "recorder_->pollStart" in start_advance
assert "RecorderStartPollResult::pending" in start_advance
assert "RecorderStartPollResult::started" in start_advance
assert "transferPermitted()" in start_advance
assert start_advance.index("RecorderStartPollResult::started") < start_advance.index(
    "captureRuntime_->start")
assert start_advance.index("captureRuntime_->start") < start_advance.index(
    "sendOk(requestId")
assert "operation_.releaseResource(LinkOperationResource::router)" in start_advance
assert "operation_.releaseResource(LinkOperationResource::transaction)" in start_advance
assert "rememberCompleted(requestId)" not in start_advance
assert "releaseRequestLease()" not in start_advance

process = CPP[CPP.index("void PokePodLinkService::processRequest("):
              CPP.index("bool PokePodLinkService::beginIncoming(")]
assert "linkRecordingStart_.ownsRequest(requestId)" in process
assert "admitLinkOperation(requestId)" in process
assert "sendBusy(requestId);" in process
async_guard = process.index("admitLinkOperation(requestId)")
assert async_guard < process.index("cJSON_ParseWithLength")
assert process.index("handleImmediate(requestId, root)") < process.rindex(
    "linkRecordingStart_.ownsRequest(requestId)")

queue = CPP[CPP.index("bool PokePodLinkService::queueFrame("):
            CPP.index("void PokePodLinkService::advanceTransmit(")]
assert "operation_.queueFrame(" in queue
assert "LinkOperationFrameRole role" in queue
assert "requestLeaseOwnerRequestId_" not in queue
assert "releaseRequestLeaseWhenTxDrained_" not in queue

poll = CPP[CPP.index("void PokePodLinkService::pollDeferredCleanup()"):
           CPP.index("void PokePodLinkService::poll(uint32_t nowMs)")]
assert "advanceLinkRecordingStop()" in poll
assert "advanceLinkRecordingStart()" in poll
assert poll.index("advanceLinkRecordingStart()") < poll.index(
    "advanceLinkRecordingStop()")

disconnect = CPP[CPP.index("void PokePodLinkService::disconnect()"):
                 CPP.index("void PokePodLinkService::requestQuiesce()")]
assert "transactionGate_.cancel()" in disconnect
assert disconnect.index("transactionGate_.cancel()") < CPP.index(
    "recorder_->takeTerminalResult(outcome)")

print("PASS link_recording_stop_contract")
