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

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Hex digit value: 0..15, or -1 when the character is not a hex digit.
inline int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

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
// nextToken() to consume in place. Returns false if the command does not fit.
//
// Reporting the overflow matters: truncating a command and then acting on the
// prefix is safe memory, but not safe policy — `filter chan add name <psk>...`
// cut short would store a mangled name. A command that does not fit is refused
// whole.
// Deliberately dependency-free: both CLI handlers use it, and the battery
// gate's host test build links neither StrHelper nor ltoa().
inline bool cliCopyCommand(char* buf, size_t sz, const char* command) {
  if (sz == 0) return false;
  size_t n = strlen(command);
  if (n >= sz) return false;
  memcpy(buf, command, n + 1);
  return true;
}

// all CLI reply buffers are 160 B: main.cpp uses char reply[160] (serial and
// ethernet), and the mesh remote-CLI hands out &temp[5] of a uint8_t temp[166].
// Reply writers must bound to this, never MAX_PACKET_PAYLOAD.
#define CLI_REPLY_MAX 160

// An odd number of double quotes means the tokenizer would swallow the rest of
// the line into one value. Cheap to check up front, and it turns a silent
// misparse into an error the user can see.
inline bool cliQuotesBalanced(const char* command) {
  int n = 0;
  for (const char* s = command; *s; s++) if (*s == '"') n++;
  return (n % 2) == 0;
}

// Refuse a trailing token the command form does not take, using that form's
// usage line. Silently ignoring the tail turns a typo into a no-op that still
// replies OK, which is the worst failure mode a management command can have:
// `filter off junk` would report success while doing something the user did not
// ask for, or the opposite. Call this BEFORE any side effect.
inline bool cliNoExtra(char* p, char* reply, const char* usage) {
  if (nextToken(&p) != NULL) {
    snprintf(reply, CLI_REPLY_MAX, "%s", usage);
    return false;
  }
  return true;
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

// A "token" charset shared by !id job keys (CliScript) and fleet tags
// (FleetManager): 1..max_len chars of [A-Za-z0-9._-]. One validator, so the key
// grammar and the tag grammar can never drift apart — a tag is the same kind
// of string a job key is.
inline bool cliValidToken(const char* s, size_t max_len) {
  size_t n = strlen(s);
  if (n == 0 || n > max_len) return false;
  for (const char* c = s; *c; c++) {
    if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
          (*c >= '0' && *c <= '9') || *c == '.' || *c == '_' || *c == '-')) return false;
  }
  return true;
}

// Hex decoder for PSK entry (16/32-byte keys); PSKs are shared/entered as hex.
// `capacity` is the caller's output buffer: an over-long or odd-length input is
// refused before any byte is written, so a rejected key can never partially
// overwrite a live slot. Returns the decoded byte count, or 0 on any refusal.
inline int cliDecodeHex(const char* in, size_t in_len, uint8_t* out, size_t capacity) {
  if ((in_len & 1) != 0 || in_len / 2 > capacity) return 0;
  for (size_t i = 0; i < in_len; i += 2) {
    int hi = hexNibble(in[i]), lo = hexNibble(in[i + 1]);
    if (hi < 0 || lo < 0) return 0;
    out[i / 2] = (uint8_t)((hi << 4) | lo);
  }
  return (int)(in_len / 2);
}

// Parse a decimal token that must be entirely a number in [lo, hi]: a trailing
// byte ("12x", "0x10"), an empty token and an out-of-range value are all
// refused. Callers keep their own error wording, which is why this only
// answers yes/no. `out` is left untouched unless the whole token parses, so a
// rejected value can never leave a stale one behind.
inline bool parseIntRange(const char* tok, long lo, long hi, long* out) {
  if (tok == NULL || tok[0] == 0) return false;
  errno = 0;
  char* end;
  long v = strtol(tok, &end, 10);
  if (end == tok || *end != 0 || errno == ERANGE || v < lo || v > hi) return false;
  *out = v;
  return true;
}

#endif
