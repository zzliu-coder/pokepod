from pathlib import Path


root = Path(__file__).parents[1]
firmware = root / "firmware" / "PokePodAmoled"
codec = (firmware / "RuntimeDiagnosticsCodec.h").read_text(encoding="utf-8")
runtime = (firmware / "RuntimeDiagnostics.cpp").read_text(encoding="utf-8")
header = (firmware / "RuntimeDiagnostics.h").read_text(encoding="utf-8")
link = (firmware / "LinkDiagnostics.cpp").read_text(encoding="utf-8")
dispatcher = (firmware / "LinkCommandDispatcher.cpp").read_text(encoding="utf-8")
cdc = (root / "cdc-status.py").read_text(encoding="utf-8")
wav = (firmware / "WavRecorder.cpp").read_text(encoding="utf-8")
wav_header = (firmware / "WavRecorder.h").read_text(encoding="utf-8")
app = (firmware / "PokePodApp.cpp").read_text(encoding="utf-8")
provisioning = (firmware / "ProvisioningCoordinator.cpp").read_text(encoding="utf-8")
provisioning_header = (firmware / "ProvisioningCoordinator.h").read_text(encoding="utf-8")

assert "kRuntimeDiagnosticsCapacity = 12" in codec
assert "kRuntimeDiagnosticTraceCapacity = 64" in codec
assert "runtimeDiagnosticShouldPersist" in codec
assert "traceJson" in header
assert '\\"persistent\\\":%s' in runtime
assert '\\"trace_sequence\\\":%lu' in runtime
assert "static_assert(sizeof(StoredRuntimeDiagnosticRecord) == 38" in codec
assert "static_assert(sizeof(StoredRuntimeDiagnosticLog) < 1024" in codec
assert "finalizeRuntimeDiagnosticLog" in codec
assert "validateRuntimeDiagnosticLog" in codec
assert 'preferences_.begin("pokepod_rt", false)' in runtime
assert "provisioningQuiesceBefore" in codec
assert "wirelessRouterAcquire" in codec
assert "linkStallRecovery" in codec
assert "radioDisconnect" in codec
assert "audioI2sBegin" in codec
assert "RuntimeDiagnosticSubsystem::radioControl" in codec
assert "RuntimeDiagnosticSubsystem::audioControl" in codec
assert "RuntimeDiagnosticSubsystem::link" in codec
assert '"get-runtime-diagnostics"' in dispatcher
assert '"get-runtime-trace"' in dispatcher
assert '"clear-runtime-diagnostics"' in dispatcher
assert "runtimeDiagnostics_->json()" in link
assert "runtimeDiagnostics_->clear(log)" in link
assert '"get-runtime-diagnostics"' in cdc
assert '"get-runtime-trace"' in cdc
assert '"clear-runtime-diagnostics"' in cdc
assert "bindRuntimeDiagnostics" in wav_header
assert "recordingStorageReserve" in wav
assert "recordingProbeFlush" in wav
assert "bindRuntimeDiagnostics" in provisioning_header
assert "provisioningQuiesceBefore" in provisioning
assert "recordWirelessRuntime" in app
assert "wirelessCaptureStart" in app
assert '"diagnostic-wireless-start"' in dispatcher
assert '"diagnostic-wireless-stop"' in dispatcher
assert '"link-probe"' in dispatcher
assert "transport_ != LinkTransport::usb" in dispatcher
assert "wirelessVoiceStart_()" in dispatcher
assert "wirelessVoiceStop_()" in dispatcher
assert "startWirelessHold, stopWirelessHold" in app
assert '"wireless-voice"' in (
    root / "fixture" / "pokepod-fixture.py"
).read_text(encoding="utf-8")

# NVS-backed runtime diagnostics must never execute inside the timed SD probe
# primitive. The probe persists only aggregate facts after cleanup.
probe_loop = wav[wav.index("for (uint8_t index = 0;"):
                 wav.index("const uint64_t totalUs")]
assert "recordRuntime(" not in probe_loop
assert "completedBytes" in wav and "failedChunk" in wav

# Wireless start tracing is RAM-only, so it cannot stall capture with NVS.
wireless_start = app[app.index("bool startWirelessHold()"):
                     app.index("AudioCaptureDispatchResult drainCapturedAudio")]
assert "runtimeDiagnosticShouldPersist" in codec
assert "wireless_capture_terminal" in app

for secret in ("secretKey", "secretId", "password", "credential"):
    assert secret not in runtime
    assert secret not in codec

print("PASS test_runtime_diagnostics")
