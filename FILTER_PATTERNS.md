# Pattern Language: Reference

Companion reference to [FILTER.md](./FILTER.md). It covers every detail of `sender=` and `text=` patterns: syntax, alternation, quoting, quirks and troubleshooting. No other file is needed to use it.

## Sender and text fields

Group-text messages look like `SenderName: message text` after decryption.

- `sender=` matches the name **without** the colon: `SpamBot`, not `SpamBot:`. `text=` matches the message **only**: `BEACON 123`, not `SpamBot: BEACON 123`. Use both together in one rule when you need both.
- These conditions only apply to group text. They never match adverts or binary group data.
- The sender name and the text are read up to the end of the message, so a name of any length can be matched, and a pattern anchored with `$` sees the real end of the field rather than a shortened copy. A message with no `:` has no sender field at all: `sender=` cannot match it, not even with `^$`.
- A sender name may contain a colon, so the split is on the first **`: `** (colon-space), the separator senders write, rather than the first `:`. `DF)EQ4X:: hello` has the sender `DF)EQ4X:`, and a colon-space in the text (`Alice: note: hi`) does not split it. A message with no `: ` at all (`Alice:hi`) still splits on the bare colon.
- A name that itself contains `: ` cannot be recovered: the first `: ` is always the boundary, exactly as the official client splits it. So `some: name: test: this is a test: hello` has the sender `some` and the text `name: test: this is a test: hello`; use `text=` as well to target the rest.
- `.` counts **bytes**, not displayed characters. A name with accented letters or symbols is longer than it looks: `P/M£<$£H` is 8 characters but 10 bytes, so `sender=^........$` does not match it, and a pattern cannot count characters. When a name varies that way, match another stable property (`text=`, `len=`, `hops=`) instead of its length. See [Unicode and emoji notes](#unicode-and-emoji-notes).
- Matching is **case-sensitive**, and a pattern matches *anywhere* in the field unless you anchor it:

  | You want | Pattern | For `SpamBot: hello` |
  |---|---|---|
  | Name *contains* `SpamBot` | `sender=SpamBot` | Matches; also matches `MySpamBot2` |
  | Name *starts with* `SpamBot` | `sender=^SpamBot` | Matches; also matches `SpamBot2` |
  | Name is *exactly* `SpamBot` | `sender=^SpamBot$` | Matches; not `SpamBot2`, not `spambot` |

## Rules of thumb

1. Type the pattern as-is: `sender=^Bot$`, **not** `sender=/^Bot$/i`.
2. Matching is **case-sensitive** and searches anywhere unless anchored.
3. `^` at the start and `$` at the end make the match exact.
4. Use `.*` for "anything", not a shell-style `*`.
5. There are **no groups, counted repeats, or flags**.
6. `|` separates alternatives: `sender=^Alice$|^Bob$`. See [Alternation](#alternation) for anchors, the 8-alternative cap, and the escape for a literal pipe.

## Supported syntax

`sender=` and `text=` use a small pattern language (**TinyRegex**) built into the firmware. It is *not* JavaScript, Python, or PCRE regex: patterns copied from those systems often mean something different here.

| Syntax | Meaning | Example | Matches / does not match |
|---|---|---|---|
| Ordinary characters | Literal, case-sensitive | `Bot` | Matches `MyBot2`; not `bot` |
| `^` at pattern start | Start of the field | `^Bot` | Matches `Bot2`; not `MyBot` |
| `$` at pattern end | End of the field | `Bot$` | Matches `MyBot`; not `Bot2` |
| `^...$` | Whole field | `^Bot$` | Matches only `Bot` |
| `.` | Any one character | `^B.t$` | Matches `Bot`, `B7t`; not `Bt` |
| `*` | Zero or more of the previous item | `^ab*c$` | Matches `ac`, `abc`, `abbbc` |
| `+` | One or more of the previous item | `^Bot\d+$` | Matches `Bot1`, `Bot42`; not `Bot` |
| `?` | Zero or one of the previous item | `^colou?r$` | Matches `color`, `colour` |
| `[abc]` | One character from the set | `^Bot[ABC]$` | Matches `BotA`, `BotB`, `BotC` |
| `[a-z]` | One character in a range | `^[A-Z][0-9]$` | Matches `A7`; not `a7` |
| `[^abc]` | One character *not* in the set | `^[^0-9]+$` | Matches `abc`; not `abc2` |
| `\d` | A digit | `^\d\d$` | Matches `42`; not `4` |
| `\w` | Letter, digit, or underscore | `^\w+$` | Matches `Bot_42`; not `Bot-42` |
| `\s` | Any whitespace (space, tab, …) | `^RX\s+OK$` | Matches `RX OK` and `RX  OK` |
| `\.` (escaped punctuation) | The literal character | `^v1\.2$` | Matches `v1.2`; not `v1x2` |
| `A\|B` | Either alternative (up to 8) | `^Alice$\|^Bob$` | Matches `Alice` or `Bob`; not `Alice2` |

Tips:

- A quantifier applies only to the character right before it: `ab+` repeats the `b`, not `ab`.
- A quantifier needs something to repeat in front of it, and `^`/`$` anchor only at the edges of a pattern, so `*Bot` and `A^B` match nothing at all. `Bot\d+` and `^A.*B$` are what you want.
- `[Bot]` means one of the characters `B`, `o`, `t`, not the word `Bot`.
- `\s` includes tabs and line breaks. If you want exactly one space, quote the pattern and type the space: `text="^RX OK$"`.
- To match punctuation like `.` `[` `\` literally, put a backslash in front.

## Alternation

`|` at the top level of a `sender=`/`text=` pattern means OR, so one rule can cover several exact values:

```text
filter add chan=#test sender=^BotA$|^WeatherBot$
```

- **Anchors bind to their own alternative.** `^Alice|Bob$` means *starts with Alice* **or** *ends with Bob*, not "is Alice or Bob". Put both anchors on each side (`^Alice$|^Bob$`) when you mean an exact match.
- **There are no groups.** `|` splits wherever it appears at the top level, even between parentheses: `^(Alice|Bob)$` means *starts with `(Alice`* **or** *ends with `Bob)`*, neither Alice nor Bob. Write `^Alice$|^Bob$`; to match the literal name `(Alice|Bob)`, escape the pipe: `^(Alice\|Bob)$`.
- **At most 8 alternatives** per pattern; exceeding it is rejected. An empty alternative (`A|`, `|A`, `A||B`) is rejected too, because a blank branch would match everything. Both errors name the pattern they came from: `Err - too many alternatives (max 8) in sender regex`, `Err - empty alternative in sender regex`.
- **A literal pipe needs an escape:** `text=a\|b` matches `a|b`, and `|` inside brackets (`[|]`) is a literal pipe too. A pipe inside one of this guide's tables is written `\|`: that is the plain `|` you type, escaped so the table renders; the literal-pipe escape cannot be shown inside a table, so that case is spelled `[|]` there.
- **2026-10 semantics:** an unescaped `|` used to mean a literal pipe and now means OR. Write `\|` if you want a literal pipe. A pattern built on a bare quantifier (`*Bot`) or a misplaced anchor (`A^B`) is accepted as before, but now matches nothing instead of matching or not depending on whichever pattern the engine compiled before it.
- **Cost:** alternatives are tried in turn, and each one that fails to match is a full match attempt: `PING|PONG` costs about twice what `PING` alone does on every packet. Keep the branches cheap and anchored (`^PING$|^PONG$`); a pattern that runs out of match budget gives up fail-open and shows up as `aborted` in `filter stats`. The whole pattern, not each alternative, must fit the pattern-length limit. See [Limits](./FILTER.md#limits).
- Only the first alternative that matches matters; the rest are not tried.

`filter add sender=^Alice|Bob$` is accepted, but it means *starts with Alice* **or** *ends with Bob*: `Alice2`, `xxBob`, and `AliceBob` all match it. For an exact match of either name, write `filter add sender=^Alice$|^Bob$`.

After copying a pattern from another tool, verify with `filter get <idx>` and a test message.

## What doesn't work

These familiar features **do not exist** here, and a few are silently treated as ordinary text rather than rejected:

| You might try | What actually happens | Do this instead |
|---|---|---|
| Groups: `sender=^(Alice\|Bob)$` | The `|` splits even inside the parentheses, so this matches a name *starting with* `(Alice` or *ending with* `Bot)`, not Alice, not Bob | `sender=^Alice$\|^Bob$` |
| Quantifier with nothing to repeat: `sender=*Bot`, `sender=a**` | Accepted, but matches nothing: there is no symbol for the quantifier to repeat. Before 2026-10 it matched or not depending on whichever pattern matched before it | `sender=Bot\d*` |
| Anchor away from an edge: `sender=A^B` | Accepted, but matches nothing: `^` and `$` only anchor at the edges. Same caveat | `sender=^A.*B$` |
| Counted repeat: `\d{3}` | Matches literal `{3}` | `\d\d\d` |
| Case-insensitive: `/i`, `(?i)` | Not supported; always case-sensitive | `^[Bb][Oo][Tt]$` |
| Word boundary: `\b` | Matches the literal letter `b` | See the word-boundary recipe below |
| Lookahead: `(?=...)` | Not supported | Two fragments → two rules, or `RX.*OK` for ordered text |
| `\n`, `\t` escapes | Match the letters `n`, `t` | `\s` for whitespace |
| Newlines / multiline mode | No per-line matching | Patterns apply to the whole field |

## Recipe table

Patterns are values for `sender` or `text`, exactly as typed at the CLI.

| Goal | Pattern | Notes |
|---|---|---|
| Contains `BEACON` | `BEACON` | Also matches inside longer text |
| Starts with `BEACON` | `^BEACON` | |
| Ends with `BEACON` | `BEACON$` | |
| Exactly `BEACON` | `^BEACON$` | |
| Case choices for `bot` | `^[Bb][Oo][Tt]$` | Covers `bot`, `BOT`, `bOt` |
| Exactly three digits | `^\d\d\d$` | Instead of `\d{3}` |
| Two to four digits | `^\d\d\d?\d?$` | Instead of `\d{2,4}` |
| Optional sign and decimals | `^-?\d+\.?\d*$` | Matches `-12`, `12.5`, also `12.` |
| Literal brackets | `^\[TEST\]$` | Matches `[TEST]` |
| Nonempty name/ID characters | `^[A-Za-z0-9_]+$` | Matches `Bot_42`; not `Bot-42` |
| `RX` followed later by `OK` | `RX.*OK` | Not `OK then RX` |
| Inside brackets | `\[[^\]]*\]` | Matches `[...]` content |

**OR between names:** one rule, alternatives separated by `|`:

```text
filter add chan=#test sender=^BotA$|^BotB$|^BotC$
filter add chan=#test sender=^Bot     # covers the whole family
```

See [Alternation](#alternation) for the rules that apply to every alternative: the cap, the empty-branch rejection, and anchors that bind per alternative.

**Word boundaries:** there is no `\b`. To drop `BOT` as a standalone word (separated by spaces, or the whole text), use these alternatives as needed:

```text
filter add text=^BOT$          # the whole text is BOT
filter add text="^BOT "        # BOT at the start, then a space
filter add text=" BOT$"        # a space, then BOT at the end
filter add text=" BOT "        # BOT between spaces
```

**Two required fragments in any order:** there is no lookahead. If order matters, use `text=RX.*OK`. If either order is fine, add two otherwise identical rules, one with `text=RX.*OK` and one with `text=OK.*RX`.

## Spaces, quotes, and sending from code

**At the repeater CLI:** double quotes just keep spaces inside one value; backslashes pass through untouched. Don't double them.

```text
filter add text="^RX in place$"      # exactly one space at each gap
filter add text=^RX\s+in\s+place$    # one or more whitespace at each gap
filter add text=^v1\.2$              # literal dot
```

A literal double-quote character can't be passed through the CLI (quotes are stripped). Use `.` as a stand-in character if needed, e.g. `text=^say.OK.$` matches `say"OK"`, but also `say!OK!`.

**From Python:** use a raw string so backslashes reach the repeater intact:

```python
command = r'filter add text=^ID:\s*\d\d\d$'
```

**From JavaScript:** double the backslashes, or use `String.raw`:

```javascript
const command = 'filter add text=^ID:\\s*\\d\\d\\d$';
// or: const command = String.raw`filter add text=^ID:\s*\d\d\d$`;
```

If you go through a shell or JSON, those layers add their own escaping, so always confirm what the repeater actually stored with `filter get <idx>`.

## Unicode and emoji notes

- Literal text with emoji or accented letters works: `sender=^John😀$` is fine.
- Internally each emoji or accented character counts as several **bytes**, so `.` won't match a whole one and that byte length eats into the [pattern-length limits](./FILTER.md#limits). For example `£` is two bytes, so `P/M£<$£H` is ten bytes even though it looks like eight characters. A pattern cannot count characters, so there is no `sender=^........$`-style rule for "exactly N characters" when multibyte characters are in play.
- There is no case-insensitive option. Use explicit choices like `[Bb]`, and note that accented letters can't be handled that way; match them literally.

## Troubleshooting

Work up from one narrow rule and confirm each piece before combining:

```text
filter add chan=#test sender=^Bot[0-9]+$ text=^BEACON action=forward
filter on
filter list
filter get 0
filter stats
```

| Symptom | Check |
|---|---|
| Typo in a command did nothing | Nothing is ignored any more: a command with an unexpected extra word, unbalanced quotes, or an over-long line is refused with a usage line and changes nothing |
| A channel is missing from `filter chan list` | The store holds more than fits one reply: the listing ends with `next=N`; run `filter chan list N` for the rest |
| `filter list` stops at a `...` line, or a rule ends with `..` | The listing did not fit one reply: run `filter list <last-index + 1>` (the number after the last line shown) for the rest. A rule ending in `..` was too large for its line: `filter get <idx>` shows more of it. A reply the fleet cut to fit one packet ends with `...` too. See [FLEET.md](./FLEET.md) |
| Rule gets no hits | Is the filter on (`filter on`)? Is the rule enabled? Did an earlier rule match first? |
| Sender/text never matches | Can the repeater decrypt the channel? Is it group text? Are you matching the right field (sender without colon, text without name)? |
| A `sender=^........$`-style length rule misses a name | `.` counts UTF-8 bytes, not characters, so accented letters and symbols count as several. Match `text=`/`len=`/`hops=` instead, or a byte-length pattern. See [Unicode and emoji notes](#unicode-and-emoji-notes) |
| Message still arrives despite a matching rule | This repeater only controls its own relaying: another repeater or a direct path can still deliver it. On this node check `filter get <idx>` (hits), `filter stats`, `filter status`, and `battery` (suspension skips content rules) |
| Matches too broadly | Add `^`/`$` anchors; escape literal dots; remember `sender=Bot` matches `MyBot2` |
| Combined rule matches nothing | Every condition must be true: test each one alone; check `route=`/case |
| Pattern from an online tester misbehaves | Remove `/slashes/`, flags, groups, `{counts}`, `\b`. See the table above |
| `Err - empty alternative in sender regex` | A `|` with nothing on one side: write `^A$\|^B$`, never end a pattern with a bare pipe |
| `Err - too many alternatives (max 8) in text regex` | Fewer alternatives per pattern, a broader one (e.g. `^Bot`), or a second rule |
| Rule never fires, pattern looks odd (`*Bot`, `a**`, `A^B`) | Such a pattern matches nothing by design: nothing to repeat, or an anchor that is not at an edge. Rewrite it (`Bot\d*`, `^A.*B$`) |
| Wrote `^(A\|B)$` and nothing matches | There are no groups: the `|` splits anyway, so it means *starts with `(A`* **or** *ends with `B)`*. Write `^A$\|^B$` |
| "Bad/long regex" error | Pattern too long (see [Limits](./FILTER.md#limits)) or broken syntax; shorten or simplify |
| `aborted` counter grows | Pattern too complex, simplify it |
| Drop rule never fires | An earlier overlapping rule (often a `forward` probe) is matching first |
| A rule vanished after a reboot | The saved config was truncated or corrupt. Rules load only if each record is one this firmware could have written; the records that survived keep their indices, and everything from the first bad record on is dropped. Add the missing rules again with `filter add` |
| A rule with `chan=` matches nothing after removing a channel | Expected: deleting the last channel a rule named leaves the mask empty, and the rule stays inert rather than becoming a catch-all. Point it at a channel that exists, or drop the `chan=` |
