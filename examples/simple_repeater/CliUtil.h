// CliUtil.h — small helpers shared by the simple_repeater fork's CLI command
// handlers (filter, battery, and future ones). Fork-owned code; keeping the
// tokenizer and the reply writer in one place guarantees every CLI parses
// input and builds replies identically.
//
// nextToken() and radd() were previously duplicated (and had diverged) in
// PacketFilter.cpp and BatteryGate.cpp — new CLI helpers belong here instead.

#ifndef _CLI_UTIL_H
#define _CLI_UTIL_H

#include <stdarg.h>
#include <stdio.h>

// split the next space-separated token off in place; spaces inside double
// quotes stay part of the token and the quote characters themselves are
// stripped (callers taking regex values reject an odd quote count for a
// clean error instead of silently swallowing the tokens that follow)
inline char* nextToken(char** p) {
  char* s = *p;
  while (*s == ' ') s++;
  if (*s == 0) { *p = s; return NULL; }
  char* t = s;
  bool inQ = false;
  char* w = s;
  while (*s) {
    if (*s == '"') { inQ = !inQ; s++; continue; }
    if (*s == ' ' && !inQ) break;
    *w++ = *s++;
  }
  // skip the delimiter before terminating, so *s still holds the original byte
  if (*s) s++;
  *w = 0;
  *p = s;
  return t;
}

// bounded reply append (CLI reply buffer is 160 bytes)
inline void radd(char** out, int* remain, const char* fmt, ...) {
  if (*remain <= 0) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(*out, *remain, fmt, ap);
  va_end(ap);
  if (n < 0) { *remain = 0; return; }
  if (n >= *remain) { *out += *remain - 1; *remain = 0; return; }
  *out += n;
  *remain -= n;
}

#endif
