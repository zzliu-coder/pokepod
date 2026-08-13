#!/usr/bin/env python3

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware" / "PokePodAmoled"


def source(name: str) -> str:
    return (FIRMWARE / name).read_text(encoding="utf-8")


def require(text: str, needle: str, label: str) -> None:
    if needle not in text:
        raise SystemExit(f"FAIL tencent_network_contract: {label}")


tls = source("TlsExternalMemory.cpp")
worker = source("TencentWorker.cpp")
worker_header = source("TencentWorker.h")
job_runtime = source("TencentJobRuntime.h")
asr = source("TencentAsr.cpp")
link = source("PokePodLinkService.cpp")
link_transport = source("LinkTransportSession.cpp")
link_diagnostics = source("LinkDiagnostics.cpp")
dashboard = source("Dashboard.cpp")
main = source("PokePodApp.cpp")

require(tls, "mbedtls_platform_set_calloc_free(",
        "mbedTLS allocator is not redirected")
require(tls, "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT",
        "TLS allocator is not PSRAM-only")
require(main, "beginTlsExternalMemory(Serial);",
        "TLS allocator is not installed during startup")
require(worker, "xTaskCreatePinnedToCoreWithCaps(",
        "ASR task stack still consumes internal RAM")
require(worker, "ulTaskNotifyTake(pdTRUE, portMAX_DELAY);",
        "ASR worker is not a persistent notified task")
if "vTaskDeleteWithCaps" in worker or "vTaskDelete(" in worker:
    raise SystemExit("FAIL tencent_network_contract: persistent ASR worker is deleted")
require(worker, "TencentCancelReason::watchdog",
        "ASR worker watchdog cancellation is missing")
require(worker, "TencentJobState::committing",
        "ASR success is not separated from atomic commit")
require(worker, "clearTaskSecrets();",
        "ASR task-local credentials are not erased on terminal paths")
require(worker, "secureWipeSecrets(taskSettings_);",
        "ASR task-local settings do not use the shared wipe helper")
require(asr, "SecureWipeGuard secretDateWipe",
        "TC3 date key is not erased on every authorization exit")
require(asr, "SecureStringWipeGuard initialKeyWipe",
        "TC3 initial key is not erased on every authorization exit")
require(worker_header, "TencentJobRuntime runtime_;",
        "production worker does not own the host-tested lifecycle runtime")
for lifecycle_call in (
    "runtime_.workerStartResult(false)",
    "runtime_.request(",
    "runtime_.start(",
    "runtime_.checkWatchdog(",
    "runtime_.networkFinished(",
    "runtime_.commitFinished(",
    "runtime_.beginQuiesce(",
    "runtime_.pollQuiesce(",
):
    require(worker, lifecycle_call,
            f"production worker bypasses shared runtime: {lifecycle_call}")
for duplicated_state in (
    "std::atomic<uint8_t> state_",
    "std::atomic<uint32_t> activeGeneration_",
    "std::atomic<uint8_t> cancelReason_",
):
    if duplicated_state in worker_header:
        raise SystemExit(
            "FAIL tencent_network_contract: production worker duplicates "
            f"runtime state: {duplicated_state}")
require(job_runtime, "class TencentJobRuntime",
        "Arduino-free production lifecycle runtime is missing")
require(asr, "control->cancelled()",
        "ASR network and file path has no generation cancel token")
require(asr, "Network.hostByName(kHost, resolvedAddress)",
        "DNS failures are not separated from TLS failures")
require(asr, "client.lastError(networkError, sizeof(networkError))",
        "TLS errors are not captured")
require(asr, "internalHeapLargestBeforeTls",
        "TLS heap diagnostics are missing")
for field in (
    "asr_last_code",
    "asr_tls_error",
    "asr_tls_detail",
    "asr_heap_free_before_tls",
    "asr_heap_largest_before_tls",
    "asr_psram_free_before_tls",
):
    require(link_diagnostics, field, f"Link status is missing {field}")
wifi_ui = source("WifiUiPolicy.h")
require(wifi_ui, 'return automaticEnabled ? "省电休眠" : "已关闭";',
        "Wi-Fi sleep and manual-off labels are not distinct")
require(wifi_ui, "wifiUiSwitchOn(WifiPhase phase)",
        "Wi-Fi toggle is not driven by the live radio phase")
require(dashboard, "等待网络重试",
        "retryable queued capsules have no visible status")
require(main, "selected->status == CapsuleStatus::queued",
        "queued transient failures cannot be manually retried")
require(main, "tencentWorker.quiesce(",
        "deep sleep and shutdown do not quiesce ASR storage/network work")
require(main, "safe_shutdown_deferred",
        "safe shutdown does not report deferred ASR quiescence")
shutdown = main[main.index("bool advanceSafeShutdown("):
                main.index("String recordingId()")]
require(shutdown, "if (progress != SafeShutdownProgress::ready)",
        "safe shutdown does not gate teardown on actual ASR quiescence")
if shutdown.index("board.endSdMount()") < shutdown.index(
        "if (progress != SafeShutdownProgress::ready)"):
    raise SystemExit(
        "FAIL tencent_network_contract: SD is unmounted before ASR quiescence")
require(main, "if (!safeShutdownQuiesce.pending() &&\n"
              "      currentPowerDecision.requestDeepSleep",
        "a deferred safe shutdown can fall through into deep sleep")
require(link_transport, "tencent_->pollQuiesce(",
        "Link reboot must retain ASR ownership until quiesced")
require(link_transport, "tencent_->beginQuiesce(",
        "Link reboot must begin ASR quiescence without blocking")
require(link_transport, "tencent_->pollQuiesce(",
        "Link reboot must poll ASR quiescence cooperatively")
require(link_transport, "RebootQuiescePhase::waiting",
        "Link reboot lacks a persistent nonblocking ASR phase")
if "delay(5)" in link_transport:
    raise SystemExit("FAIL tencent_network_contract: Link transport still sleeps in poll")

print("PASS tencent_network_contract")
