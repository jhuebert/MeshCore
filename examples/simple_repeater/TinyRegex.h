/*
 * TinyRegex.h — vendored from kokke/tiny-regex-c (public domain), with a
 * backtracking step budget added for use in MeshCore repeater firmware
 * (see PacketFilter.h). Single-file, no-heap, iterative matcher.
 *
 * Original: https://github.com/kokke/tiny-regex-c (public domain / unlicense)
 *
 * Modifications vs upstream:
 *   - fixed step budget: re_matchp() aborts and reports failure (fail-open)
 *     if the budget set by re_set_step_budget() is exhausted, protecting
 *     against catastrophic backtracking on attacker-controlled input;
 *   - re_budget_exhausted() reports whether the last match aborted;
 *   - renamed re.h/re.c to TinyRegex.h/TinyRegex.cpp.
 *
 * Supports:
 * ---------
 *   '.'        Dot, matches any character
 *   '^'        Start anchor, matches beginning of string
 *   '$'        End anchor, matches end of string
 *   '*'        Asterisk, match zero or more (greedy)
 *   '+'        Plus, match one or more (greedy)
 *   '?'        Question, match zero or one (non-greedy)
 *   '[abc]'    Character class, match if one of {'a', 'b', 'c'}
 *   '[^abc]'   Inverted class, match if NOT one of {'a', 'b', 'c'}
 *   '[a-zA-Z]' Character ranges, the character set of the ranges { a-z | A-Z }
 *   '\s'       Whitespace, \t \f \r \n \v and spaces
 *   '\S'       Non-whitespace
 *   '\w'       Alphanumeric, [a-zA-Z0-9_]
 *   '\W'       Non-alphanumeric
 *   '\d'       Digits, [0-9]
 *   '\D'       Non-digits
 *
 * NOTE: no groups '(...)' and no alternation '|'. NOTE: '\n' and '\t' are NOT
 * C-style escapes ('\n' matches a literal 'n'); use '\s' for whitespace.
 */

#ifndef _TINY_REGEX_C
#define _TINY_REGEX_C

#ifdef __cplusplus
extern "C" {
#endif

/* Typedef'd pointer to get abstract datatype. */
typedef struct regex_t* re_t;

/* Compile regex string pattern to a regex_t-array. Returns NULL on an
   invalid pattern (use for add-time syntax validation). */
re_t re_compile(const char* pattern);

/* Find matches of the compiled pattern inside text. Returns the offset of the
   first match, or -1. Aborts with failure if the step budget is exhausted. */
int re_matchp(re_t pattern, const char* text, int* matchlength);

/* Find matches of the txt pattern inside text (will compile automatically first). */
int re_match(const char* pattern, const char* text, int* matchlength);

/* Set the backtracking step budget for the next re_matchp()/re_match() calls.
   A budget of <= 0 means unlimited. Default: RE_STEP_BUDGET_DEFAULT. */
void re_set_step_budget(int steps);

/* Non-zero if the last re_matchp()/re_match() aborted on budget exhaustion. */
int re_budget_exhausted(void);

#ifdef __cplusplus
}
#endif

#endif /* ifndef _TINY_REGEX_C */
