// PatternMatch.h — the user-facing sender=/text= pattern language: the vendored
// regex engine plus top-level '|' alternation and structural validation.
// Personal fork feature; this file pair is fork-owned like the rest of the
// packet filter's code set, and the vendored engine (TinyRegex.h/.cpp) stays
// untouched — everything a user can type is decided here.
//
// Dialect (see FILTER.md for the user-facing reference):
//   - '|' at the top level splits the pattern into alternatives, which are
//     matched in order, first match wins: "Alice|Bob" matches either name.
//     '\|' is a literal pipe and '|' inside a [...] class is a class member.
//   - Anchors bind to their own alternative, standard regex style:
//     "^Alice|Bob$" is "(^Alice)|(Bob$)". For an exact match of either, write
//     "^Alice$|^Bob$".
//   - There are no groups: '(' and ')' stay ordinary characters and a '|'
//     between them still splits, so "^(Alice|Bob)$" is the two alternatives
//     "^(Alice" and "Bob)$" — it matches neither Alice nor Bob. Escape a
//     literal parenthesis ("\("); parentheses are documented as not supported.
//   - At most FILTER_PATTERN_MAX_ALTS alternatives; an empty alternative
//     ("A|", "|A", "A||B") is rejected, because an empty branch matches
//     everything. Both are add-time rules only: when matching, a branch that
//     cannot match is skipped so a config stored before '|' was an alternation
//     keeps the branches it does have.
// A pattern is stored as text and re-split on every evaluation, so the
// compiled engine state never outlives one evaluation. Cost is bounded by the
// alternatives a stored pattern can hold (one per '|', within its length
// limit) x the engine's per-call step budget, and evaluation stops at the
// first alternative that exhausts that budget (fail-open).

#ifndef _PATTERN_MATCH_H
#define _PATTERN_MATCH_H

#include <stddef.h>

// patternValid() reason buffer size. The longest reason is
// "too many alternatives (max <int>)", which fits with room to spare.
#define PATTERN_ERR_MAX 48

// Syntax-check a pattern at add time. Returns false if it is rejected.
// `err` must be a buffer of at least `err_sz` bytes (err_sz >= 1 — PATTERN_ERR_MAX
// is enough for every reason written here) and is always written. It holds a
// short reason phrase only for rejections made here (empty
// alternative, too many alternatives), worded so the caller can say which field
// it came from — an empty `err` means the vendored engine rejected the pattern
// itself, so the caller should use its own "bad/long ... regex" wording.
bool patternValid(const char* pattern, char* err, size_t err_sz);

// True if the pattern matches anywhere in `subject`. Alternatives are tried in
// order and the first match wins. On step-budget exhaustion the whole
// evaluation gives up: no match, `*aborted` true, fail-open (the engine has
// always failed open). `aborted` may be NULL when the caller does not count
// budget aborts; it is written (false first) on every call, so it never carries
// an earlier call's answer.
bool patternMatches(const char* pattern, const char* subject, bool* aborted = NULL);

#endif // _PATTERN_MATCH_H
