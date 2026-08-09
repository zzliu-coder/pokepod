#pragma once

#include <Arduino.h>
#include <NetworkClient.h>
#include <esp_tls.h>

namespace pokepod {

class WirelessSyncIdentity;

enum class WirelessTlsPhase : uint8_t {
  idle,
  handshaking,
  ready,
  failed,
  closed,
};

class WirelessSyncTlsStream : public Stream {
 public:
  bool begin(NetworkClient client, const WirelessSyncIdentity &identity,
             uint32_t nowMs, Print &log);
  bool pollHandshake(uint32_t nowMs);
  void close();

  int available() override;
  int read() override;
  int peek() override;
  void flush() override;
  size_t write(uint8_t value) override;
  size_t write(const uint8_t *buffer, size_t size) override;

  WirelessTlsPhase phase() const { return phase_; }
  bool ready() const { return phase_ == WirelessTlsPhase::ready; }
  bool failed() const { return phase_ == WirelessTlsPhase::failed; }
  bool closed() const { return phase_ == WirelessTlsPhase::closed; }

 private:
  bool pump();
  void markFailed(int error);

  NetworkClient client_;
  esp_tls_t *tls_ = nullptr;
  esp_tls_cfg_server_t config_ = {};
  WirelessTlsPhase phase_ = WirelessTlsPhase::idle;
  uint32_t startedAtMs_ = 0;
  int lastError_ = 0;
  Print *log_ = nullptr;
  uint8_t receive_[4096] = {};
  size_t receiveOffset_ = 0;
  size_t receiveUsed_ = 0;
};

}  // namespace pokepod
