// PatternMatch.cpp — top-level '|' alternation and structural validation over
// the vendored regex engine. See PatternMatch.h for the dialect.

#include "PatternMatch.h"
#include <stdio.h>
#include <string.h>

#include "PacketFilterConfig.h"
#include "TinyRegex.h"

// The split buffer must hold the longest storable pattern, and it sits on the
// stack in the packet path: size it by the wider of the two rule pattern
// storages, both of which are build-flag overridable (PacketFilterConfig.h).
#define PATTERN_SPLIT_MAX \
  ((FILTER_SENDER_PATTERN_LEN > FILTER_TEXT_PATTERN_LEN) ? FILTER_SENDER_PATTERN_LEN \
                                                        : FILTER_TEXT_PATTERN_LEN)

// Copy `pattern` into `buf` and terminate every top-level alternative in place,
// so the alternatives can be walked as consecutive NUL-terminated strings.
// Returns the alternative count (at least 1, empty branches included), or 0 if
// the pattern does not fit `buf`. Splitting is purely mechanical — what an
// empty or engine-rejected branch means is each caller's business. A '\'
// escape and a [...] class both hold literal bytes: '\|' and '[|]' are pipes,
// '[\]|]' is a class holding ']'.
static int splitAlternatives(const char* pattern, char* buf, size_t buf_sz) {
  size_t len = strlen(pattern);
  if (len + 1 > buf_sz) return 0;
  memcpy(buf, pattern, len + 1);

  int alts = 1;
  bool in_class = false;  // inside [...] — '|' there is a class member
  for (size_t i = 0; i < len; i++) {
    char c = buf[i];
    if (c == '\\') { i++; continue; }          // escaped byte
    if (in_class) {
      if (c == ']') in_class = false;
      continue;
    }
    if (c == '[') { in_class = true; continue; }
    if (c != '|') continue;
    buf[i] = 0;          // terminate this alternative in place
    alts++;
  }
  return alts;
}

bool patternValid(const char* pattern, char* err, size_t err_sz) {
  err[0] = 0;
  char buf[PATTERN_SPLIT_MAX];
  int alts = splitAlternatives(pattern, buf, sizeof(buf));
  if (alts == 0) return false;                 // too long for buf: the caller's check
  if (alts > FILTER_PATTERN_MAX_ALTS) {
    // reason phrases stay field-agnostic; the caller names the field they came from
    snprintf(err, err_sz, "too many alternatives (max %d)", FILTER_PATTERN_MAX_ALTS);
    return false;
  }
  const char* alt = buf;
  for (int i = 0; i < alts; i++, alt += strlen(alt) + 1) {
    // an empty branch matches everything, so it is never an accident the user
    // meant: "A|", "|A" and "A||B" are refused rather than quietly stored
    if (alt[0] == 0) {
      snprintf(err, err_sz, "empty alternative");
      return false;
    }
    if (re_compile(alt) == NULL) return false;   // engine rejects it: no reason given
  }
  return true;
}

bool patternMatches(const char* pattern, const char* subject, bool* aborted) {
  if (aborted) *aborted = false;
  char buf[PATTERN_SPLIT_MAX];
  int alts = splitAlternatives(pattern, buf, sizeof(buf));
  if (alts == 0) return false;   // pattern cannot split: never matches
  const char* alt = buf;
  for (int i = 0; i < alts; i++, alt += strlen(alt) + 1) {
    // Branches that cannot match are skipped rather than failing the whole
    // pattern: an empty one would match everything, and one the engine refuses
    // is broken. `filter add` rejects both, so only a config stored before '|'
    // was an alternation can hold one ("A||B") — the branches around it should
    // keep working.
    if (alt[0] == 0) continue;
    re_t compiled = re_compile(alt);
    if (compiled == NULL) continue;
    int matchlength;
    int idx = re_matchp(compiled, subject, &matchlength);
    if (re_budget_exhausted()) {   // fail-open
      if (aborted) *aborted = true;
      return false;
    }
    if (idx >= 0) return true;
  }
  return false;
}
