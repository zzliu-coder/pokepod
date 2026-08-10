#include <assert.h>
#include <string.h>

#include "TimePolicy.h"

using namespace pokepod;

int main() {
  char output[12];
  assert(formatUtcOffsetShort("2026-08-10T13:28:17Z",
                              kChinaStandardTimeOffsetMinutes,
                              output, sizeof(output)));
  assert(strcmp(output, "08-10 21:28") == 0);

  assert(formatUtcOffsetShort("2026-12-31T20:30:00Z", 8 * 60,
                              output, sizeof(output)));
  assert(strcmp(output, "01-01 04:30") == 0);
  assert(formatUtcOffsetShort("2024-02-29T20:01:00Z", 8 * 60,
                              output, sizeof(output)));
  assert(strcmp(output, "03-01 04:01") == 0);
  assert(!formatUtcOffsetShort("2026-02-29T00:00:00Z", 8 * 60,
                               output, sizeof(output)));
  assert(!formatUtcOffsetShort("2026-08-10T13:28:17+08:00", 8 * 60,
                               output, sizeof(output)));

  int64_t utcEpoch = 0;
  assert(buildLocalDateTimeToUtcEpoch("Aug 10 2026", "21:28:17",
                                      kChinaStandardTimeOffsetMinutes,
                                      utcEpoch));
  CivilDateTime utc = civilFromUnixSeconds(utcEpoch);
  assert(utc.year == 2026 && utc.month == 8 && utc.day == 10);
  assert(utc.hour == 13 && utc.minute == 28 && utc.second == 17);
  assert(buildLocalDateTimeToUtcEpoch("Feb 29 2024", "00:10:20", 8 * 60,
                                      utcEpoch));
  utc = civilFromUnixSeconds(utcEpoch);
  assert(utc.year == 2024 && utc.month == 2 && utc.day == 28);
  assert(utc.hour == 16 && utc.minute == 10 && utc.second == 20);
  assert(!buildLocalDateTimeToUtcEpoch("Feb 29 2025", "00:00:00", 8 * 60,
                                       utcEpoch));
  return 0;
}
