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
asr = source("TencentAsr.cpp")
link = source("PokePodLinkService.cpp")
dashboard = source("Dashboard.cpp")
main = source("PokePodAmoled.ino")

require(tls, "mbedtls_platform_set_calloc_free(",
        "mbedTLS allocator is not redirected")
require(tls, "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT",
        "TLS allocator is not PSRAM-only")
require(main, "beginTlsExternalMemory(Serial);",
        "TLS allocator is not installed during startup")
require(worker, "xTaskCreatePinnedToCoreWithCaps(",
        "ASR task stack still consumes internal RAM")
require(worker, "vTaskDeleteWithCaps(nullptr);",
        "PSRAM task stack is deleted with the wrong API")
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
    require(link, field, f"Link status is missing {field}")
wifi_ui = source("WifiUiPolicy.h")
require(wifi_ui, 'return automaticEnabled ? "省电休眠" : "已关闭";',
        "Wi-Fi sleep and manual-off labels are not distinct")
require(wifi_ui, "wifiUiSwitchOn(WifiPhase phase)",
        "Wi-Fi toggle is not driven by the live radio phase")
require(dashboard, "等待网络重试",
        "retryable queued capsules have no visible status")
require(main, "selected->status == CapsuleStatus::queued",
        "queued transient failures cannot be manually retried")

print("PASS tencent_network_contract")
