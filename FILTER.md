# Repeater Packet Filter — User Guide

A remote-configurable packet filter for the `simple_repeater` firmware. Rules can be
added, inspected, and removed over the repeater CLI (locally via serial, or remotely
over the mesh by an authenticated admin) — no rebuild or physical access required.

> Maintained on the [jhuebert fork](https://github.com/jhuebert/MeshCore) — prebuilt
> firmware is on the fork's Releases page (`filter-v*` tags).

**Quick navigation:** [Rule model and AND/OR semantics](#2-rule-model) ·
[Examples for every predicate](#examples-for-every-predicate-and-action) ·
[Use-case walkthroughs](#8-use-case-walkthroughs) ·
[TinyRegex reference](#11-tinyregex-syntax-examples-and-migration-guide)

**TinyRegex help:** [Supported syntax](#113-supported-syntax) ·
[Unsupported features](#114-unsupported-syntax-and-differences-from-other-engines) ·
[Recipes](#115-practical-recipes-and-migrations) ·
[Quoting and escaping](#116-cli-quoting-and-host-language-escaping) ·
[Limits](#118-limits-rejected-patterns-and-fail-open-behavior) ·
[Troubleshooting](#119-operator-checklist-and-troubleshooting)

---

## 1. Overview

The filter evaluates every packet offered to the repeater and decides whether to
**forward** it, **drop** it, or (in shadow mode) forward it while still counting the
match. The guiding model:

- A **rule** is a set of optional predicates (conditions) **ANDed together**.
  Every specified condition must match the same packet for that rule to act.
  **First match wins within each evaluation phase** (packet or content; see §2).
- An **unspecified predicate is a wildcard** (matches everything).
- If no rule matches, the packet is forwarded (default allow).
- Rules with content predicates are evaluated on the *decrypted* payload, so
  channel identity and sender/text patterns are cryptographically proven, not
  guessed from on-air bytes.

### How rules are evaluated

- **Packet-level rules** (type, route, region, hops, len, snr, path, hsize,
  chanhash) are decided before the packet is forwarded.
- **Content rules** (keyed `chan`, `sender`, `text`) are deferred: they are
  decided only after the group payload is decrypted and verified.

A rule that only sets packet-level predicates (e.g. `route=flood hops=[3,*]`)
applies to *all* packet types including adverts, telemetry-carrying packets, and
trace packets — use `type=` to narrow it when needed.

### Channel identity

- **Keyed channel** (`chan=#name`): matches only if the packet decrypts
  successfully with the stored key — the strongest identity proof. `#`-hashtag
  channels derive their key from the name, matching the companion apps, so a
  rule for `#someday` works with zero key distribution.
- **`chanhash=XX`**: matches only the 1-byte channel hash carried on-air. It is
  collision-prone and does **not** prove the repeater can read the channel —
  useful when you want to drop traffic on a channel whose key you don't have.

---

## 2. Rule model

### Combining conditions: AND within a rule, alternatives across rules

**Each `filter add` creates one rule. All predicate keys in that command are
ANDed together — not ORed.** Adding another condition narrows that rule; it does
not create a second independent reason to drop a packet. `action` selects what
happens on a match; it is not a matching condition.

```text
filter add chan=#test sender=^SpamBot$ text=^BEACON hops=[2,*] action=drop
```

This means: channel is `#test` **AND** sender is exactly `SpamBot` **AND** text
starts with `BEACON` **AND** this is flood traffic with at least two hops.
If even one condition is false, this rule does nothing.

| Channel | Sender | Text | Flood hops | This rule matches? |
|---|---|---|---|---|
| `#test` | `SpamBot` | `BEACON 123` | 2 | Yes |
| `#other` | `SpamBot` | `BEACON 123` | 2 | No: wrong channel |
| `#test` | `Alice` | `BEACON 123` | 2 | No: wrong sender |
| `#test` | `SpamBot` | `Hello` | 2 | No: wrong text |
| `#test` | `SpamBot` | `BEACON 123` | 1 | No: too few hops |

“Does not match” means **this rule** does not act; another rule or a normal
repeater forwarding restriction can still prevent forwarding.

| Combination | Logic | Example |
|---|---|---|
| Different predicate keys in one rule | **AND** | `type=txt hops=[2,*]` = group text **and** at least 2 flood hops |
| Comma-separated choices in a list-valued key | **OR within that key** | `chan=#local,#weather sender=^Bot$` = (`#local` **or** `#weather`) **and** sender `Bot` |
| Separate rules | Independent alternatives | One drop rule for `SpamBot`, another for `BEACON`: either can cause a drop |

Only `type`, `chan`, `hsize`, and `region` accept comma-separated alternatives.
The comma in `hops=[2,5]` separates interval endpoints, not alternatives.
`sender=Alice,Bob` searches for literal `Alice,Bob`; it is not a sender list.
`route=flood,direct` is invalid; omit `route` to include both.
Specify each key once; repeating a key does not create multiple independent
conditions (for example, a second `text=` replaces the first).

To drop messages from `SpamBot` **OR** messages starting with `BEACON` on `#test`,
use two rules, repeating the shared channel restriction:

```text
filter add chan=#test sender=^SpamBot$ action=drop
filter add chan=#test text=^BEACON action=drop
```

**Ordering and actions.** Rules are appended and have zero-based indices.
Each phase scans its applicable enabled rules in index order and stops at its
first match, including `logonly`. The packet phase skips content rules; the
content phase skips packet-only rules. There is no single first-match decision
across both phases. A `logonly` match prevents later rules in the **same phase**
from firing, but does not override a drop in the other phase, the advert limiter,
or normal forwarding restrictions. It is not an explicit “allow” action.
Put specific rules before broad rules within the same phase.

Omitted predicates are wildcards. A rule with only `action=drop` is therefore a
catch-all, not an empty rule. Do not add one unless that is your intention.

### Fields

Each rule holds:

| Field | Meaning |
|---|---|
| `enabled` | Disabled rules are skipped entirely |
| `action` | `drop` (default) or `logonly` |
| `type` | Payload-type mask: `advert`, `txt`, `data`, or any combination |
| `route` | `flood` or `direct`; omit to match either |
| `hops` | Flood path length (number of path hashes); direct traffic never matches |
| `len` | Payload length in bytes |
| `snr` | Received SNR in dB (signed, quarter-dB resolution) |
| `path` | Sliding-window match of 1–4 adjacent repeater pubkey-hash prefixes (`^`/`$` anchor first/last entry; both = whole path); add `route=flood` to restrict to flood history |
| `hsize` | Path hash size (1–4 bytes) used by the packet |
| `chan` | Keyed channel match (identity proven by decryption) |
| `chanhash` | Single 1-byte on-air channel hash |
| `region` | Comma list of flood regions (or `unscoped`); direct traffic never matches |
| `sender` | TinyRegex over the extracted sender name (without the colon), group text only |
| `text` | Regex over the message text, group text only |
| `hits` | Match counter (see §6) |

**Region identity.** Scoped flood packets arrive with a transport code that the
repeater matches against its region table (`region def ...`); plain flood packets
with no scope are **unscoped**. The `region` predicate is decided at packet time,
*before* decryption. Region names are resolved (and stored in canonical form)
when the rule is added, so a rule keeps working if the region table is rebuilt —
as long as the region is recreated under the same name. Direct-routed packets
have no region at all and therefore never match a `region=` predicate.

**Interval syntax** (used by `hops`, `len`, `snr`):

| Form | Meaning |
|---|---|
| `3` | exactly 3 |
| `[2,*]` | ≥ 2 (both endpoints inclusive) |
| `(2,5)` | > 2 and < 5 (both endpoints exclusive) |
| `[*,-4]` | ≤ −4 (useful for `snr`) |
| `*` | unbounded side |

**Regexes.** `sender` and `text` use **TinyRegex**, a reduced byte-oriented
regex engine, not JavaScript, Python, or PCRE. Patterns are case-sensitive and
unanchored by default: `TestBot` means *contains*; `^TestBot$` means *exactly*.
See [§11: TinyRegex syntax, examples, and migration guide](#11-tinyregex-syntax-examples-and-migration-guide)
before copying a pattern from another regex system. Unsupported constructs may
be treated as literal text rather than rejected.

Each regex evaluation has a step budget. If exhausted, that predicate does not
match and `aborted` increments; other rules can still match. This is fail-open
for that predicate, not an unconditional guarantee of forwarding.

**Limits** (per-rule / global capacities worth knowing when designing rules):

| Limit | Default |
|---|---|
| Rules | 16 |
| Keyed channels | 16 |
| Channel name storage | 16 bytes including NUL (use names up to 15 bytes) |
| Sender regex | 23 pattern bytes (24-byte storage including NUL) |
| Text regex | 47 pattern bytes (48-byte storage including NUL) |
| Compiled regex | 29 symbols plus the end sentinel (30 slots); see §11.8 |
| Path hashes per rule | 4 |
| Region list storage | 32 bytes including NUL and commas; canonical names must fit |
| Advert rate-limiter cache | 256 origins |

---

## 3. Keyed channels

Content rules match against a store of named channels and their keys:

- `Public` is pre-provisioned with the well-known Public key.
- `#name` channels are **derived** from the name, exactly as companion apps do.
  Add them with no key argument.
- Non-`#` (private) channels require an explicit base64 key (16 or 32 bytes).
- Referencing an unknown `#name` inside a rule (`filter add chan=#foo`) will
  auto-add it to the store.
- Deleting a channel keeps existing rules working (their references are
  updated; the deleted channel simply no longer matches).

**The store is not the rule list.** Rules reference store entries by index and
never own them: deleting a rule (or `filter clear`) leaves auto-added channels
in place, so a `#name` channel created by the shorthand can outlive the rule
that created it. That is usually harmless — a stored channel is also offered
for decryption while the filter is on — but the store holds a hard maximum of
16 entries no matter how few rules use them. When it is full, new `chan=#...`
references are rejected with `Err - chan store full` until you free a slot with
`filter chan del <name>`; candidates can be found by comparing `filter chan
list` against the `chan=` lines of `filter get <idx>`.

While the filter is enabled, group payloads on any stored channel are offered
for decryption — so a keyed channel rule doubles as an implicit "decrypt this
channel" registration, letting content rules fire on channels the repeater
otherwise wouldn't read. With the filter off the repeater behaves like stock
firmware.

---

## 4. Advert rate limiter

Independent of the rule list, flood-route adverts can be rate-limited **per
originating node**: *each node's advert is forwarded at most once every N hours.*

- `filter ratelimit advert <hours>` sets the window (0 = off, max 720 h ≈ 30 days).
- A repeated advert from a known origin inside the window is silently dropped
  and counted in `limiter`. After the window expires the origin is re-admitted
  and the window restarts.
- The limiter only runs on flood-routed adverts, and only when the rule list
  has not already dropped the packet. Logonly adverts are also subject to it.
- The cache holds up to 256 recent origins and is RAM-only (cleared on reboot).
- The cache can be emptied without changing the window (`filter ratelimit
  clear`), e.g. after a repeater has been offline for a while.

Use case: on a well-connected repeater, stop re-flooding everyone's periodic
adverts while still forwarding each node's advert once per window so it stays
reachable through you.

---

## 5. Persistence

All rules, channels, the enabled flag, and the ratelimit window are saved on the
repeater's filesystem and survive reboots. **Counters and the advert cache do
not** — they are memory-only and start at zero after every reboot (a deliberate
fail-open for the advert cache).

---

## 6. Counters

`filter stats` shows `hits` per rule plus the limiter and regex-abort counters.
All counters are RAM-only: they start at zero after every reboot, and `filter
clear` resets them with the rules. Use `logonly` rules as long-term probes to
measure what a `drop` *would* remove before enforcing it.

---

## 7. CLI reference

All commands begin with `filter`. Replies are short (the remote CLI reply buffer is
160 bytes). Works identically over serial and remote-admin (see §9).

| Command | Effect |
|---|---|
| `filter` | Status line: on/off, rule count, channel count, ratelimit, cache fill, counters |
| `filter on` / `filter off` | Enable/disable the whole filter (config is kept) |
| `filter add <key>=<val> ...` | Add a rule (space-separated predicates; see below) |
| `filter list` | Compact one-line digest of all rules |
| `filter get <idx>` | Full detail of one rule, including its `hits` |
| `filter enable <idx>` / `filter disable <idx>` | Toggle a single rule |
| `filter del <idx>` | Delete a rule (later rules shift down one index) |
| `filter clear` | Delete all rules (channels and ratelimit are kept) |
| `filter chan` / `filter chan list` | List the keyed channel store |
| `filter chan add <name> [<psk-b64>]` | Add a channel; key optional for `#` names |
| `filter chan del <name>` | Remove a channel (existing rules are updated) |
| `filter ratelimit` | Show the advert ratelimit window and cache fill |
| `filter ratelimit advert <hours>` | Set the window (0–720 h; 0 = off) |
| `filter ratelimit clear` | Empty the advert cache (all origins re-recorded) |
| `filter stats` | Per-rule hit counters + limiter/abort counters |
| anything else | Usage line: `on\|off\|add\|list\|get\|enable\|disable\|del\|clear\|chan\|ratelimit\|stats` |

### `filter add` predicate keys

Space-separated `key=value` tokens are **ANDed within this rule** (see §2).
List-valued keys accept comma-separated alternatives (`type=advert,txt`,
`chan=#a,#b`, `hsize=1,2`, `region=Foo,Bar`). Values containing spaces must be
wrapped in double quotes: `text="^RX in place"`. Alternatively,
`text=^RX\sin\splace` accepts any single whitespace byte at each `\s`, not just
a literal space. Quotes group the CLI token; they are not regex delimiters.
See §11.6 for backslashes, quotes, and commands sent from JavaScript or Python.

Examples in this guide are commands for the **repeater CLI**, not shell commands.
Lines beginning with `#` and trailing `# ...` annotations are explanatory comments;
do not send those comments to the repeater. Most examples are alternatives, not
a script to install in full. Test selected rules with `action=logonly`, inspect
the returned index with `filter get <idx>`, and use `filter stats` before enforcing.
The whole filter must be enabled with `filter on` for rules to run.

| Key | Values | Matches |
|---|---|---|
| `type` | `advert` `txt` `data` (comma-combine) | Payload type(s) |
| `route` | `flood` \| `direct` | Routing class of the packet |
| `hops` | interval | Flood hop count (see interval syntax, §2) |
| `len` | interval | Payload length, bytes |
| `snr` | interval, signed dB | Packet SNR (e.g. `snr=[-100,-5]`, `snr=-2.25`) |
| `path` | `[^]HEX>HEX>…[$]` | Adjacent path hash prefixes; `^` anchors first, `$` anchors last, both together require the whole path (e.g. `path=^10$` = one entry only); 2, 4, 6, or 8 hex chars per entry, up to 4 entries. Use `route=flood` for flood history |
| `hsize` | `1..4` (comma-combine) | Path hash size the packet carries |
| `chan` | channel name(s) | Keyed store match, decryption-proven; `#` names auto-add |
| `chanhash` | 2 hex chars | Bare 1-byte on-air channel hash (no decryption proof) |
| `region` | region name(s), `unscoped` (comma-combine) | Flood region the packet arrived in (`unscoped` = plain flood with no scope); direct traffic never matches |
| `sender` | regex | Sender name in `"<sender>: <text>"` (group text only) |
| `text` | regex | Message text (group text only) |
| `action` | `drop` \| `logonly` | Default `drop` |

Rules containing `chan`, `sender`, or `text` are content rules (evaluated after
decryption). All other combinations are packet-level.

Invalid input is rejected with `Err - ...` and no half-added rule is left behind.

### Examples for every predicate and action

The tables below give complete, independent commands. A matching `logonly`
rule increments its hit counter without itself dropping the packet. Unless a
row says otherwise, fields not present in the command are unrestricted.

#### `type`: payload category

`txt` means **group text**, not private-message text. `data` means **group data**,
not every non-text packet. Omitting `type` includes all payload types, which can
affect adverts, requests, telemetry-carrying packets, and traces.

| Command | What matches |
|---|---|
| `filter add type=advert action=logonly` | Adverts, regardless of route |
| `filter add type=txt action=logonly` | Group text on any channel, without needing its key |
| `filter add type=data route=flood action=logonly` | Group data AND flood route |
| `filter add type=txt,data snr=[*,-8.5] action=logonly` | (Group text OR group data) AND SNR ≤ −8.5 dB |

An advert does not match `type=txt,data`. To include all packet types, omit
`type` rather than listing only these three categories.

#### `route`: flood versus direct routing

| Command | What matches |
|---|---|
| `filter add route=flood action=logonly` | Flood traffic, scoped or unscoped |
| `filter add route=direct action=logonly` | Direct-routed traffic |
| `filter add route=flood len=[180,*] action=logonly` | Flood traffic AND payload length ≥ 180 bytes |

`route=direct` describes routing, not a synonym for a private text message.
Omit `route` to include both classes; `route=flood,direct` is invalid. Combining
`route=direct` with `hops` or `region` produces a rule that never matches,
because those conditions require flood traffic.

#### `hops`: flood hop count

Hops count the path hash entries already in the incoming flood packet, not the
number of bytes in the path. A two-hop packet is still two hops when `hsize=3`.
Direct-routed packets never match a `hops` predicate, even an unbounded interval.

| Command | What matches |
|---|---|
| `filter add hops=0 action=logonly` | Originating flood traffic with no path entries |
| `filter add hops=3 action=logonly` | Exactly 3 flood hops |
| `filter add chan=#wardriving hops=[2,*] action=logonly` | Proven `#wardriving` channel AND at least 2 flood hops |
| `filter add hops=(2,5) action=logonly` | 3 or 4 flood hops; neither 2 nor 5 |
| `filter add type=advert hops=[1,3) action=logonly` | Adverts AND 1 or 2 flood hops |

Use `[2,5]` to include both endpoints; `(2,5]` excludes only 2. Leave out
`hops` entirely if direct traffic should remain eligible.

#### `len`: packet payload bytes

This is the packet's payload length, **not the displayed message's character
count**, decrypted text length, or entire radio frame size. Headers and path
bytes outside the payload are not counted; protocol/encryption overhead inside
the payload is counted. UTF-8 characters can occupy multiple bytes.

| Command | What matches |
|---|---|
| `filter add len=100 action=logonly` | Exactly 100 payload bytes |
| `filter add len=[180,*] action=logonly` | At least 180 payload bytes |
| `filter add type=txt len=[*,80] action=logonly` | Group text AND at most 80 payload bytes |
| `filter add route=flood len=(80,180] action=logonly` | Flood payloads of 81–180 bytes |

`len=[180,*]` excludes 179 but includes 180. Use regex structure, not `len`, to
recognize the format of a message's visible text.

#### `snr`: received signal-to-noise ratio

SNR is for reception at **this repeater**, in signed dB, not RSSI and not the
originator's reported signal measurement. Values have quarter-dB resolution;
use `.00`, `.25`, `.50`, or `.75` when specifying fractional thresholds.

| Command | What matches |
|---|---|
| `filter add snr=-2.25 action=logonly` | Exactly −2.25 dB |
| `filter add snr=[*,-8.5] action=logonly` | SNR ≤ −8.5 dB |
| `filter add snr=(-8.5,0] action=logonly` | SNR > −8.5 and ≤ 0 dB |
| `filter add type=advert snr=[5,*] action=logonly` | Adverts AND SNR ≥ 5 dB |

At a −8.5 dB inclusive cutoff, −8.75 matches and −8.25 does not. Probe first:
a weak link may be a useful route, not unwanted traffic.

#### `path`: adjacent repeater hash prefixes

`path` has its **own syntax**, not TinyRegex: hexadecimal prefixes separated by
`>`, optionally anchored with `^` and/or `$`. Each prefix is 2, 4, 6, or 8 hex
digits (1–4 bytes); at most four adjacent entries can be specified. There are
no regex dots, classes, quantifiers, or “skip any hops” operators here.
Use `route=flood` explicitly when identifying historical flood relays; a direct
route's path has different meaning and `path` alone does not exclude it.

| Command | What matches on a flood path |
|---|---|
| `filter add route=flood path=A1 action=logonly` | An entry beginning `A1` anywhere |
| `filter add route=flood path=^A1 action=logonly` | First entry begins `A1` |
| `filter add route=flood path=A1$ action=logonly` | Last entry begins `A1` (most recent recorded relay) |
| `filter add route=flood path=A1>B2 action=logonly` | `A1` immediately followed by `B2`, anywhere |
| `filter add route=flood path=^A1>B2 action=logonly` | First two entries are `A1`, then `B2`; more may follow |
| `filter add route=flood path=^A1>B2$ action=logonly` | Exactly two entries: `A1`, then `B2` |
| `filter add route=flood path=^A1$ action=logonly` | Exactly one entry, beginning `A1` |

For path `10>A1>B2>30`, `A1>B2` matches, but `^A1>B2` and `A1>B2$` do not.
`A1>30` does not match because the entries are not adjacent. A zero-hop packet
never matches a path predicate.

**Prefix precision depends on the packet.** Each entry compares only the smaller
of the configured prefix length and the packet's hash size. Thus `path=A1B2C3`
can match a one-byte path entry `A1`; it does not require three-byte hashes.
To require all three bytes, combine `path=A1B2C3 hsize=3,4`. Hash prefixes can
collide and are not cryptographic proof of a repeater's identity.

#### `hsize`: bytes per path hash

| Command | What matches |
|---|---|
| `filter add hsize=1 action=logonly` | Packets using one-byte path hashes |
| `filter add hsize=1,2 action=logonly` | One-byte OR two-byte path hashes |
| `filter add route=flood path=A1B2C3 hsize=3,4 action=logonly` | Flood route AND matching three-byte prefix AND hash size 3 or 4 |

`hsize` is neither a hop count nor the channel hash size. `hsize=1,2` excludes a
three-byte-hash packet regardless of how many hops it has travelled.

#### `chan`: decryption-proven channel

| Command | What matches |
|---|---|
| `filter add chan=Public action=logonly` | Group text or group data verified with the stored Public key |
| `filter add chan=#noisy action=drop` | Verified traffic on `#noisy`; auto-adds its derived key if needed |
| `filter add chan=#local,#weather action=logonly` | Verified traffic on either named channel |
| `filter add chan=#test sender=^Bot$ text=^BEACON action=logonly` | `#test` AND sender exactly `Bot` AND text starts with `BEACON` |

A packet with only a coincidentally equal on-air hash does not satisfy `chan`.
`chan` alone can match group data; adding `sender` or `text` restricts the rule
to group text. For a private channel, first register its real key using
`filter chan add <name> <psk-b64>` (§3), then reference that name. Channel names
are literal store names, not regexes: `chan=#test.*` does not mean all test channels.

#### `chanhash`: key-less on-air channel hash

| Command | What matches |
|---|---|
| `filter add chanhash=7A action=logonly` | Group text/data carrying on-air channel hash `7A`, even without a key |
| `filter add type=txt chanhash=7A action=logonly` | Group text AND hash `7A`; not group data |
| `filter add route=flood chanhash=7A hops=[3,*] action=logonly` | Flood group traffic AND hash `7A` AND at least 3 hops |

`7A` is an example, not a particular known channel. Supply exactly two hex
digits, without `0x`. Only one hash is accepted per rule; use separate rules
for several hashes. Colliding channels are also affected. Combining `chan` and
`chanhash` requires **both**, not a fallback to the hash if decryption fails.

#### `region`: incoming flood scope

Except for `unscoped`, names must already resolve in the repeater's region table
(see `region def ...`). These examples assume `Foo` and `Bar` exist. Prefer full
canonical names to avoid selecting the wrong region through a short prefix.

| Command | What matches |
|---|---|
| `filter add region=unscoped action=logonly` | Plain flood traffic without scope |
| `filter add region=Foo action=logonly` | Flood traffic arriving in `Foo` |
| `filter add region=Foo,Bar action=logonly` | Flood traffic arriving in either `Foo` or `Bar` |
| `filter add chan=#local region=Foo,unscoped action=logonly` | Proven `#local` AND (region `Foo` OR unscoped) |
| `filter add type=advert region=Bar hops=[2,*] action=logonly` | Adverts AND region `Bar` AND at least 2 flood hops |

Direct traffic never matches. An unknown scoped transport code is not the same
as unscoped traffic. `region=*` is an alias for **unscoped only**, not “every
region”; omit the predicate for no region restriction. Region is not a channel:
`region=Foo chan=#local` requires both independently.

#### `sender`: extracted group-text sender

Match the name **without** its colon or message text. For `SpamBot: BEACON 123`,
the sender subject is `SpamBot`. This is a name in decrypted content, not a
public-key identity or an advert name. See §11 for full syntax and parsing details.

| Command | What matches |
|---|---|
| `filter add sender=SpamBot action=logonly` | Sender contains `SpamBot`, including `MySpamBot2` |
| `filter add sender=^SpamBot$ action=logonly` | Sender is exactly `SpamBot`; not `SpamBot2` or `spambot` |
| `filter add chan=#test sender=^Bot[0-9]+$ action=logonly` | `#test` AND names such as `Bot1` or `Bot42`; not `Bot` |
| `filter add sender="^Test Bot$" action=logonly` | Sender exactly `Test Bot`, including one literal space |

Without `chan`, a sender rule applies to all group text the repeater can decrypt,
not to encrypted traffic it cannot read. It never matches binary group data.

#### `text`: extracted group-text message

For `SpamBot: BEACON 123`, the text subject is `BEACON 123`, not the full
`SpamBot: ...` string. `sender` AND `text` is the way to constrain both fields.

| Command | What matches |
|---|---|
| `filter add text=BEACON action=logonly` | Text contains uppercase `BEACON` anywhere |
| `filter add chan=Public text=^BEACON action=logonly` | Public AND text starts with `BEACON` |
| `filter add text=^BEACON$ action=logonly` | Text is exactly `BEACON`; not `BEACON 123` |
| `filter add text="^RX in place" action=logonly` | Text starts with that phrase with literal spaces |
| `filter add chan=#test text=^ID:\s*\d\d\d$ action=logonly` | `#test` AND `ID:` followed by optional whitespace and exactly three ASCII digits |

The last pattern matches `ID:123` and `ID: 042`, but not `ID:12` or `ID:1234`.
There is no comma-list syntax for `text`; see §11.5 for alternatives and rewrites.

#### `action`, `enabled`, and `hits`: control and observation

These are not packet predicates. New rules are enabled and default to `drop`;
`enabled` and `hits` are not `filter add` keys.

| Command | Effect |
|---|---|
| `filter add chan=#test text=^BEACON action=logonly` | Count matching beacons without this rule dropping them |
| `filter add chan=#test text=^BEACON action=drop` | Drop matching beacons |
| `filter add chan=#test text=^BEACON` | Same drop action, by default |
| `filter disable 0` | Keep rule 0 but skip it (no new hits) |
| `filter enable 0` | Re-enable rule 0 |
| `filter get 0` | Inspect rule 0, including its hit count |
| `filter stats` | Inspect rule/limiter/regex-abort counters |

Use the actual index returned by `filter add`, not necessarily 0. To change an
action, delete and re-add the rule; deletion shifts later indices and re-adding
appends it, so re-check ordering. Do not leave an earlier overlapping `logonly`
rule in the same phase and expect a later `drop` rule to take effect.

---

## 8. Use-case walkthroughs

### 8.1 Silencing one channel on a shared repeater

The mesh around you has a `#memes` channel generating constant relayed traffic you
don't want your repeater to spend airtime on — but you still want everything else,
including other hashtag channels, forwarded.

```text
filter add chan=#memes action=drop
filter on
filter stats          # watch hits climb as #memes traffic arrives
```

Because the key for `#memes` is derived from its name, no key exchange is needed.
The drop happens before the repeater ever spends a transmitter slot relaying it.

### 8.2 Anti-wardriving (hop-count gating)

You operate a public repeater and want to carry local `#wardriving` traffic but
refuse to be a free relay for it across the wider mesh: anything that has already
hopped at least twice shouldn't go further.

```text
filter add chan=#wardriving hops=[2,*] action=drop
filter on
```

Two hops or more: dropped. Zero or one hop (originators and first relays): passes.

### 8.3 Bot sender blocklist (content rules)

A bot floods several channels you *can* decrypt. Match on the sender inside the
decrypted payload, scoped to the channels where it's a problem:

```text
filter chan add #local
filter chan add #weather
filter add chan=#local,#weather sender=^BotName$ action=drop

# substring match instead of exact:
filter add chan=#local,#weather sender=BotName action=drop

# predicates combine (AND): match the sender AND the message text
filter add chan=#local sender=^BotName$ text="^RX in place" action=drop
```

### 8.4 Shadow mode before enforcement

Before actually dropping `#test`, observe what would be caught:

```text
filter add chan=#test action=logonly
# ...later, check telemetry:
filter stats           # rule hit counter grows; packets still forwarded
# when satisfied, switch to enforcing:
filter del 0
filter add chan=#test action=drop
```

### 8.5 Cutting advert noise

Your repeater relays the same nodes' adverts every few minutes all day. Cap the
relay:

```text
filter ratelimit advert 48        # each origin forwarded at most every 48 h
filter ratelimit clear            # optional: forget history, start clean
filter ratelimit                  # ratelimit advert 48h; cache 0/256
```

### 8.6 Isolating one upstream repeater

One repeater (pubkey-hash prefix `A1B2C3`) keeps injecting junk into
flood traffic. Drop anything whose recorded flood path *starts* with it:

```text
filter add route=flood path=^A1B2C3 action=drop
```

Or only adverts it relayed:

```text
filter add type=advert route=flood path=^A1B2C3 action=drop
```

The first entry is the earliest recorded relay, not necessarily your immediate
upstream neighbour. To match the most recent recorded relay, use
`path=A1B2C3$` instead. Add `hsize=3,4` if all three prefix bytes must be present
in the packet; shorter hashes otherwise compare only the bytes available.

### 8.7 Temporary experiment without losing the production config

```text
filter add len=[200,*] action=logonly     # probe oversized payloads
# ...decide you don't need it:
filter del 3
```

Disabled vs deleted: `filter disable 2` keeps the rule for later
(`filter enable 2`), useful for A/B testing a predicate.

### 8.8 Keeping out-of-region traffic off a channel

Not all flood regions are appropriate for all channels. To forbid unscoped
(plain, scope-less) flood traffic on a hashtag channel:

```text
filter add chan=#zperA region=unscoped action=drop
```

Or drop traffic scoped to several regions everywhere, on any channel:

```text
filter add region=Foo,Bar action=drop
```

Region names resolve by prefix at add time (like `region allow`), so
`region=fo` stores the canonical name `Foo`. A combined rule shares one `hits`
counter — for per-region usage stats, add one `logonly` rule per region
instead. Direct-routed packets have no region and are never affected.

---

## 9. Remote administration

Everything above can be done over the mesh. From a companion device that has the
repeater as a contact:

1. Log in with the admin password (one-time per session) — the login packet's
   timestamp also keeps the repeater's clock honest.
2. Issue `filter ...` as CLI commands to the repeater contact.

Example with `meshcore_py` over a companion TCP port:

```python
await mc.commands.send_login_sync(rep, ADMIN_PASSWORD, min_timeout=30)
await mc.start_auto_message_fetching()
await mc.commands.send_cmd(rep, "filter add chan=#wardriving hops=[2,*] action=drop", dst_type=2)
await mc.commands.send_cmd(rep, "filter stats", dst_type=2)
```

This is the normal management path for a repeater deployed on a tower: the serial
port is only needed for initial flashing and emergencies.

---

## 10. Behaviour notes and limits

- **AND within a rule; first match wins within each phase** — see §2 for list alternatives and packet/content ordering.
- Content rules can only match traffic the repeater can actually decrypt; a keyed
  channel rule doubles as an implicit "decrypt this channel" registration.
- `sender`/`text` predicates apply to group text payloads only; a matching rule
  with these predicates never fires on binary group data.
- `chanhash` is a convenience for key-less cases; prefer keyed `chan` matching.
- Regex evaluations are step-budgeted and fail open for that predicate (see §11.8).
  Watch `aborted` for expensive patterns. Storage limits are 23 sender / 47 text
  pattern **bytes**, plus the separate 29-symbol compile limit and class-buffer
  limit. Over-limit patterns are rejected, not silently truncated.
- All counters (`hits`, `limiter`, `aborted`) and the advert cache are memory-only:
  they reset on reboot, and the cache can also be emptied via `filter ratelimit clear`.
- Reply strings are capped at 160 bytes (remote CLI buffer); `filter list` shows a
  short digest per rule and `filter get <idx>` the full detail.


---

## 11. TinyRegex syntax, examples, and migration guide

### 11.1 Which regex language is this?

`sender=` and `text=` use the firmware's small, vendored **TinyRegex** engine,
derived from [kokke/tiny-regex-c](https://github.com/kokke/tiny-regex-c) with local
changes, including a matching step budget and UTF-8 literal-byte fixes. The
implementation in [TinyRegex.cpp](examples/simple_repeater/TinyRegex.cpp) and
[TinyRegex.h](examples/simple_repeater/TinyRegex.h), not a desktop regex tester,
is the reference for this firmware. Filter parsing and CLI handling are in
[PacketFilter.cpp](examples/simple_repeater/PacketFilter.cpp).

It is a **search/match language**, not a replacement language. There are no
capture groups to extract, replacement strings, or global replacement flags.
Only `sender` and `text` use it: numeric intervals, path chains, channel names,
and region lists are different formats.

Start with these rules of thumb:

1. Type the pattern itself: `sender=^Bot$`, **not** `sender=/^Bot$/i`.
2. Matching is **case-sensitive** and searches anywhere unless anchored.
3. Use `^` at the beginning and `$` at the end for an exact match.
4. Use `.*` for arbitrary bytes, not shell-style `*` by itself.
5. Use `\d`, `\w`, and `\s` for common byte classes; `\n` does **not** mean newline.
6. There are **no groups, alternation, counted repetition, lookarounds, or flags**.
7. An accepted rule is not proof that a copied JavaScript/Python regex means the
   same thing here. Unsupported syntax can be accepted as ordinary characters.

### 11.2 What string does the regex see?

After decryption, the group-text parser skips the timestamp and text-type byte,
then splits the remaining content at its **first colon**:

```text
On-message content:  WeatherBot:   TEMP=21.5
sender subject:      WeatherBot
text subject:        TEMP=21.5
```

- The sender excludes the colon. Trailing spaces/tabs before the colon are
  removed; leading sender whitespace is not removed.
- Leading spaces, tabs, CR, and LF after the colon are removed from the text.
  Trailing text whitespace is retained. Thus `text=^OK$` does not match `OK `.
- With no colon, the sender is empty and the whole remainder becomes text.
- The sender subject is bounded to 63 bytes, independently of the 23-byte
  sender **pattern** limit. Text is bounded by the packet payload buffer.
- Matching uses NUL-terminated byte strings: an embedded NUL ends the subject
  seen by TinyRegex. `^` and `$` refer to these parsed subjects, not the raw
  encrypted payload, entire packet, or individual lines.
- Only group text is eligible for `sender`/`text`. Binary group data, adverts,
  private messages, and channels the repeater cannot decrypt do not match them.

For `WeatherBot: TEMP=21.5`, use `sender=^WeatherBot$ text=^TEMP=` to constrain
both subjects. `text=^WeatherBot:` looks in the wrong field. Decryption verifies
the channel key, not the claimed sender name as a unique person or public key.

### 11.3 Supported syntax

In this table, patterns are shown as sent to the repeater, with **one actual
backslash** for each regex escape. “Byte” matters for UTF-8; see §11.7.

| Syntax | Meaning | Pattern example | Matches / does not match |
|---|---|---|---|
| Ordinary characters | Literal, case-sensitive sequence | `Bot` | Matches `MyBot2`; not `bot` |
| `^` at pattern start | Beginning of subject | `^Bot` | Matches `Bot2`; not `MyBot` |
| `$` at pattern end | End of subject | `Bot$` | Matches `MyBot`; not `Bot2` |
| `^...$` | Whole subject | `^Bot$` | Matches only `Bot` |
| `.` | One byte, excluding CR/LF by default | `^B.t$` | Matches `Bot`, `B7t`; not `Bt` |
| `*` after an atom | Zero or more repetitions, greedy | `^ab*c$` | Matches `ac`, `abc`, `abbbc`; not `abb` |
| `+` after an atom | One or more repetitions, greedy | `^Bot\d+$` | Matches `Bot1`, `Bot42`; not `Bot` |
| `?` after an atom | Zero or one repetition, tries zero first | `^colou?r$` | Matches `color`, `colour`; not `colouur` |
| `[abc]` | One byte from a set | `^Bot[ABC]$` | Matches `BotA`, `BotB`, `BotC`; not `BotAB` |
| `[a-z]` | One byte in an inclusive range | `^[A-Z][0-9]$` | Matches `A7`; not `a7` or `A77` |
| `[^abc]` | One byte not in the set | `^[^0-9]+$` | Matches `abc`; not `abc2` or empty text |
| `\d` / `\D` | Digit / non-digit | `^\d\d$` | Matches `42`; not `4` or `4x` |
| `\w` / `\W` | Word byte / non-word byte | `^\w+$` | Matches `Bot_42`; not `Bot-42` |
| `\s` / `\S` | Whitespace / non-whitespace byte | `^RX\s+OK$` | Matches `RX OK` and `RX  OK`; not `RXOK` |
| Escaped punctuation | Literal punctuation | `^v1\.2$` | Matches `v1.2`; not `v1x2` |
| `\\` | Literal backslash | `^a\\b$` | Matches the three-byte text `a\b` |

An **atom** is one literal byte, dot, character class, or shorthand class.
Quantifiers apply to the preceding atom only: `ab+` repeats `b`, not `ab`.
Never start a pattern with a quantifier or stack quantifiers (`**`, `*?`, `++`,
etc.); these are not supported constructs, even if the compiler accepts them.
Use anchors only in the positions described above. Escape them (`\^`, `\$`)
when you want literal punctuation.

**Whitespace is not just a space.** `\s` uses C's whitespace classification
(space, tab, LF, CR, form feed, vertical tab in the usual C locale). For exactly
one space, quote the pattern and type a space: `text="^RX OK$"`.
`\d` and `\w` use C byte classification; in the firmware's usual C locale,
think `[0-9]` and `[A-Za-z0-9_]`, not Unicode digits or letters. Use explicit
ASCII ranges when that intent should be unambiguous.

**Character-class details:**

- `[A-Za-z0-9_]` combines ranges and literals; `[\dA-F]` combines a shorthand
  and a range. A class always consumes **one byte**, never an entire word.
- `[Bot]` means one of `B`, `o`, or `t`, not the word `Bot`.
- `[^0-9]` means one non-digit byte, not “the entire message has no digits”.
  For the latter use `^[^0-9]+$` (nonempty subjects).
- Place a literal hyphen first or last (`[-A-Z]`, `[A-Z-]`), or escape it.
- Escape a literal closing bracket as `\]` inside a class, and a literal
  backslash as `\\`. Use `\[` outside a class for a literal opening bracket.
- Dot and quantifiers inside a class are literals: `[.*+]` selects one of those
  three punctuation bytes. A leading `^` negates a class; it is not a start anchor.
- Use well-formed, nonempty classes and ascending ASCII ranges. Do not assume
  POSIX classes, nested sets, class subtraction/intersection, or Unicode ranges work.

**Greediness:** `*` and `+` try the longest run first and backtrack if needed.
`?` tries zero occurrences before one, unlike the normally greedy `?` in
JavaScript/Python. There is no lazy `*?`/`+?` syntax. The filter uses only whether
there was a match, not its captured text or length.

### 11.4 Unsupported syntax and differences from other engines

JavaScript RegExp, Python `re`, PCRE/Perl, and POSIX grep/sed are **different
regex dialects**. Even those systems do not all support the same features.
Do not select one of them in an online tester and assume it models TinyRegex.

| Familiar feature | TinyRegex behavior | What to use instead |
|---|---|---|
| JavaScript `/pattern/i`, Python `re.I`, PCRE `(?i)` | No delimiters or flags; case-sensitive only | Bare pattern; spell case choices with `[Bb]` or separate rules |
| Alternation `foo\|bar` | No OR operator; the pipe is literal | Separate rules with the same shared predicates |
| Capturing `(ab)` or noncapturing `(?:ab)` groups | No grouping; parentheses are ordinary characters | Write the literal sequence or use separate rules |
| Counted repetition `a{3}`, `a{2,4}` | Braces/counts are ordinary text, not repetition | `aaa`, or a short sequence of optional atoms |
| Lazy `.*?`, `.+?`, `a??`; possessive `a++` | Not supported; do not rely on acceptance | Use one supported quantifier or a delimiter-excluding class |
| Word boundaries `\b`, `\B` | Match literal `b` and `B`, not boundaries | Explicit start/end or delimiter patterns; see §11.5 |
| Lookahead/lookbehind, including negative lookahead | No lookarounds | Restructure positive rules; no general equivalent |
| Backreferences `\1`, named references | No captures/references; `\1` matches literal `1` | No general equivalent |
| `\n`, `\r`, `\t`, `\f`, `\v` escapes in regex text | Match the letters `n`, `r`, `t`, `f`, `v` | `\s` if any whitespace is acceptable |
| Hex/Unicode/octal escapes `\x41`, `\u0041`, `\101` | Not decoded as character codes | Type the literal character (`A`) |
| Unicode properties `\p{L}`, Unicode case folding | Not supported; byte-oriented engine | Literal UTF-8 text or explicit ASCII sets |
| Python/PCRE `\A`, `\Z`, `\z` | No special anchor meaning; escaped letters are literals | Leading `^` and trailing `$` |
| Multiline mode (`m` / `re.M`) | No per-line anchors | Match the parsed subject's start/end only |
| Dot-all (`s` / `re.S`) | No runtime flag; dot excludes CR/LF by default | `[\s\S]` for any non-NUL byte, including CR/LF |
| POSIX `[[:digit:]]`, set operations, atomic groups | Not supported | `[0-9]` and simple classes |
| Regex replacement syntax `$1`, `\g<name>` | No replacement operation | Not applicable to filter rules |

For clarity, an attempted alternation actually looks like this at the CLI:

```text
filter add sender=^Alice|Bob$ action=logonly
```

That pattern matches the literal full sender name `Alice|Bob`, **not** either
`Alice` or `Bob`. Likewise, `^Bot[0-9]{2}$` looks for one digit followed by the
literal text `{2}`, and `^(Bot)$` looks for the literal name `(Bot)`.
These can be accepted without an error while doing something you did not intend.

The compiler rejects conditions such as an unterminated class, a trailing
backslash, or exhausted compile storage. It is **not a comprehensive validator
for every unsupported regex dialect**. Avoid unsupported syntax even when
`filter add` returns success.

Also note: TinyRegex's trailing `$` requires the actual end of the subject; it
does not have the before-final-newline allowance common in Python/PCRE. Its
unanchored search does not report a match starting at the terminating NUL, so
avoid bare `$` as an “anything” pattern. Use meaningful anchored patterns.

### 11.5 Practical recipes and migrations

Patterns below are values for `sender` or `text`, without surrounding slashes.
Quote the whole value in the CLI if it contains literal spaces.

| Goal | TinyRegex pattern | Examples |
|---|---|---|
| Contains `BEACON` | `BEACON` | `new BEACON 7` matches; `beacon` does not |
| Starts with `BEACON` | `^BEACON` | `BEACON 7` matches; `new BEACON` does not |
| Ends with `BEACON` | `BEACON$` | `new BEACON` matches; `BEACON 7` does not |
| Exactly `BEACON` | `^BEACON$` | No prefixes, suffixes, or trailing whitespace |
| Case choices for `bot` | `^[Bb][Oo][Tt]$` | `bot`, `BOT`, `bOt`; no case-insensitive flag needed |
| Exactly three digits, instead of `\d{3}` | `^\d\d\d$` | `007` matches; `07` and `0007` do not |
| Two to four digits, instead of `\d{2,4}` | `^\d\d\d?\d?$` | `12`, `123`, `1234`; not `1` or `12345` |
| At least three digits | `^\d\d\d+$` | Three required digits, then any more |
| Optional sign and decimal fraction | `^-?\d+\.?\d*$` | `-12`, `12.5`, and also `12.`; not a strict number validator |
| Literal decimal format, one fractional digit | `^-?\d+\.\d$` | `-12.5` and `0.0`; not `12` or `12.55` |
| Optional one-byte suffix | `^BotA?$` | `Bot` or `BotA`, not `BotAA` |
| Literal brackets | `^\[TEST\]$` | `[TEST]`, not `TEST` |
| Literal dot/plus | `^v1\.2\+$` | `v1.2+`, not `v1x2+` |
| Nonempty ASCII identifier | `^[A-Za-z0-9_]+$` | `Bot_42`, not `Bot-42` |
| At least one non-whitespace byte | `\S` | `hello` matches; only spaces/tabs do not |
| `RX` followed later by `OK` | `RX.*OK` | `RX 123 OK`; not `OK then RX` |
| Content between brackets without crossing a closing bracket | `\[[^\]]*\]` | Searches a bracket-delimited segment without lazy `.*?` |

**OR between whole words or names: use separate rules.** Instead of a grouped
alternation such as `^(BotA|WeatherBot)$`, write:

```text
filter add chan=#test sender=^BotA$ action=logonly
filter add chan=#test sender=^WeatherBot$ action=logonly
```

Repeat the channel and any other shared restrictions in every rule. If the only
difference is one byte, `sender=^Bot[AB]$` can cover `BotA` or `BotB` in one rule.
`[BotAWeatherBot]` is not a substitute for word alternation.

**Repeated words:** `(ha){3}` becomes `hahaha` for exactly three repetitions.
There is no general grouped equivalent to `(ha)+`. Making a sequence of optional
letters does not make the entire word optional; use separate rules for the
with-word and without-word cases.

**Word boundaries:** `\bBOT\b` is unsupported. For the narrower requirement
“`BOT` is the whole text or is separated by literal spaces”, use these four
alternatives, depending on the positions you want to cover:

```text
filter add chan=#test text=^BOT$ action=logonly
filter add chan=#test text="^BOT " action=logonly
filter add chan=#test text=" BOT$" action=logonly
filter add chan=#test text=" BOT " action=logonly
```

These deliberately do not treat punctuation or tabs as separators. Replace a
space with `\s` to accept whitespace, or an explicit class for your allowed
separators. This is an explicit delimiter policy, not a Unicode word-boundary
implementation. Multiple matching alternatives still obey first-match ordering.

**AND of two text fragments:** there is no lookahead expression such as
`(?=.*RX)(?=.*OK)`. If order matters, use `text=RX.*OK`. If either order is
acceptable, use two otherwise-identical rules with `text=RX.*OK` and
`text=OK.*RX`. Both fragments are then required in either order (without
crossing CR/LF under the default dot behavior). Do not repeat `text=` in one
command; it replaces the earlier value rather than adding another predicate.

**Negation:** `[^X]` negates one byte class, not an entire predicate or message.
There is no general `NOT text=...`, negative lookahead, or negated channel list.
Do not use a `logonly` rule as a universal allowlist exception; see §2 for phase
ordering and the independent advert limiter.

### 11.6 CLI quoting and host-language escaping

There are two separate layers: the command string sent to the repeater, and
any programming-language string used to construct it. Get the repeater command
right first, then escape it for the host language.

**Direct repeater CLI:** use double quotes only to keep spaces within one value.
Backslashes are passed through to TinyRegex, not expanded as C/Python escapes.
Single quotes are not the repeater tokenizer's quoting mechanism.

```text
filter add chan=#test text="^RX in place$" action=logonly
filter add chan=#test text=^RX\s+in\s+place$ action=logonly
filter add chan=#test text=^v1\.2$ action=logonly
```

The first command requires exactly one space at each position. The second
accepts one or more whitespace bytes at each position. The third matches a
literal dot. Do not double these backslashes when typing directly into the CLI.

**Literal double quotes are a CLI limitation.** The tokenizer strips double
quote characters wherever they occur; an unmatched quote is rejected. `\"`
does not provide a way to pass a literal quote to the regex engine. If suitable,
use `.` as a one-byte placeholder, understanding that it also matches other
non-newline bytes. For example, `text=^say.OK.$` matches `say"OK"` but also
`say!OK!`; it is not an exact quote match. `\x22` cannot help because TinyRegex
does not decode hex escapes.

**Python:** a raw string preserves the backslashes for the repeater:

```python
command = r'filter add chan=#test text=^ID:\s*\d\d\d$ action=logonly'
# Equivalent ordinary Python string:
command = 'filter add chan=#test text=^ID:\\s*\\d\\d\\d$ action=logonly'
```

**JavaScript:** either double backslashes in an ordinary string or use
`String.raw`. Do not pass a JavaScript `RegExp` object's slash-delimited string.

```javascript
const command = 'filter add chan=#test text=^ID:\\s*\\d\\d\\d$ action=logonly';
const sameCommand = String.raw`filter add chan=#test text=^ID:\s*\d\d\d$ action=logonly`;
```

These strings all send the same single-backslash regex to the repeater. There
is no Python `re` or JavaScript RegExp evaluation involved. A host language may
turn `"\n"` into an actual newline before sending it; that is host-string behavior,
not TinyRegex's escape syntax, and may also break command transport/tokenization.

**Shells and JSON introduce their own escaping.** A shell can interpret `>`,
`*`, brackets, `$`, or `#`; JSON requires backslashes to be escaped. Pass the
complete command as one correctly quoted string through your client, and use
`filter get <idx>` to inspect what the repeater stored. Do not paste the examples
as standalone bash commands.

### 11.7 UTF-8, bytes, and newlines

TinyRegex is **not Unicode-aware**. Literal UTF-8 sequences are supported, but
matching, classes, repetition, and storage limits operate on individual bytes.

- `sender=^John😀$` can match that exact UTF-8 name. No `\u...` notation is needed.
- One ASCII letter is one byte; `😀` is four UTF-8 bytes. `^J.hn$` does not match
  `J😀hn`; `^J....hn$` can, as can the exact literal `^J😀hn$`.
- `[😀]` is a set of its encoded bytes, not a class containing one emoji.
  Likewise `😀+` repeats only the last encoded byte, not the entire emoji.
  Prefer literal sequences or separate rules for non-ASCII alternatives.
- Accented-letter normalization and Unicode case folding are not performed.
  Visually identical names encoded differently may not match the same literal.
- `\w` is not “any Unicode letter”; `\d` is not “any Unicode numeric character”.
- Dot excludes both CR and LF by default. `^A.B$` does not match a subject
  consisting of `A`, a newline, then `B`. `^A[\s\S]B$` does match that shape,
  because the class consumes any one non-NUL byte.
- A custom firmware build can define `RE_DOT_MATCHES_NEWLINE=1` to change dot
  behavior. There is no per-rule `s` flag. Anchors still refer to the whole
  parsed subject, not each line.

### 11.8 Limits, rejected patterns, and fail-open behavior

There are **separate limits**, so fitting in a text field is not enough:

| Limit | Default and how to count it |
|---|---|
| Sender pattern storage | At most **23 bytes**, plus NUL in the 24-byte array |
| Text pattern storage | At most **47 bytes**, plus NUL in the 48-byte array |
| Compiled symbols | **29 usable symbols**, plus a sentinel in the 30-slot array |
| Character-class buffer | **40 bytes shared by all classes in one pattern**, including internal reserved/terminator bytes |
| Matching work | **5,000 budgeted steps per regex evaluation** by default; not milliseconds |

Storage capacities come from
[PacketFilterConfig.h](examples/simple_repeater/PacketFilterConfig.h); engine
capacities and the default budget are in `TinyRegex.cpp`. Custom builds can
change defaults. CLI quoting characters are stripped before pattern storage;
literal pattern bytes and regex backslashes count. `\d` uses two pattern bytes
but one compiled symbol. A UTF-8 emoji uses four pattern bytes and four literal
symbols. A class such as `[A-Z]` is one symbol but also occupies class storage.
Anchors and quantifiers each consume a symbol of their own.

For example, `^Bot\d+$` is 8 pattern bytes and 7 symbols: `^`, `B`, `o`, `t`,
`\d`, `+`, `$`. A 30-letter literal text pattern fits the text storage but exceeds
the 29-symbol compile limit. Long classes can exhaust their separate shared
buffer even if the pattern uses few symbols. Shorten a pattern or split
alternatives across rules; there is no automatic truncation of an overlong regex.

`Err - bad/long sender regex` or `Err - bad/long text regex` can indicate syntax,
storage length, symbol count, or class-buffer limits. Empty values (`sender=`
or `text=""`) are rejected. To leave a field unrestricted, omit it rather than
supply an empty regex. Patterns that can match an empty substring, such as
`a*` or `.*`, can match much more broadly than intended; use `+` or required
literal content when a nonempty match is necessary.

A matching-budget exhaustion increments `aborted` and makes that regex predicate
**not match**. The filter may continue to later rules; another rule or forwarding
restriction can still drop the packet. This fail-open behavior protects the
repeater from spending unbounded time matching, but does not guarantee your
intended block will succeed. Sender and text are separate regex evaluations,
each with its own budget when reached.

Prefer a short, distinctive literal prefix (`^BEACON`) or constrained classes
over chains of unconstrained repetitions. There is no need to wrap a substring
search as `.*BEACON.*`: `BEACON` already searches anywhere. Packet/channel
conditions can narrow when a content regex is evaluated, but do not remove the
regex's own limits.

### 11.9 Operator checklist and troubleshooting

When configuring a repeater, start with one narrowly scoped `logonly` rule and
inspect its stored form and counters before choosing to enforce it. The commands
below illustrate the workflow; use the index actually returned by `filter add`.

```text
filter add chan=#test sender=^Bot[0-9]+$ text=^BEACON action=logonly
filter on
filter list
filter get 0
filter stats
```

| Symptom | Check |
|---|---|
| Rule gets no hits | Is the whole filter on? Is the rule enabled? Did an earlier rule match in the same phase? |
| Sender/text never matches | Can the repeater decrypt the channel? Is this group text? Are you matching the correct parsed field without its colon? |
| Matches too broadly | Add anchors; escape literal dots; avoid zero-length patterns; remember comma lists are OR choices |
| Combined rule matches nothing | Every specified condition is ANDed; check each condition, route compatibility, and case |
| Pattern copied from a desktop tester fails | Remove delimiters/flags and unsupported groups, alternation, counts, boundaries, or escapes |
| Literal space behaves differently from `\s` | `\s` includes tabs and line breaks; use quoted literal spaces if that is the requirement |
| Unicode name fails | Check exact UTF-8 bytes, byte limits, normalization, and whether a dot/class was used for a multibyte character |
| “Bad/long regex” error | Check all storage/compile limits, closing brackets, and trailing backslashes (§11.8) |
| `aborted` grows | Simplify the pattern, especially overlapping repetitions; an aborted predicate does not enforce its drop |
| New drop rule never fires | Remove/disable an earlier overlapping `logonly` rule in the same phase; re-check shifted indices |

Choose representative examples that **should match** and near-misses that
**should not**: different channel, different sender, changed case, missing
prefix, extra suffix, different hop count. A hit counter alone does not tell
you which field made your intended pattern too broad. Standard online regex
testers cannot establish compatibility with this firmware's reduced dialect.
