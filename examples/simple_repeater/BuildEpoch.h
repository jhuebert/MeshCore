// BuildEpoch.h — UTC build epoch from __DATE__/__TIME__.
//
// Bakes a firmware build time into the image with no build-system change, so
// it is trivially removable. The parse is `inline` rather than `constexpr`
// because the ESP32/nRF52 toolchains build as C++11, where a constexpr body
// must be a single return statement; the input is a string literal, so the
// optimizer folds it to a constant anyway. It uses the compiler's local time;
// callers treat it as a lower plausibility bound and absorb the up-to-~1 day
// local-vs-UTC difference with their own grace (see
// MESHCLOCK_BUILD_GRACE_SECS), never as an exact comparison.

#ifndef _BUILD_EPOCH_H
#define _BUILD_EPOCH_H

#include <stdint.h>

namespace meshclock {

// Days since 1970-01-01 for a civil date (Howard Hinnant's algorithm).
inline int64_t daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (int64_t)doe - 719468;
}

// __DATE__ month name ("Jan".."Dec") to 1..12.
inline int monthFromName(const char* s) {
  return (s[0]=='J' && s[1]=='a') ? 1 : (s[0]=='F') ? 2 :
         (s[0]=='M' && s[2]=='r') ? 3 : (s[0]=='A' && s[1]=='p') ? 4 :
         (s[0]=='M') ? 5 : (s[0]=='J' && s[2]=='n') ? 6 :
         (s[0]=='J') ? 7 : (s[0]=='A') ? 8 : (s[0]=='S') ? 9 :
         (s[0]=='O') ? 10 : (s[0]=='N') ? 11 : 12;
}

// __DATE__ = "Mmm DD YYYY" (day space-padded), __TIME__ = "HH:MM:SS".
inline uint32_t buildEpoch() {
  const char* d = __DATE__;
  const int mon  = monthFromName(d);
  const int day  = (d[4] == ' ') ? (d[5] - '0')
                                 : (d[4] - '0') * 10 + (d[5] - '0');
  const int year = (d[7]-'0')*1000 + (d[8]-'0')*100 + (d[9]-'0')*10 + (d[10]-'0');
  const char* t = __TIME__;
  const int hh = (t[0]-'0')*10 + (t[1]-'0');
  const int mm = (t[3]-'0')*10 + (t[4]-'0');
  const int ss = (t[6]-'0')*10 + (t[7]-'0');
  return (uint32_t)(daysFromCivil(year, mon, day) * 86400 + hh*3600 + mm*60 + ss);
}

} // namespace meshclock

#endif // _BUILD_EPOCH_H
