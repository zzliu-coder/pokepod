#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>

#include <Arduino.h>

namespace pokepod {

inline void secureWipeBytes(void *storage, size_t bytes) {
  volatile uint8_t *cursor = static_cast<volatile uint8_t *>(storage);
  while (cursor != nullptr && bytes-- > 0) *cursor++ = 0;
}

inline void secureWipe(String &secret) {
  // ESP32 Arduino keeps capacity private. Every live secret byte is within
  // length(), so overwrite that storage before changing the logical length.
#if defined(ARDUINO)
  secureWipeBytes(secret.begin(), secret.length());
#else
  secureWipeBytes(const_cast<char *>(secret.c_str()), secret.length());
#endif
  secret = "";
}

inline void secureWipe(std::string &secret) {
  // std::string exposes writable storage only through size(); capacity bytes
  // beyond end() are not portable to write.
  if (!secret.empty()) secureWipeBytes(&secret[0], secret.size());
  secret.clear();
}

class SecureWipeGuard {
 public:
  SecureWipeGuard(void *storage, size_t bytes)
      : storage_(storage), bytes_(bytes) {}
  ~SecureWipeGuard() { secureWipeBytes(storage_, bytes_); }
  SecureWipeGuard(const SecureWipeGuard &) = delete;
  SecureWipeGuard &operator=(const SecureWipeGuard &) = delete;

 private:
  void *storage_ = nullptr;
  size_t bytes_ = 0;
};

class SecureStringWipeGuard {
 public:
  explicit SecureStringWipeGuard(String &secret) : secret_(&secret) {}
  ~SecureStringWipeGuard() {
    if (secret_ != nullptr) secureWipe(*secret_);
  }
  SecureStringWipeGuard(const SecureStringWipeGuard &) = delete;
  SecureStringWipeGuard &operator=(const SecureStringWipeGuard &) = delete;

 private:
  String *secret_ = nullptr;
};

}  // namespace pokepod
