#pragma once

#include <cstdarg>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <string>
#include <type_traits>

class String {
 public:
  String() = default;
  String(const char *value) : value_(value == nullptr ? "" : value) {}
  String(const std::string &value) : value_(value) {}
  String(char value) : value_(1, value) {}

  template <typename Integer,
            typename = std::enable_if_t<std::is_integral<Integer>::value>>
  String(Integer value) : value_(std::to_string(value)) {}

  const char *c_str() const { return value_.c_str(); }
  size_t length() const { return value_.size(); }
  bool isEmpty() const { return value_.empty(); }
  bool reserve(size_t capacity) {
    value_.reserve(capacity);
    return value_.capacity() >= capacity;
  }
  bool concat(const char *value, size_t length) {
    if (value == nullptr && length != 0) return false;
    value_.append(value == nullptr ? "" : value, length);
    return true;
  }

  bool startsWith(const char *prefix) const {
    if (prefix == nullptr) return false;
    const size_t length = std::strlen(prefix);
    return value_.size() >= length && value_.compare(0, length, prefix) == 0;
  }

  bool endsWith(const char *suffix) const {
    if (suffix == nullptr) return false;
    const size_t length = std::strlen(suffix);
    return value_.size() >= length &&
        value_.compare(value_.size() - length, length, suffix) == 0;
  }

  String substring(size_t start) const {
    if (start >= value_.size()) return String();
    return String(value_.substr(start));
  }

  String substring(size_t start, size_t end) const {
    if (start >= value_.size() || end <= start) return String();
    if (end > value_.size()) end = value_.size();
    return String(value_.substr(start, end - start));
  }

  int lastIndexOf(char needle) const {
    const size_t index = value_.find_last_of(needle);
    return index == std::string::npos ? -1 : static_cast<int>(index);
  }

  int indexOf(char needle) const {
    const size_t index = value_.find(needle);
    return index == std::string::npos ? -1 : static_cast<int>(index);
  }

  int indexOf(const char *needle) const {
    if (needle == nullptr) return -1;
    const size_t index = value_.find(needle);
    return index == std::string::npos ? -1 : static_cast<int>(index);
  }

  void trim() {
    const auto whitespace = [](unsigned char value) {
      return std::isspace(value) != 0;
    };
    const auto first = std::find_if_not(value_.begin(), value_.end(), whitespace);
    const auto last = std::find_if_not(value_.rbegin(), value_.rend(), whitespace)
                          .base();
    value_ = first < last ? std::string(first, last) : std::string();
  }

  bool equalsIgnoreCase(const String &other) const {
    if (value_.size() != other.value_.size()) return false;
    for (size_t index = 0; index < value_.size(); ++index) {
      const unsigned char left = static_cast<unsigned char>(value_[index]);
      const unsigned char right =
          static_cast<unsigned char>(other.value_[index]);
      if (std::tolower(left) != std::tolower(right)) return false;
    }
    return true;
  }

  String &operator=(const char *value) {
    value_ = value == nullptr ? "" : value;
    return *this;
  }

  String &operator+=(const String &value) {
    value_ += value.value_;
    return *this;
  }

  String &operator+=(const char *value) {
    if (value != nullptr) value_ += value;
    return *this;
  }

  String &operator+=(char value) {
    value_ += value;
    return *this;
  }

  template <typename Integer,
            typename = std::enable_if_t<std::is_integral<Integer>::value>>
  String &operator+=(Integer value) {
    value_ += std::to_string(value);
    return *this;
  }

  friend String operator+(const String &left, const String &right) {
    return String(left.value_ + right.value_);
  }

  friend String operator+(const String &left, const char *right) {
    return String(left.value_ + (right == nullptr ? "" : right));
  }

  friend String operator+(const char *left, const String &right) {
    return String(std::string(left == nullptr ? "" : left) + right.value_);
  }

  friend bool operator==(const String &left, const String &right) {
    return left.value_ == right.value_;
  }

  friend bool operator!=(const String &left, const String &right) {
    return !(left == right);
  }

 private:
  std::string value_;
};

class Print {
 public:
  virtual ~Print() = default;

  size_t print(const char *value) {
    return value == nullptr ? 0 : std::strlen(value);
  }

  size_t println(const char *value = "") {
    return print(value) + 1;
  }

  int printf(const char *format, ...) {
    if (format == nullptr) return 0;
    va_list args;
    va_start(args, format);
    va_list copy;
    va_copy(copy, args);
    const int length = std::vsnprintf(nullptr, 0, format, copy);
    va_end(copy);
    va_end(args);
    return length < 0 ? 0 : length;
  }
};
