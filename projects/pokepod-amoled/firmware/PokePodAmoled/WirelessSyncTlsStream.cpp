#include "WirelessSyncTlsStream.h"

#include <esp_tls_errors.h>
#include <fcntl.h>

#include "WirelessSyncIdentity.h"

namespace pokepod {
namespace {
constexpr uint32_t kHandshakeTimeoutMs = 6000;
constexpr uint32_t kWriteTimeoutMs = 5000;
}

bool WirelessSyncTlsStream::begin(NetworkClient client,
                                  const WirelessSyncIdentity &identity,
                                  LinkTransferGate &transferGate,
                                  uint32_t nowMs, Print &log) {
  close();
  log_ = &log;
  transferGate_ = &transferGate;
  transferGate_->attachCancellationSink(this);
  if (!ensureTransferPermitted()) return false;
  if (!client || client.fd() < 0 || !identity.ready()) {
    close();
    return false;
  }
  client_ = client;
  const int flags = fcntl(client_.fd(), F_GETFL, 0);
  if (flags < 0 || fcntl(client_.fd(), F_SETFL, flags | O_NONBLOCK) < 0) {
    markFailed(-1);
    return false;
  }
  tls_ = esp_tls_init();
  if (tls_ == nullptr) {
    markFailed(-2);
    return false;
  }
  config_ = esp_tls_cfg_server_t();
  config_.servercert_buf = identity.certificate();
  config_.servercert_bytes = identity.certificateBytes();
  config_.serverkey_buf = identity.privateKey();
  config_.serverkey_bytes = identity.privateKeyBytes();
  config_.tls_handshake_timeout_ms = kHandshakeTimeoutMs;
  if (esp_tls_server_session_init(&config_, client_.fd(), tls_) != ESP_OK) {
    markFailed(-3);
    return false;
  }
  startedAtMs_ = nowMs == 0 ? 1 : nowMs;
  phase_ = WirelessTlsPhase::handshaking;
  return true;
}

bool WirelessSyncTlsStream::pollHandshake(uint32_t nowMs) {
  if (!ensureTransferPermitted()) return false;
  if (phase_ == WirelessTlsPhase::ready) return true;
  if (phase_ != WirelessTlsPhase::handshaking || tls_ == nullptr) return false;
  const int result = esp_tls_server_session_continue_async(tls_);
  if (!ensureTransferPermitted()) return false;
  if (result == 0) {
    phase_ = WirelessTlsPhase::ready;
    if (log_ != nullptr) {
      log_->println("{\"event\":\"wifi_sync_tls\",\"ok\":true}");
    }
    return true;
  }
  if (result != ESP_TLS_ERR_SSL_WANT_READ &&
      result != ESP_TLS_ERR_SSL_WANT_WRITE) {
    markFailed(result);
    return false;
  }
  if (static_cast<uint32_t>(nowMs - startedAtMs_) >= kHandshakeTimeoutMs) {
    markFailed(ESP_ERR_ESP_TLS_SERVER_HANDSHAKE_TIMEOUT);
  }
  return false;
}

void WirelessSyncTlsStream::close() {
  if (transferGate_ != nullptr) {
    transferGate_->detachCancellationSink(this);
    transferGate_ = nullptr;
  }
  if (tls_ != nullptr) {
    esp_tls_server_session_delete(tls_);
    tls_ = nullptr;
  }
  if (client_) client_.stop();
  client_ = NetworkClient();
  phase_ = WirelessTlsPhase::closed;
  startedAtMs_ = 0;
  receiveOffset_ = receiveUsed_ = 0;
  log_ = nullptr;
}

void WirelessSyncTlsStream::cancelForTransferDeadline() {
  if (log_ != nullptr) {
    log_->println(
        "{\"event\":\"wifi_sync_tls\",\"ok\":false,"
        "\"error\":\"window-deadline\"}");
  }
  close();
}

bool WirelessSyncTlsStream::pump() {
  if (!ensureTransferPermitted()) return false;
  if (!ready() || tls_ == nullptr) return false;
  if (receiveOffset_ < receiveUsed_) return true;
  receiveOffset_ = receiveUsed_ = 0;
  const ssize_t count = esp_tls_conn_read(tls_, receive_, sizeof(receive_));
  if (!ensureTransferPermitted()) return false;
  if (count > 0) {
    receiveUsed_ = static_cast<size_t>(count);
    return true;
  }
  if (count == 0) {
    phase_ = WirelessTlsPhase::closed;
  } else if (count != ESP_TLS_ERR_SSL_WANT_READ &&
             count != ESP_TLS_ERR_SSL_WANT_WRITE) {
    markFailed(static_cast<int>(count));
  }
  return false;
}

int WirelessSyncTlsStream::available() {
  pump();
  return static_cast<int>(receiveUsed_ - receiveOffset_);
}

int WirelessSyncTlsStream::read() {
  if (available() <= 0) return -1;
  return receive_[receiveOffset_++];
}

int WirelessSyncTlsStream::peek() {
  if (available() <= 0) return -1;
  return receive_[receiveOffset_];
}

void WirelessSyncTlsStream::flush() {}

size_t WirelessSyncTlsStream::write(uint8_t value) {
  return write(&value, 1);
}

size_t WirelessSyncTlsStream::write(const uint8_t *buffer, size_t size) {
  if (!ensureTransferPermitted() || !ready() || tls_ == nullptr ||
      buffer == nullptr) {
    return 0;
  }
  const uint32_t started = millis();
  size_t offset = 0;
  while (offset < size) {
    if (!ensureTransferPermitted()) return offset;
    const ssize_t written = esp_tls_conn_write(tls_, buffer + offset,
                                               size - offset);
    if (!ensureTransferPermitted()) return offset;
    if (written > 0) {
      offset += static_cast<size_t>(written);
      continue;
    }
    if (written != ESP_TLS_ERR_SSL_WANT_READ &&
        written != ESP_TLS_ERR_SSL_WANT_WRITE) {
      markFailed(static_cast<int>(written));
      break;
    }
    if (static_cast<uint32_t>(millis() - started) >= kWriteTimeoutMs) {
      markFailed(-4);
      break;
    }
    delay(1);
  }
  return offset;
}

bool WirelessSyncTlsStream::ensureTransferPermitted() {
  if (linkTransferPermitted(transferGate_, millis())) return true;
  return false;
}

void WirelessSyncTlsStream::markFailed(int error) {
  lastError_ = error;
  phase_ = WirelessTlsPhase::failed;
  if (log_ != nullptr) {
    log_->printf("{\"event\":\"wifi_sync_tls\",\"ok\":false,\"error\":%d}\n",
                 error);
  }
}

}  // namespace pokepod
