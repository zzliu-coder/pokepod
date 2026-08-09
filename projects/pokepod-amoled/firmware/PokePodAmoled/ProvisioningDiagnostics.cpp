#include "ProvisioningDiagnostics.h"

#include <algorithm>
#include <cstring>
#include <time.h>

#include "WifiFailurePolicy.h"

namespace pokepod {
namespace {

constexpr char kProvisioningLogKey[] = "wifi_log_v1";

void copySsid(char *destination, size_t capacity, const String &ssid) {
  const size_t length = std::min(ssid.length(), capacity - 1);
  if (length > 0) std::memcpy(destination, ssid.c_str(), length);
  destination[length] = '\0';
}

}  // namespace

bool ProvisioningDiagnostics::begin(Print &log) {
  open_ = preferences_.begin("pokepod_diag", false);
  initializeProvisioningLog(stored_);
  if (!open_) {
    log.println("{\"event\":\"provisioning_log\",\"ok\":false,\"stage\":\"nvs_open\"}");
    return false;
  }
  StoredProvisioningLog loaded{};
  const bool loadedOk =
      preferences_.getBytesLength(kProvisioningLogKey) == sizeof(loaded) &&
      preferences_.getBytes(kProvisioningLogKey, &loaded, sizeof(loaded)) ==
          sizeof(loaded) && validateProvisioningLog(loaded);
  if (loadedOk) stored_ = loaded;
  ++revision_;
  log.printf("{\"event\":\"provisioning_log\",\"ok\":true,\"count\":%u,\"recovered\":%s}\n",
             static_cast<unsigned>(stored_.count), loadedOk ? "true" : "false");
  return true;
}

bool ProvisioningDiagnostics::persist(const StoredProvisioningLog &proposed) {
  return open_ && preferences_.putBytes(
      kProvisioningLogKey, &proposed, sizeof(proposed)) == sizeof(proposed);
}

bool ProvisioningDiagnostics::record(
    ProvisioningLogStage stage, ProvisioningLogOutcome outcome,
    const String &ssid, int16_t rssi, uint16_t reason,
    uint32_t elapsedMs, uint8_t attempt, Print &log) {
  if (!open_ || ssid.length() > 32) return false;
  StoredProvisioningLog proposed = stored_;
  StoredProvisioningLogRecord record{};
  const time_t now = time(nullptr);
  record.epoch = now >= 1704067200 ? static_cast<uint32_t>(now) : 0;
  record.elapsedMs = elapsedMs;
  record.reason = reason;
  record.rssi = rssi;
  record.stage = static_cast<uint8_t>(stage);
  record.outcome = static_cast<uint8_t>(outcome);
  record.attempt = attempt;
  copySsid(record.ssid, sizeof(record.ssid), ssid);
  appendProvisioningLog(proposed, record);
  finalizeProvisioningLog(proposed);
  const bool ok = persist(proposed);
  if (ok) {
    stored_ = proposed;
    ++revision_;
  }
  log.printf("{\"event\":\"provisioning_diagnostic\",\"ok\":%s,\"stage\":\"%s\",\"outcome\":%u,\"reason\":%u,\"rssi\":%d,\"elapsed_ms\":%lu}\n",
             ok ? "true" : "false", provisioningLogStageKey(stage),
             static_cast<unsigned>(outcome), static_cast<unsigned>(reason),
             static_cast<int>(rssi), static_cast<unsigned long>(elapsedMs));
  return ok;
}

bool ProvisioningDiagnostics::clear(Print &log) {
  StoredProvisioningLog proposed{};
  initializeProvisioningLog(proposed);
  finalizeProvisioningLog(proposed);
  const bool ok = persist(proposed);
  if (ok) {
    stored_ = proposed;
    ++revision_;
  }
  log.printf("{\"event\":\"provisioning_log_cleared\",\"ok\":%s}\n",
             ok ? "true" : "false");
  return ok;
}

const char *provisioningLogStageKey(ProvisioningLogStage stage) {
  switch (stage) {
    case ProvisioningLogStage::portalStarted: return "portal_started";
    case ProvisioningLogStage::scanStarted: return "scan_started";
    case ProvisioningLogStage::scanFinished: return "scan_finished";
    case ProvisioningLogStage::connectStarted: return "connect_started";
    case ProvisioningLogStage::connected: return "connected";
    case ProvisioningLogStage::configSaved: return "config_saved";
    case ProvisioningLogStage::failed: return "failed";
    case ProvisioningLogStage::portalStopped: return "portal_stopped";
  }
  return "unknown";
}

String provisioningLogStageLabel(ProvisioningLogStage stage) {
  switch (stage) {
    case ProvisioningLogStage::portalStarted: return "配网热点已启动";
    case ProvisioningLogStage::scanStarted: return "正在扫描网络";
    case ProvisioningLogStage::scanFinished: return "网络扫描完成";
    case ProvisioningLogStage::connectStarted: return "开始连接热点";
    case ProvisioningLogStage::connected: return "热点连接成功";
    case ProvisioningLogStage::configSaved: return "配置保存成功";
    case ProvisioningLogStage::failed: return "连接失败";
    case ProvisioningLogStage::portalStopped: return "配网热点已关闭";
  }
  return "未知阶段";
}

String provisioningLogReasonLabel(const StoredProvisioningLogRecord &record) {
  if (record.outcome != static_cast<uint8_t>(ProvisioningLogOutcome::failure)) {
    if (record.rssi > -127 && record.rssi != 0) {
      return String(record.rssi) + " dBm · " + String(record.elapsedMs) + " ms";
    }
    return String(record.elapsedMs) + " ms";
  }
  switch (record.reason) {
    case kProvisioningReasonStorage: return "配置写入失败";
    case kProvisioningReasonScanStart: return "扫描启动失败";
    case kProvisioningReasonScanTimeout: return "扫描超时";
    case kProvisioningReasonScanFailed: return "扫描失败";
    case kProvisioningReasonInvalidInput: return "输入内容不完整";
    case kProvisioningReasonPortalFailed: return "配网热点启动失败";
  }
  switch (wifiFailureKind(record.reason)) {
    case WifiFailureKind::noAccessPoint: return "没有收到热点响应";
    case WifiFailureKind::authentication: return "认证失败";
    case WifiFailureKind::capacity: return "热点容量已满";
    case WifiFailureKind::timeout: return "连接超时";
    case WifiFailureKind::association: return "热点拒绝连接";
    case WifiFailureKind::other:
      return String("原因代码 ") + record.reason;
    case WifiFailureKind::none: return "没有明确断开原因";
  }
  return "未知原因";
}

}  // namespace pokepod
