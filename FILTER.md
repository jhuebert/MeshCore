# Repeater Packet Filter — User Guide

A remotely configurable packet filter for the `simple_repeater` firmware. You can
add, inspect, and remove filter rules over the repeater CLI — locally via serial,
or remotely over the mesh as an authenticated admin. No rebuild or physical
access to the repeater is required.

> Maintained on the [jhuebert fork](https://github.com/jhuebert/MeshCore) — prebuilt
> firmware is on the fork's Releases page (`filter-v*` tags).

**Quick navigation:** [Quick start](#2-quick-start) ·
[Common setups](#8-common-setups) ·
[Command reference](#7-command-reference) ·
[Writing sender/text patterns](#13-writing-sendertext-patterns)

---

## 1. What it does

The filter looks at every packet that reaches the repeater and decides whether
to **forward** it or **drop** it.

- Each rule is a list of conditions. A packet that meets **all** of a rule's
  conditions is handled by that rule's action.
- If no rule matches, the packet is forwarded as usual. The filter only ever
  removes traffic you explicitly target.
- Rules that match on channel name, sender, or message text look at the
  *decrypted* message, so they only fire on traffic the repeater can actually
  read. Other rules (type, route, hops, signal strength, …) work on every packet.

Everything is saved on the repeater and survives reboots.

## 2. Quick start

Silence one noisy channel, keeping everything else:

```text
filter add chan=#memes
filter on
filter stats          # watch hits climb as #memes traffic arrives
```

That's it. Rules default to **drop**, and `filter on` switches the whole filter
on. To undo: `filter del 0` (use the index the repeater printed when you added
the rule) or `filter clear` to remove all rules.

## 3. How rules work

### One rule = conditions ANDed together

Every condition in a single `filter add` command must be true for that rule to
act. Adding more conditions narrows the rule:

```text
filter add chan=#test sender=^SpamBot$ text=^BEACON hops=[2,*]
```

This drops messages on `#test` **from** `SpamBot` **whose** text starts with
`BEACON` **and** that have already hopped at least twice. If even one condition
fails, this rule does nothing.

| Channel | Sender | Text | Flood hops | This rule acts? |
|---|---|---|---|---|
| `#test` | `SpamBot` | `BEACON 123` | 2 | Yes |
| `#other` | `SpamBot` | `BEACON 123` | 2 | No — wrong channel |
| `#test` | `Alice` | `BEACON 123` | 2 | No — wrong sender |
| `#test` | `SpamBot` | `Hello` | 2 | No — wrong text |
| `#test` | `SpamBot` | `BEACON 123` | 1 | No — too few hops |

### Want "either/or"? Use two rules

Separate rules are independent alternatives — either one can act. To drop
messages from `SpamBot` **or** messages starting with `BEACON` on `#test`,
repeat the shared channel in each rule:

```text
filter add chan=#test sender=^SpamBot$
filter add chan=#test text=^BEACON
```

### Good to know

- Some values accept comma-separated alternatives, which acts as an OR inside
  that one condition: `chan=#local,#weather`, `type=txt,data`. Only `type`,
  `chan`, `hsize`, and `region` support this. `sender=Alice,Bob` searches for
  the literal text `Alice,Bob` — it is not a list.
- The **first matching rule wins**. Once a rule acts, later rules are not
  consulted for that packet.
- Name each condition once per rule. Repeating a key (e.g. a second `text=`)
  replaces the earlier value rather than adding another condition.
- A rule without `type=` applies to **all** packet types, including adverts and
  telemetry. Add `type=` when you want to narrow it.
- Invalid input is rejected with `Err - ...`; no half-added rule is left behind.

## 4. What you can match on

Each `filter add` takes one or more of these `key=value` conditions, separated
by spaces. Values containing spaces go in double quotes: `text="^RX in place"`.

| Condition | Values | Matches |
|---|---|---|
| `type` | `advert`, `txt`, `data` (comma-combine) | Packet category. `txt` means **group text**, `data` means **group data** |
| `route` | `flood` or `direct` | How the packet travelled; omit to match either |
| `hops` | interval | Flood hop count (direct packets never match) |
| `len` | interval | Payload size in bytes |
| `snr` | interval, dB | Received signal strength at your repeater |
| `path` | `[^]HEX>HEX>…[$]` | Repeater IDs on the flood path (see §9) |
| `hsize` | `1..4` (comma-combine) | Path ID size used by the packet |
| `chan` | channel name(s) | Keyed channel — proven by decryption (see §5) |
| `chanhash` | 2 hex digits | On-air channel tag, no key needed (see §9) |
| `region` | region name(s), `unscoped` (comma-combine) | Flood region the packet arrived in (direct packets never match) |
| `sender` | pattern | Sender name in group text, e.g. `SpamBot` from `SpamBot: hello` |
| `text` | pattern | Message text in group text |
| `action` | `drop` (default) or `logonly` | What to do on a match (see §10) |

`chan`, `sender`, and `text` are content conditions: they are checked after the
message is decrypted. Everything else is checked before forwarding and works on
every packet, including adverts.

### Interval syntax

Used by `hops`, `len`, and `snr`:

| You type | Meaning |
|---|---|
| `3` | exactly 3 |
| `[2,*]` | 2 or more (endpoints included) |
| `(2,5)` | more than 2 and less than 5 (endpoints excluded) |
| `[*,-4]` | −4 or less (handy for `snr`) |

Examples: `hops=[2,*]` = at least 2 hops · `len=[*,80]` = at most 80 bytes ·
`snr=[*,-8.5]` = signal of −8.5 dB or weaker. Fractional SNR values use
quarter-dB steps: `.00`, `.25`, `.50`, `.75`.

## 5. Channels, senders, and message text

### Channels

Content rules match against the repeater's store of named channels:

- `Public` is built in with the well-known Public key.
- `#name` channels need no key — the key is derived from the name, exactly as
  the companion apps do. Just write `chan=#memes` in a rule; the channel is
  added to the store automatically.
- Private (non-`#`) channels need a key: add them first with
  `filter chan add <name> <psk-b64>`, then reference the name in a rule.
- Channel names are literal, not patterns: `chan=#test.*` does **not** mean
  "all test channels".
- `chan=` only matches traffic the repeater can actually decrypt with the
  stored key — a strong identity check. By contrast, `chanhash=` matches a
  1-byte tag carried on-air, which other channels can collide with; use it only
  when you don't have (or want) the channel's key.
- The store holds at most 16 channels. If it fills up, `filter chan list`
  shows what's stored and `filter chan del <name>` frees a slot.

### Sender and text

Group-text messages look like `SenderName: message text` after decryption.

- `sender=` matches the name **without** the colon: `SpamBot`, not `SpamBot:`.
- `text=` matches the message **only**: `BEACON 123`, not `SpamBot: BEACON 123`.
- Use `sender=` **and** `text=` together in one rule when you need both.
- These conditions only apply to group text. They never match adverts or
  binary group data.
- Matching is **case-sensitive**, and a pattern matches *anywhere* in the
  field unless you anchor it. `sender=SpamBot` also matches `MySpamBot2`;
  `sender=^SpamBot$` matches exactly `SpamBot` and nothing else. See
  §13 for the full pattern language.

## 6. Cutting advert noise (rate limiter)

Independent of the rules, you can rate-limit flood adverts per originating
node: *each node's advert is forwarded at most once every N hours.*

```text
filter ratelimit advert 48     # each origin forwarded at most every 48 h
filter ratelimit clear         # optional: forget history, start clean
filter ratelimit               # show current window and cache usage
```

Use this on a well-connected repeater to stop re-flooding everyone's periodic
adverts while still passing each node's advert once per window so it stays
reachable through you. The window can be 0 (off) to 720 hours; 0 turns it off.

## 7. Command reference

All commands begin with `filter`. They work identically over serial and remote
admin (see §11).

| Command | Effect |
|---|---|
| `filter` | Status: on/off, rule and channel counts, ratelimit, counters |
| `filter on` / `filter off` | Enable/disable the whole filter (rules are kept) |
| `filter add <cond>=<val> ...` | Add a rule (space-separated conditions, see §4) |
| `filter list` | One-line summary of every rule |
| `filter get <idx>` | Full detail of one rule, including its hit count |
| `filter enable <idx>` / `filter disable <idx>` | Toggle a single rule |
| `filter del <idx>` | Delete a rule (later rules shift down one index) |
| `filter clear` | Delete all rules (channels and ratelimit are kept) |
| `filter chan` / `filter chan list` | List the stored channels |
| `filter chan add <name> [<psk-b64>]` | Add a channel; key optional for `#` names |
| `filter chan del <name>` | Remove a channel (existing rules are updated) |
| `filter ratelimit` | Show the advert ratelimit window and cache usage |
| `filter ratelimit advert <hours>` | Set the window (0–720 h; 0 = off) |
| `filter ratelimit clear` | Empty the advert cache |
| `filter stats` | Hit counters per rule |
| anything else | Usage line listing the commands |

Notes:

- The repeater replies with short lines; `filter get <idx>` gives the most
  detail about a rule.
- Deleting a rule shifts later indexes down — re-check `filter list` after
  deletions.
- Counters (`filter stats`) reset to zero on reboot; the rules themselves do not.

## 8. Common setups

These are ready-to-use recipes. Lines starting with `#` are comments — don't
send them to the repeater.

### Silencing one channel on a shared repeater

A `#memes` channel is eating your airtime, but you want everything else forwarded:

```text
filter add chan=#memes
filter on
```

### Keeping local traffic local (hop-count gating)

You carry `#wardriving` but won't relay it across the wider mesh: anything that
has already hopped twice or more goes no further through you.

```text
filter add chan=#wardriving hops=[2,*]
```

Zero or one hop (originators and first relays) still passes.

### Blocking a bot by name

A bot floods several channels you can decrypt. Scope the rule to the channels
where it's a problem:

```text
filter add chan=#local,#weather sender=^BotName$

# match anywhere in the name instead of exactly:
filter add chan=#local,#weather sender=BotName

# match the sender AND the message text:
filter add chan=#local sender=^BotName$ text="^RX in place"
```

### Cutting advert noise

See §6 — the one-liner is `filter ratelimit advert 48`.

### Isolating one upstream repeater

One repeater (ID prefix `A1B2C3`) keeps injecting junk into flood traffic.
Drop anything whose recorded flood path *starts* with it:

```text
filter add route=flood path=^A1B2C3

# only adverts it relayed:
filter add type=advert route=flood path=^A1B2C3
```

The first path entry is the earliest recorded relay, not necessarily your
immediate neighbour. To match the *most recent* relay instead, use
`path=A1B2C3$`. IDs can be written with 2, 4, 6, or 8 hex digits per entry;
chain up to four with `>`, and use `^`/`$` to anchor the first/last entry.

### Keeping out-of-region traffic off a channel

Flood packets can carry a region scope. To forbid plain, scope-less flood
traffic on a channel:

```text
filter add chan=#mychan region=unscoped
```

Or drop traffic from specific regions on any channel:

```text
filter add region=Foo,Bar
```

Region names must already exist on the repeater (`region def ...`). Direct
packets have no region and are never affected.

### Turning a rule off temporarily

Disable rather than delete, so you can re-enable it later:

```text
filter disable 2     # keep rule 2 but skip it
filter enable 2      # put it back
```

## 9. Examples for every condition

Complete, independent commands. Anything not specified is unrestricted. Rules
default to dropping — add `action=logonly` if you want to just count matches
first (see §10).

### `type`

| Command | What matches |
|---|---|
| `filter add type=advert` | Adverts, regardless of route |
| `filter add type=txt` | Group text on any channel, without needing its key |
| `filter add type=data route=flood` | Group data AND flood route |
| `filter add type=txt,data snr=[*,-8.5]` | (Group text OR group data) AND weak signal |

An advert does not match `type=txt,data`. To cover all packet types, omit
`type` entirely.

### `route`

| Command | What matches |
|---|---|
| `filter add route=flood` | Flood traffic, scoped or unscoped |
| `filter add route=direct` | Direct-routed traffic |
| `filter add route=flood len=[180,*]` | Flood traffic AND large payloads |

`route=direct` describes routing, not private messages. Combining it with
`hops` or `region` gives a rule that never matches, since those require flood.

### `hops`

Hops counts how many repeaters are already on the flood path. Direct packets
never match, even with `hops=[0,*]`.

| Command | What matches |
|---|---|
| `filter add hops=0` | Flood traffic with no path entries (originated here) |
| `filter add hops=3` | Exactly 3 flood hops |
| `filter add chan=#wardriving hops=[2,*]` | `#wardriving` AND at least 2 hops |
| `filter add hops=(2,5)` | 3 or 4 hops — neither 2 nor 5 |

### `len`

Payload size in bytes — not the number of characters shown in an app, and not
the whole radio frame. Accented/emoji characters can count as several bytes.

| Command | What matches |
|---|---|
| `filter add len=100` | Exactly 100 payload bytes |
| `filter add len=[180,*]` | 180 bytes or more |
| `filter add type=txt len=[*,80]` | Group text AND at most 80 payload bytes |

### `snr`

Signal strength measured at **your** repeater, in dB. Weak-signal traffic is
not always unwanted — probe before you drop.

| Command | What matches |
|---|---|
| `filter add snr=[*,-8.5]` | −8.5 dB or weaker |
| `filter add snr=(-8.5,0]` | Stronger than −8.5, up to 0 dB |
| `filter add type=advert snr=[5,*]` | Adverts AND strong signal |

### `path`

`path` has its **own** syntax: repeater-ID prefixes separated by `>`, optionally
anchored with `^` (first entry) and/or `$` (last entry). It is not a regex.

| Command | What matches on a flood path |
|---|---|
| `filter add route=flood path=A1` | An entry starting `A1`, anywhere in the path |
| `filter add route=flood path=^A1` | First entry starts `A1` |
| `filter add route=flood path=A1$` | Last (most recent) entry starts `A1` |
| `filter add route=flood path=A1>B2` | `A1` immediately followed by `B2`, anywhere |
| `filter add route=flood path=^A1>B2$` | Exactly two entries: `A1`, then `B2` |
| `filter add route=flood path=^A1$` | Exactly one entry, starting `A1` |

For the path `10>A1>B2>30`: `A1>B2` matches, but `^A1>B2` and `A1>B2$` do not,
and `A1>30` never matches because the entries aren't adjacent. ID prefixes can
collide; add `hsize=3,4` if you need the full three bytes compared.

### `hsize`

| Command | What matches |
|---|---|
| `filter add hsize=1` | Packets using one-byte path IDs |
| `filter add hsize=1,2` | One-byte OR two-byte path IDs |

`hsize` is neither a hop count nor the channel tag size.

### `chan`

| Command | What matches |
|---|---|
| `filter add chan=Public` | Traffic verified with the stored Public key |
| `filter add chan=#noisy` | Traffic on `#noisy`; the channel is auto-added if needed |
| `filter add chan=#local,#weather` | Traffic on either named channel |
| `filter add chan=#test sender=^Bot$ text=^BEACON` | `#test` AND sender exactly `Bot` AND text starts with `BEACON` |

`chan` alone also matches group data; adding `sender` or `text` restricts the
rule to group text.

### `chanhash`

Matches the on-air channel tag without needing the key — handy for dropping a
channel whose key you don't have. Other channels with the same tag are caught
too.

| Command | What matches |
|---|---|
| `filter add chanhash=7A` | Group traffic carrying channel tag `7A` |
| `filter add type=txt chanhash=7A` | Group text AND tag `7A` |

Use exactly two hex digits, no `0x`, and only one tag per rule — use separate
rules for more.

### `region`

Region names must already be defined on the repeater (`region def ...`).
These examples assume `Foo` and `Bar` exist.

| Command | What matches |
|---|---|
| `filter add region=unscoped` | Plain flood traffic without a region scope |
| `filter add region=Foo` | Flood traffic arriving in `Foo` |
| `filter add region=Foo,Bar` | Flood traffic arriving in either region |
| `filter add chan=#local region=Foo,unscoped` | `#local` AND (region `Foo` OR unscoped) |

`region=*` means **unscoped only**, not "every region". Direct traffic never
matches any region condition.

### `sender`

Match the name without its colon. For `SpamBot: BEACON 123`, the sender is
`SpamBot`.

| Command | What matches |
|---|---|
| `filter add sender=SpamBot` | Name *contains* `SpamBot`, including `MySpamBot2` |
| `filter add sender=^SpamBot$` | Name is exactly `SpamBot` — not `SpamBot2`, not `spambot` |
| `filter add chan=#test sender=^Bot[0-9]+$` | `#test` AND names like `Bot1`, `Bot42` — not `Bot` |
| `filter add sender="^Test Bot$"` | Name exactly `Test Bot`, with the space |

Without `chan`, a sender rule applies to all group text the repeater can
decrypt.

### `text`

Match the message only. For `SpamBot: BEACON 123`, the text is `BEACON 123`.

| Command | What matches |
|---|---|
| `filter add text=BEACON` | Text contains `BEACON` anywhere |
| `filter add text=^BEACON` | Text starts with `BEACON` |
| `filter add text=^BEACON$` | Text is exactly `BEACON` — not `BEACON 123` |
| `filter add text="^RX in place"` | Text starts with that phrase, literal spaces |
| `filter add chan=#test text=^ID:\s*\d\d\d$` | `#test` AND `ID:` + optional space + exactly three digits |

The last pattern matches `ID:123` and `ID: 042`, but not `ID:12` or `ID:1234`.

## 10. Trying a rule before enforcing it (shadow mode)

Not sure a rule is right? Add it with `action=logonly`: matching packets are
**counted but still forwarded**. Watch the counters, then enforce.

```text
filter add chan=#test action=logonly
# ...later, check:
filter stats           # hit counter grows; packets still forwarded
# when satisfied, replace it with an enforcing rule:
filter del 0
filter add chan=#test
```

Two things to remember:

- `logonly` rules count hits but don't drop anything, and they don't stop the
  rate limiter (§6) from acting.
- Don't leave an overlapping `logonly` rule in place after adding the real
  drop rule — the earlier rule matches first and the drop never fires.

## 11. Managing the repeater remotely

Everything above works over the mesh. From a companion device that has the
repeater as a contact:

1. Log in with the admin password (once per session).
2. Send `filter ...` commands as CLI messages to the repeater contact.

Example with `meshcore_py` over a companion TCP port:

```python
await mc.commands.send_login_sync(rep, ADMIN_PASSWORD, min_timeout=30)
await mc.start_auto_message_fetching()
await mc.commands.send_cmd(rep, "filter add chan=#wardriving hops=[2,*]", dst_type=2)
await mc.commands.send_cmd(rep, "filter stats", dst_type=2)
```

This is the normal management path for a repeater on a tower — the serial port
is only needed for initial flashing and emergencies.

## 12. Limits and good-to-knows

| Limit | Value |
|---|---|
| Rules | 16 |
| Channels in the store | 16 (names up to 15 characters) |
| Sender pattern length | 23 characters |
| Text pattern length | 47 characters |
| Advert rate-limit window | 0–720 hours (0 = off) |
| CLI reply length | short (~160 bytes) — use `filter get <idx>` for detail |

- Overly long or complex patterns are **rejected with an error**, not silently
  shortened. Keep patterns short and specific.
- Rules, channels, and settings survive reboots. Counters and the advert cache
  do not — they start fresh after every reboot.
- Very complicated patterns can be slow to match. Prefer short, distinctive
  patterns like `^BEACON` over long wildcard chains. The `aborted` counter in
  `filter stats` grows if a pattern gives up mid-match; simplify it if you see
  that.

## 13. Writing sender/text patterns

`sender=` and `text=` use a small pattern language (**TinyRegex**) built into
the firmware. It is *not* JavaScript, Python, or PCRE regex — patterns copied
from those systems often mean something different here. This section is the
complete reference.

### Rules of thumb

1. Type the pattern as-is: `sender=^Bot$`, **not** `sender=/^Bot$/i`.
2. Matching is **case-sensitive** and searches anywhere unless anchored.
3. `^` at the start and `$` at the end make the match exact.
4. Use `.*` for "anything", not a shell-style `*`.
5. There are **no groups, OR operators, counted repeats, or flags**.

### Supported syntax

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

Tips:

- A quantifier applies only to the character right before it: `ab+` repeats
  the `b`, not `ab`.
- `[Bot]` means one of the characters `B`, `o`, `t` — not the word `Bot`.
- `\s` includes tabs and line breaks. If you want exactly one space, quote the
  pattern and type the space: `text="^RX OK$"`.
- To match punctuation like `.` `[` `\` literally, put a backslash in front.

### What doesn't work

These familiar features **do not exist** here — and a few are silently treated
as ordinary text rather than rejected:

| You might try | What actually happens | Do this instead |
|---|---|---|
| OR: `Alice\|Bob` | Matches the literal name `Alice\|Bob` | Two rules, one per name |
| Groups: `(ab)+` | Matches the literal text `(ab)+` | Write the sequence out, or separate rules |
| Counted repeat: `\d{3}` | Matches literal `{3}` | `\d\d\d` |
| Case-insensitive: `/i`, `(?i)` | Not supported; always case-sensitive | `^[Bb][Oo][Tt]$` |
| Word boundary: `\b` | Matches the literal letter `b` | See the word-boundary recipe below |
| Lookahead: `(?=...)` | Not supported | Two fragments → two rules, or `RX.*OK` for ordered text |
| `\n`, `\t` escapes | Match the letters `n`, `t` | `\s` for whitespace |
| Newlines / multiline mode | No per-line matching | Patterns apply to the whole field |

The command `filter add sender=^Alice|Bob$` therefore matches the literal
sender name `Alice|Bob`, **not** either name — and it's accepted without an
error. After copying a pattern from another tool, verify with
`filter get <idx>` and a test message.

### Recipe table

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

**OR between names:** use separate rules (repeat the channel too):

```text
filter add chan=#test sender=^BotA$
filter add chan=#test sender=^WeatherBot$
```

**Word boundaries:** there is no `\b`. To drop `BOT` as a standalone word
(separated by spaces, or the whole text), use these alternatives as needed:

```text
filter add text=^BOT$          # the whole text is BOT
filter add text="^BOT "        # BOT at the start, then a space
filter add text=" BOT$"        # a space, then BOT at the end
filter add text=" BOT "        # BOT between spaces
```

**Two required fragments in any order:** there is no lookahead. If order
matters, use `text=RX.*OK`. If either order is fine, add two otherwise
identical rules, one with `text=RX.*OK` and one with `text=OK.*RX`.

### Spaces, quotes, and sending from code

**At the repeater CLI:** double quotes just keep spaces inside one value;
backslashes pass through untouched. Don't double them.

```text
filter add text="^RX in place$"      # exactly one space at each gap
filter add text=^RX\s+in\s+place$    # one or more whitespace at each gap
filter add text=^v1\.2$              # literal dot
```

A literal double-quote character can't be passed through the CLI (quotes are
stripped). Use `.` as a stand-in character if needed — e.g. `text=^say.OK.$`
matches `say"OK"`, but also `say!OK!`.

**From Python:** use a raw string so backslashes reach the repeater intact:

```python
command = r'filter add text=^ID:\s*\d\d\d$'
```

**From JavaScript:** double the backslashes, or use `String.raw`:

```javascript
const command = 'filter add text=^ID:\\s*\\d\\d\\d$';
// or: const command = String.raw`filter add text=^ID:\s*\d\d\d$`;
```

If you go through a shell or JSON, those layers add their own escaping —
always confirm what the repeater actually stored with `filter get <idx>`.

### Unicode and emoji notes

- Literal text with emoji or accented letters works: `sender=^John😀$` is fine.
- Internally each emoji counts as several characters, so `.` won't match a
  whole emoji and emoji length eats into the pattern-length limits (§12).
  Exact literal matches are safest for non-ASCII names.
- There is no case-insensitive option. Use explicit choices like `[Bb]` — and
  note that accented letters can't be handled that way; match them literally.

### Troubleshooting

Work up from one narrow rule and confirm each piece before combining:

```text
filter add chan=#test sender=^Bot[0-9]+$ text=^BEACON action=logonly
filter on
filter list
filter get 0
filter stats
```

| Symptom | Check |
|---|---|
| Rule gets no hits | Is the filter on (`filter on`)? Is the rule enabled? Did an earlier rule match first? |
| Sender/text never matches | Can the repeater decrypt the channel? Is it group text? Are you matching the right field (sender without colon, text without name)? |
| Matches too broadly | Add `^`/`$` anchors; escape literal dots; remember `sender=Bot` matches `MyBot2` |
| Combined rule matches nothing | Every condition must be true — test each one alone; check `route=`/case |
| Pattern from an online tester misbehaves | Remove `/slashes/`, flags, groups, OR, `{counts}`, `\b` — see the table above |
| "Bad/long regex" error | Pattern too long (§12 limits) or broken syntax; shorten or simplify |
| `aborted` counter grows | Pattern too complex — simplify it |
| Drop rule never fires | An earlier overlapping rule (often `logonly`) is matching first |
