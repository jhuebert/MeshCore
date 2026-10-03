// CliUtil.h — small helpers shared by the simple_repeater fork's CLI command
// handlers (filter, battery, and future ones). Fork-owned code; keeping the
// tokenizer and the reply writer in one place guarantees every CLI parses
// input and builds replies identically.
//
// nextToken() and radd() were previously duplicated (and had diverged) in
// PacketFilter.cpp and BatteryGate.cpp — new CLI helpers belong here instead.
// Same for parseIntRange(), which both CLIs spelled out per parameter.

#ifndef _CLI_UTIL_H
#define _CLI_UTIL_H

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

// Copy a command tail (everything after the verb) into a scratch buffer for
// nextToken() to consume in place, truncating safely at the buffer size.
// Deliberately dependency-free: both CLI handlers use it, and the battery
// gate's host test build links neither StrHelper nor ltoa().
inline void cliCopyCommand(char* buf, size_t sz, const char* command) {
  if (sz == 0) return;
  size_t n = strlen(command);
  if (n >= sz) n = sz - 1;
  memcpy(buf, command, n);
  buf[n] = 0;
}

// all CLI reply buffers are 160 B: main.cpp uses char reply[160] (serial and
// ethernet), and the mesh remote-CLI hands out &temp[5] of a uint8_t temp[166].
// Reply writers must bound to this, never MAX_PACKET_PAYLOAD.
#define CLI_REPLY_MAX 160

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

// Parse a decimal token that must be entirely a number in [lo, hi]: a trailing
// byte ("12x", "0x10"), an empty token and an out-of-range value are all
// refused. Callers keep their own error wording, which is why this only
// answers yes/no.
inline bool parseIntRange(const char* tok, long lo, long hi, long* out) {
  if (tok == NULL || tok[0] == 0) return false;
  char* end;
  long v = strtol(tok, &end, 10);
  if (end == tok || *end != 0 || v < lo || v > hi) return false;
  *out = v;
  return true;
}

#endif
