// NativeTestStubs.cpp — small native-only implementations for the packet
// filter test env (see [env:native_packet_filter] in platformio.ini).

#include <cstdio>

#include "NativeShim.h"

// Arduino libc extra used by src/helpers/TxtDataHelpers.cpp; host builds
// don't have it. Only base 10 is exercised by the filter tests.
char* ltoa(long value, char* str, int base) {
  if (base == 10) { sprintf(str, "%ld", value); return str; }
  str[0] = 0;
  return str;
}
