#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

namespace pokepod {

constexpr int32_t kChinaStandardTimeOffsetMinutes = 8 * 60;

struct CivilDateTime {
  int32_t year = 1970;
  uint8_t month = 1;
  uint8_t day = 1;
  uint8_t hour = 0;
  uint8_t minute = 0;
  uint8_t second = 0;
};

inline bool isLeapYear(int32_t year) {
  return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

inline uint8_t daysInMonth(int32_t year, uint8_t month) {
  static constexpr uint8_t kDays[] = {
      31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  if (month == 2 && isLeapYear(year)) return 29;
  return kDays[month - 1];
}

inline bool validCivilDateTime(const CivilDateTime &value) {
  return value.year >= 1970 && value.year <= 2400 && value.month >= 1 &&
      value.month <= 12 && value.day >= 1 &&
      value.day <= daysInMonth(value.year, value.month) && value.hour < 24 &&
      value.minute < 60 && value.second < 60;
}

// Howard Hinnant's civil calendar mapping, adapted to a small header-only
// implementation. The result is whole days relative to 1970-01-01.
inline int64_t daysFromCivil(int32_t year, uint8_t month, uint8_t day) {
  year -= month <= 2;
  const int32_t era = (year >= 0 ? year : year - 399) / 400;
  const uint32_t yearOfEra = static_cast<uint32_t>(year - era * 400);
  const uint32_t shiftedMonth = month > 2 ? month - 3 : month + 9;
  const uint32_t dayOfYear =
      (153 * shiftedMonth + 2) / 5 + static_cast<uint32_t>(day) - 1;
  const uint32_t dayOfEra = yearOfEra * 365 + yearOfEra / 4 -
      yearOfEra / 100 + dayOfYear;
  return static_cast<int64_t>(era) * 146097 + dayOfEra - 719468;
}

inline CivilDateTime civilFromDays(int64_t days) {
  days += 719468;
  const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const uint32_t dayOfEra = static_cast<uint32_t>(days - era * 146097);
  const uint32_t yearOfEra =
      (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 -
       dayOfEra / 146096) / 365;
  int32_t year = static_cast<int32_t>(yearOfEra) +
      static_cast<int32_t>(era) * 400;
  const uint32_t dayOfYear = dayOfEra -
      (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
  const uint32_t monthPrime = (5 * dayOfYear + 2) / 153;
  const uint8_t day = static_cast<uint8_t>(
      dayOfYear - (153 * monthPrime + 2) / 5 + 1);
  const uint8_t month = static_cast<uint8_t>(
      monthPrime < 10 ? monthPrime + 3 : monthPrime - 9);
  year += month <= 2;
  CivilDateTime result;
  result.year = year;
  result.month = month;
  result.day = day;
  return result;
}

inline bool civilToUnixSeconds(const CivilDateTime &value, int64_t &seconds) {
  if (!validCivilDateTime(value)) return false;
  seconds = daysFromCivil(value.year, value.month, value.day) * 86400 +
      static_cast<int64_t>(value.hour) * 3600 +
      static_cast<int64_t>(value.minute) * 60 + value.second;
  return true;
}

inline CivilDateTime civilFromUnixSeconds(int64_t seconds) {
  int64_t days = seconds / 86400;
  int64_t secondsOfDay = seconds % 86400;
  if (secondsOfDay < 0) {
    secondsOfDay += 86400;
    --days;
  }
  CivilDateTime result = civilFromDays(days);
  result.hour = static_cast<uint8_t>(secondsOfDay / 3600);
  result.minute = static_cast<uint8_t>((secondsOfDay % 3600) / 60);
  result.second = static_cast<uint8_t>(secondsOfDay % 60);
  return result;
}

inline bool parseTwoDigits(const char *value, uint8_t &result) {
  if (value == nullptr || value[0] < '0' || value[0] > '9' ||
      value[1] < '0' || value[1] > '9') return false;
  result = static_cast<uint8_t>((value[0] - '0') * 10 + value[1] - '0');
  return true;
}

inline bool parseIsoUtc(const char *value, CivilDateTime &result) {
  if (value == nullptr || value[4] != '-' || value[7] != '-' ||
      value[10] != 'T' || value[13] != ':' || value[16] != ':' ||
      value[19] != 'Z' || value[20] != '\0') return false;
  for (uint8_t index = 0; index < 4; ++index) {
    if (value[index] < '0' || value[index] > '9') return false;
  }
  result.year = (value[0] - '0') * 1000 + (value[1] - '0') * 100 +
      (value[2] - '0') * 10 + value[3] - '0';
  if (!parseTwoDigits(value + 5, result.month) ||
      !parseTwoDigits(value + 8, result.day) ||
      !parseTwoDigits(value + 11, result.hour) ||
      !parseTwoDigits(value + 14, result.minute) ||
      !parseTwoDigits(value + 17, result.second)) return false;
  return validCivilDateTime(result);
}

inline bool formatUtcOffsetShort(const char *utc, int32_t offsetMinutes,
                                 char *output, size_t capacity) {
  if (output == nullptr || capacity < 12) return false;
  CivilDateTime parsed;
  int64_t seconds = 0;
  if (!parseIsoUtc(utc, parsed) || !civilToUnixSeconds(parsed, seconds)) {
    output[0] = '\0';
    return false;
  }
  const CivilDateTime local = civilFromUnixSeconds(
      seconds + static_cast<int64_t>(offsetMinutes) * 60);
  const int written = snprintf(output, capacity, "%02u-%02u %02u:%02u",
                               local.month, local.day,
                               local.hour, local.minute);
  return written == 11;
}

inline bool buildLocalDateTimeToUtcEpoch(const char *date, const char *clock,
                                         int32_t localOffsetMinutes,
                                         int64_t &utcEpoch) {
  if (date == nullptr || clock == nullptr || date[3] != ' ' ||
      date[6] != ' ' || clock[2] != ':' || clock[5] != ':') return false;
  static constexpr const char *kMonths[] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  CivilDateTime local;
  bool monthFound = false;
  for (uint8_t month = 0; month < 12; ++month) {
    if (date[0] == kMonths[month][0] && date[1] == kMonths[month][1] &&
        date[2] == kMonths[month][2]) {
      local.month = month + 1;
      monthFound = true;
      break;
    }
  }
  if (!monthFound) return false;
  const char dayTens = date[4] == ' ' ? '0' : date[4];
  const char dayValue[2] = {dayTens, date[5]};
  if (!parseTwoDigits(dayValue, local.day)) return false;
  for (uint8_t index = 7; index < 11; ++index) {
    if (date[index] < '0' || date[index] > '9') return false;
  }
  local.year = (date[7] - '0') * 1000 + (date[8] - '0') * 100 +
      (date[9] - '0') * 10 + date[10] - '0';
  if (!parseTwoDigits(clock, local.hour) ||
      !parseTwoDigits(clock + 3, local.minute) ||
      !parseTwoDigits(clock + 6, local.second)) return false;
  int64_t localEpoch = 0;
  if (!civilToUnixSeconds(local, localEpoch)) return false;
  utcEpoch = localEpoch - static_cast<int64_t>(localOffsetMinutes) * 60;
  return true;
}

}  // namespace pokepod
