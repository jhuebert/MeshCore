// CliUtil.h — small helpers shared by the simple_repeater fork's CLI command
// handlers (filter, battery, fleet, clock). Fork-owned code; keeping the
// tokenizer and the reply writer in one place guarantees every CLI parses
// input and builds replies identically. New CLI helpers belong here, not in a
// handler.
//
// The reply side lives here for the same reason: CliBuf is the one bounded
// writer every CLI builds its reply with, cliPrepare() is the one command
// entry point, and cliCutReply() is the one place that knows how a reply says
// it was cut.

#ifndef _CLI_UTIL_H
#define _CLI_UTIL_H

#include <errno.h>
#include <limits.h>
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

// A fork reply writer never fills more than CLI_REPLY_MAX bytes (one-liners via
// snprintf(reply, CLI_REPLY_MAX, ...), staged output via CliBuf), never
// MAX_PACKET_PAYLOAD.
//
// CLI_REPLY_MAX is 157, three short of the 160-byte buffers those replies are
// written into (main.cpp's serial and ethernet `char reply[160]`; the mesh
// remote CLI's `&temp[5]` of a `uint8_t temp[166]` leaves 161). The missing
// three bytes are the core's companion-radio CLI form ("xx|command"): it
// copies that prefix back in front of the reply and then moves the pointer, so
// the handler writes a whole reply after it, into a buffer the fork does not
// own. Keeping the prefix's room here means no fork reply can overrun the
// smallest of them.
#define CLI_REPLY_MAX 157
#define CLI_REPLY_PREFIX_LEN 3
#define CLI_REPLY_BUFFER_MAX (CLI_REPLY_MAX + CLI_REPLY_PREFIX_LEN)
// the smallest upstream reply buffer: a prefixed fork reply must fit it
static_assert(CLI_REPLY_BUFFER_MAX <= 160,
              "a prefixed fork reply must fit the upstream CLI reply buffer");

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

// Cut a reply to at most `room` bytes (NUL included) and mark the cut, so a
// reply that does not fit says so instead of ending in silence. The text before
// the cut is kept exactly as it was — a line the cut landed inside stays
// visible, with nothing inserted into it — and the reply ends with "...".
//
// One marker, at the very end, and nothing else: nothing follows it, so "..."
// needs no second form to say where the cut landed. The tightest budget the
// fork has is the fleet's packet room (what a node name and a job key leave).
//
// `dst` must have room for `room` bytes; with no room nothing is written.
inline void cliCutReply(char* dst, size_t room, const char* src) {
  const char* marker = "...";     // the one end-of-reply marker
  const size_t marker_len = 3;
  if (room == 0) return;          // no room: write nothing, keep the caller's NUL
  size_t n = strlen(src);
  if (n + 1 <= room) {   // fits: unchanged, byte for byte
    memcpy(dst, src, n + 1);
    return;
  }
  if (room <= marker_len + 1) {   // no room to mark: a plain cut, silently
    memcpy(dst, src, room - 1);
    dst[room - 1] = 0;
    return;
  }
  // reserve the marker's room first: a cut reply has to say so
  size_t keep = room - 1 - marker_len;
  memcpy(dst, src, keep);
  memcpy(dst + keep, marker, marker_len + 1);
}

// A bounded, allocation-free builder for a fixed buffer — the one reply writer
// the fork's CLIs share. The caller asks the builder how much room is left
// instead of tracking a write pointer and a count, and a write that does not
// fit is dropped instead of overrunning the buffer.
//
// The buffer is the caller's: no heap, no copy-in. `cap` includes the NUL.
class CliBuf {
public:
  CliBuf(char* buf, size_t cap)
      : _start(buf), _out(buf), _remain(cap > (size_t)INT_MAX ? INT_MAX : (int)cap) {}

  // Append a formatted string. Never writes past the buffer.
  void add(const char* fmt, ...) {
    if (_remain <= 0) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(_out, (size_t)_remain, fmt, ap);
    va_end(ap);
    if (n < 0) { _remain = 0; return; }
    if (n >= _remain) { _out += _remain - 1; _remain = 0; return; }
    _out += n;
    _remain -= n;
  }
  void addStr(const char* s) { add("%s", s); }

  // Append `src`, cut to at most `limit` bytes (NUL included) and marked: a
  // reply that does not fit ends with "..." instead of ending in silence.
  // `limit` is further clamped to the room the buffer has left.
  void addCut(const char* src, size_t limit) {
    if (_remain <= 0) return;   // no room: nothing is written and nothing is skipped
    if (limit > (size_t)_remain) limit = (size_t)_remain;
    cliCutReply(_out, limit, src);
    size_t wrote = strlen(_out);
    _out += wrote;
    _remain -= (int)wrote;
    if (_remain <= 0) _remain = 0;
  }

  // The room left, NUL included: 0 when nothing more fits.
  int remain() const { return _remain; }
  size_t len() const { return (size_t)(_out - _start); }
  const char* c_str() const { return _start; }

private:
  char* _start;
  char* _out;
  int _remain;
};

// The shared first step of every CLI command: the quotes must balance (or the
// tokeniser would swallow the rest of the line into one value), and the
// command must fit the scratch buffer the tokeniser works in — the tokeniser
// edits its copy in place. Both refusals answer the operator in one place, so
// the CLIs cannot drift apart.
//
// `scratch` is the caller's buffer; on success it holds the copy to tokenise.
inline bool cliPrepare(const char* command, char* scratch, size_t scratch_sz, char* reply) {
  if (!cliQuotesBalanced(command)) {
    snprintf(reply, CLI_REPLY_MAX, "Err - unbalanced quotes");
    return false;
  }
  if (!cliCopyCommand(scratch, scratch_sz, command)) {
    snprintf(reply, CLI_REPLY_MAX, "Err - command too long");
    return false;
  }
  return true;
}

// True when `text` starts with `prefix`. Both are NUL-terminated.
inline bool cliStartsWith(const char* text, const char* prefix) {
  return text && strncmp(text, prefix, strlen(prefix)) == 0;
}

// True when a CLI reply reports a failure.
//
// The fork's own CLIs answer "Err - ...", but the core's CommonCLI has its own
// vocabulary: "Error", "Error, ...", "Error: ...", "ERROR: ...", "ERR: ...",
// "error", "Unknown command", "unknown config: ...". A fleet script runs core
// commands as well as fork ones, and `!ack err` promises "reply only on
// error" — so a typo'd command that answers "Unknown command" must not pass
// for a success, or the operator is told the job worked.
//
// The list is a prefix table: a reply is a failure when it starts with one of
// these. Nothing the core answers on success ("OK - ...", a value, a listing)
// starts with one of them.
inline bool cliIsErrorReply(const char* reply) {
  static const char* const kErrorPrefixes[] = {
      "Err",   // the fork's "Err - ...", and the core's "Error", "Error, ..."
      "ERR",   // the core's "ERR: ...", "ERROR: ..."
      "err",   // the core's "error"
      "Unknown command",
      "unknown config: ",
      "Board not supported",
      "Bridge not supported",
      "can't find custom var",
      "Can't find GPS",
      "gps provider not found",
      "gps toggle not found",
      "no custom var",
      "No extra SF configured",
      "Invalid extra SF config",
  };
  if (reply == NULL || reply[0] == 0) return false;
  for (size_t i = 0; i < sizeof(kErrorPrefixes) / sizeof(kErrorPrefixes[0]); i++) {
    if (strncmp(reply, kErrorPrefixes[i], strlen(kErrorPrefixes[i])) == 0) return true;
  }
  return false;
}

// The value after `prefix` in a command, or NULL when the command is not that
// one: the prefix must be followed by a space or the end of the command, so
// "get mqtt.interval" is not a match for the prefix "get mqtt.inter" and
// "get mqtt.intervalx" is not a match for "get mqtt.interval" either.
inline const char* cliCommandValue(const char* command, const char* prefix) {
  size_t len = strlen(prefix);
  if (command == NULL || strncmp(command, prefix, len) != 0 ||
      (command[len] && command[len] != ' ')) return nullptr;
  const char* value = command + len;
  while (*value == ' ') value++;
  return value;
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

// The PSK rule every channel key follows: exactly 16 or 32 bytes of hex (32 or
// 64 digits), decoded into the caller's buffer. The filter's channel store and
// the fleet channel both take their key this way, so the rule lives here once;
// each caller keeps its own error wording.
inline bool cliDecodePskHex(const char* hex, uint8_t* out, size_t capacity, int* out_len) {
  size_t hex_len = strlen(hex);
  if (hex_len != 32 && hex_len != 64) return false;
  int len = cliDecodeHex(hex, hex_len, out, capacity);
  if (len != 16 && len != 32) return false;
  *out_len = len;
  return true;
}

// Split a decrypted group-text body into "<sender>: <text>". Content is
// sender-controlled, so parse defensively and bounded by len (never strlen()).
// The decrypted block is zero-padded to the packet buffer, so the visible field
// ends at the first NUL — parsing past it would invent a colon (and an empty
// sender) out of padding. `has_text`/`has_sender` report which fields were
// really present, so a caller can tell "no sender field" from "empty text".
// Shared by the filter's text= selectors and the fleet manager's script
// intake: the definition of "what a message's sender and text are" must live
// in one place, or the two would see different bodies for the same message.
inline void parseGroupText(const uint8_t* data, size_t len, char* sender, size_t sender_sz,
                           char* text, size_t text_sz, bool* has_text, bool* has_sender) {
  sender[0] = 0;
  text[0] = 0;
  *has_text = false;
  *has_sender = false;
  if (len < 5) return;   // too short for ts(4) + txt_type(1)

  const uint8_t* p = data + 5;
  // bounded to the first NUL within the decrypted body: everything after it is
  // padding, not content
  size_t n = 0;
  const uint8_t* term = (const uint8_t*)memchr(p, 0, len - 5);
  if (term != NULL) n = (size_t)(term - p);
  else n = len - 5;

  const uint8_t* colon = NULL;
  for (size_t i = 0; i < n; i++) {
    if (p[i] == ':') { colon = &p[i]; break; }
  }

  if (colon == NULL) {   // no sender extractable; whole remainder is text
    *has_text = true;
    size_t cpy = n < text_sz - 1 ? n : text_sz - 1;
    memcpy(text, p, cpy);
    text[cpy] = 0;
    return;
  }

  size_t slen = (size_t)(colon - p);
  while (slen > 0 && (p[slen - 1] == ' ' || p[slen - 1] == '\t')) slen--;   // trim trailing spaces/tabs
  size_t cpy = slen < sender_sz - 1 ? slen : sender_sz - 1;
  memcpy(sender, p, cpy);
  sender[cpy] = 0;
  *has_sender = true;

  const uint8_t* tp = colon + 1;
  size_t tlen = n - (size_t)(tp - p);
  while (tlen > 0 && (*tp == ' ' || *tp == '\t' || *tp == '\r' || *tp == '\n')) { tp++; tlen--; }
  *has_text = true;
  cpy = tlen < text_sz - 1 ? tlen : text_sz - 1;
  memcpy(text, tp, cpy);
  text[cpy] = 0;
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
