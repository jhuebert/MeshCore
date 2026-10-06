# Repeater Packet Filter — User Guide

A remotely configurable packet filter for the `simple_repeater` firmware. You can
add, inspect, and remove filter rules over the repeater CLI — locally via serial,
or remotely over the mesh as an authenticated admin. No rebuild or physical
access to the repeater is required.

> Maintained on the [jhuebert fork](https://github.com/jhuebert/MeshCore) — prebuilt
> firmware is on the fork's Releases page (`filter-v*` tags).

**Quick navigation:** [Quick start](#quick-start) ·
[Common setups](#common-setups) ·
[Command reference](#command-reference) ·
[Writing sender/text patterns](#writing-sendertext-patterns)

---

## What it does

The filter decides whether the repeater should **relay** a packet onward or drop
it. It governs *forwarding*, not every packet that arrives.

- Each rule is a list of conditions. A packet that meets **all** of a rule's
  conditions is handled by that rule's action.
- If no rule matches, the packet is forwarded as usual. The filter only ever
  removes traffic you explicitly target.
- Rules that match on channel name, sender, or message text look at the
  *decrypted* message, so they only fire on traffic the repeater can actually
  read. Other rules (type, route, hops, signal strength, …) work on every packet.

Two honest limits on what "every packet" means:

- **Traffic addressed to this repeater is handled, not filtered.** A management
  request for the node itself is answered directly; there is no relaying
  decision to make. The filter never gates your ability to log in or read status.
- **A packet addressed directly to this repeater is never decrypted.** Group
  traffic that arrives over a direct (endpoint-to-endpoint) route does not pass
  through the repeater's decryption, so `chan=`, `sender=` and `text=` cannot
  match it here even when you have the key. Use packet-level conditions
  (`route=`, `type=`, `chanhash=`) for that traffic.

Everything is saved on the repeater and survives reboots.

## Quick start

Silence one noisy channel, keeping everything else:

```text
filter add chan=#memes
filter on
filter stats          # watch hits climb as #memes traffic arrives
```

Rules default to **drop**, and the filter is already **on** on a fresh node, so
`filter on` here is belt-and-braces: it makes the state explicit rather than
relying on a default. To undo: `filter del 0` — rules are numbered **starting at 0**, so the
first rule is rule 0 (the repeater prints the number when it adds a rule) —
or `filter clear` to remove all rules.

## How rules work

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

Inside a single `sender=`/`text=` pattern, `|` is the OR — see
[Writing sender/text patterns](#writing-sendertext-patterns).

### Good to know

- Some values accept comma-separated alternatives, which acts as an OR inside
  that one condition: `chan=#local,#weather`, `type=txt,data`. Only `type`,
  `chan`, `hsize`, and `region` support this. `sender=Alice,Bob` searches for
  the literal text `Alice,Bob` — it is not a list.
- Rules are evaluated **top to bottom**: the first enabled rule whose conditions all match decides the packet's fate, and later rules are not consulted. A rule with `action=forward` is terminal too — it counts the hit and forwards the packet, skipping the rest of the list.
- Decryption-dependent conditions (keyed `chan=`, `sender=`, `text=`) can only match traffic the repeater actually decrypts. A rule carrying any of them is skipped for packets that never decrypt (adverts, wrong or missing keys), so a later packet-level rule still decides those.
- Name each condition once per rule. Repeating a key (e.g. a second `text=`)
  replaces the earlier value rather than adding another condition. That includes
  the list conditions: `type=advert type=txt` is just `type=txt`, and
  `chan=#a chan=#b` is just `chan=#b`. Repeating `path=` replaces the whole
  chain. `chanhash=` is a separate condition, so it survives a repeated `chan=`.
- A rule without `type=` applies to **all** packet types, including adverts and
  telemetry. Add `type=` when you want to narrow it.
- Invalid input is rejected with `Err - ...`; no half-added rule is left behind.

## What you can match on

Each `filter add` takes one or more of these `key=value` conditions, separated
by spaces. Values containing spaces go in double quotes: `text="^RX in place"`.

| Condition | Values | Matches |
|---|---|---|
| `type` | `advert`, `txt`, `data`, `any` (comma-combine) | Packet category. `txt` means **group text**, `data` means **group data**. `any` is the wildcard: all packet types, and it wins if it appears beside named types |
| `route` | `flood` or `direct` | How the packet travelled; omit to match either |
| `hops` | interval | Flood hop count (direct packets never match) |
| `len` | interval | Payload size in bytes |
| `snr` | interval, dB | Received signal strength at your repeater |
| `path` | `[^]HEX>HEX>…[$]` | Repeater IDs on the flood path (see [path examples](#path)) |
| `hsize` | `1..4` (comma-combine) | Path ID size used by the packet. `4` is accepted for completeness but is reserved by the protocol and never appears on air |
| `chan` | channel name(s) | Keyed channel — proven by decryption (see [Channels](#channels-senders-and-message-text)) |
| `chanhash` | 2 hex digits | On-air channel tag, no key needed (see [chanhash examples](#chanhash)) |
| `region` | region name(s), `unscoped` (comma-combine) | Flood region the packet arrived in (direct packets never match) |
| `sender` | pattern | Sender name in group text, e.g. `SpamBot` from `SpamBot: hello` |
| `text` | pattern | Message text in group text |
| `prob` | `1..100` | Match probability: the rule decides only that percentage of the packets its conditions match (see [prob examples](#prob)) |
| `throttle` | seconds, `1..65535` | Rate gate: the rule decides only the matches that exceed one per N seconds; one per N seconds slips past (see [throttle examples](#throttle)) |
| `action` | `drop` (default) or `forward` | What to do on a match: `drop` discards the packet, `forward` stops the rule list and lets it through (see [shadow mode](#trying-a-rule-before-enforcing-it-shadow-mode)). Upgrading from firmware that called this `logonly`: such rules now read as `forward` — the stored value is unchanged, only the keyword and display moved. The *rule ordering* around them is not identical on old firmware (see [Upgrading from earlier firmware](#upgrading-from-earlier-firmware)) |

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
quarter-dB steps: `.00`, `.25`, `.50`, `.75`, and a value that is not a quarter
is rounded to the nearest one (`snr=-8.5` means −8.50 dB; `snr=-8.6` becomes
−8.50). SNR is stored as signed quarter-dB values, so the usable range is
−32768…32767 quarter-dB — far beyond any real radio reading. Plain numeric
predicates (`hops=`, `len=`) are unsigned and top out at 32767; the endpoints you
can usefully write are much lower than that.

## Channels, senders, and message text

### Channels

Content rules match against the repeater's store of named channels:

- `Public` is built in with the well-known Public key.
- `#name` channels need no key — the key is derived from the name, exactly as
  the companion apps do. Just write `chan=#memes` in a rule; the channel is
  added to the store automatically.
- Private (non-`#`) channels need a key: add them first with
  `filter chan add <name> <psk-hex>` (32 or 64 hex digits — 16 or 32 bytes),
  then reference the name in a rule.
- A channel name cannot contain `,`, `=`, `"` or control characters: those
  cannot be represented in a rule's comma-separated `chan=` list, so a channel
  named with them could never be referenced by any rule. Ordinary spaces are
  fine — quote the value where you use it: `chan="two words"`. Names already in
  the store from an older config are left alone; they stay visible in
  `filter chan list` even though a rule cannot reference them.
- The on-air channel hash is only one byte, so a channel's key can collide with
  another's. The repeater offers core at most **4 distinct keys** per hash, and
  only keys that differ count: several channel *names* sharing one key are free,
  because they decrypt identically. `filter chan add` says so
  (`multiple keys on this hash; core tries 4`) when a hash starts carrying keys
  the repeater may never get to try.
- Channel names are literal, not patterns: `chan=#test.*` does **not** mean
  "all test channels".
- If you delete the last channel a rule named, that rule's `chan=` list becomes
  empty and the rule stops matching **anything** — deliberately. It is not
  turned into a catch-all, so deleting a channel can never quietly widen a rule
  into dropping every packet of that type. Re-point the rule or remove it.
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
- The sender name and the text are read up to the end of the message, so a name
  of any length can be matched, and a pattern anchored with `$` sees the real end
  of the field rather than a shortened copy. A message with no `:` has no sender
  field at all — `sender=` cannot match it, not even with `^$`.
- Matching is **case-sensitive**, and a pattern matches *anywhere* in the
  field unless you anchor it. The three most common shapes, at a glance:

  | You want | Pattern | For `SpamBot: hello` |
  |---|---|---|
  | Name *contains* `SpamBot` | `sender=SpamBot` | Matches; also matches `MySpamBot2` |
  | Name *starts with* `SpamBot` | `sender=^SpamBot` | Matches; also matches `SpamBot2` |
  | Name is *exactly* `SpamBot` | `sender=^SpamBot$` | Matches; not `SpamBot2`, not `spambot` |

  See [Writing sender/text patterns](#writing-sendertext-patterns) for the
  full pattern language.

## Cutting advert noise (rate limiter)

Independent of the rules, you can rate-limit flood adverts per originating
node: *each node's advert is forwarded at most once every N hours.*

The window counts adverts this repeater actually **admitted for relay**, not
every advert it heard. An advert refused afterwards — by `set off`, a hop limit,
an unknown region, loop detection, or the battery gate — does not use up the
node's budget, so turning forwarding back on does not leave that node invisible
for the rest of the window. The honest limit: an advert queued for transmission
that never makes it onto the air still counts as admitted. Already-queued
relays are not cancelled by turning the filter or the battery gate off.

```text
filter ratelimit advert 48     # each origin forwarded at most every 48 h
filter ratelimit clear         # optional: forget history, start clean
filter ratelimit               # show current window and cache usage
```

Use this on a well-connected repeater to stop re-flooding everyone's periodic
adverts while still passing each node's advert once per window so it stays
reachable through you. The window can be 0 (off) to 720 hours; 0 turns it off.

## Command reference

All commands begin with `filter`. They work identically over serial and remote
admin (see [Managing the repeater remotely](#managing-the-repeater-remotely)).

| Command | Effect |
|---|---|
| `filter` | Status line, e.g. `on; rules 1/16; chans 2/16; ratelimit advert 0h; cache 0/256; limiter 0; aborted 0` — filter on/off, rules used out of 16, channels stored out of 16, advert rate-limit window and cache fill, limiter and regex-abort counters |
| `filter on` / `filter off` | Enable/disable the whole filter (rules are kept) |
| `filter add <cond>=<val> ...` | Add a rule (space-separated conditions, see [What you can match on](#what-you-can-match-on)) |
| `filter list` | One line per rule, e.g. `on 1/16: 0eDBE9` — see below for how to read it |
| `filter stats` | Totals: limiter drops, regex aborts, airtime saved, and a per-rule hit count |
| `filter stats reset` | Zero the counters above (per-rule hits, airtime saved, limiter drops, regex aborts). Rate state is **not** reset: a throttled rule gets no free pass, and advert history is kept |
| `filter get <idx>` | Full detail of one rule, including its `throttle=`/`pass=` rate gate, hit count and saved-airtime stat |
| `filter enable <idx>` / `filter disable <idx>` | Toggle a single rule |
| `filter move <from> <to>` | Move a rule so it ends up **at** index `<to>` (the rules in between shift; hit counters travel with the rule) |
| `filter del <idx>` | Delete a rule (later rules shift down one index) |
| `filter clear` | Delete all rules (channels and ratelimit are kept) |
| `filter chan` / `filter chan list [<start-idx>]` | List the stored channels. A full store does not fit one reply, so a cut-off listing ends with `next=N`; repeat the command with that index to see the rest |
| `filter chan add <name> [<psk-hex>]` | Add a channel; key optional for `#` names, 32 or 64 hex digits |
| `filter chan del <name>` | Remove a channel (existing rules are updated) |
| `filter ratelimit` | Show the advert ratelimit window and cache usage |
| `filter ratelimit advert <hours>` | Set the window (0–720 h; 0 = off) |
| `filter ratelimit clear` | Empty the advert cache |
| `filter stats` | Per-rule hit counters, limiter/abort counters, and total saved airtime with percentage (see below) |
| anything else | Usage line listing the commands |

Notes:

- **Reading `filter list` output.** The line starts with the filter state and
  rule count (`on 1/16` = on, 1 rule of max 16), followed by one token per
  rule. Each token packs four facts:

  ```text
  0eDBE9
  │││└─ 3 hex digits: digest of the rule's content (changes whenever
  │││     any part of the rule changes; identical rules always match)
  ││└─── D = drop, F = forward
  │└──── e = enabled, d = disabled
  └────── rule index (0-based position)
  ```

  The digest is handy as a change indicator: if a rule's digest is the same
  before and after a reboot or re-add, its stored content is unchanged.
- **Commands that take a rule number want the rule's index**, not the whole
  `filter list` token. Indexes count from **0**: the first rule added
  is rule 0, the second rule 1, and so on. The repeater prints the index when
  you add a rule (`OK - rule 0 added`). So `filter get 0` is correct;
  `filter get 0eDBE9` is not — the token is for display only.
- Deleting a rule shifts later rules down: delete rule 1 of 3 and the old
  rules 2 and 3 become 1 and 2. Re-check `filter list` after deletions.
- The repeater replies with short lines; `filter get <idx>` gives the most
  detail about a rule.
- Counters (`filter stats`) and per-rule rate state reset to zero on reboot —
  the rate state means each [`throttle=`](#throttle) rule grants one free pass
  after a reboot; the rules themselves do not.

## Common setups

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

### Slowing one sender down instead of muting them

Bob floods `#test`, but you don't want him gone — just quieter. Let one
message per minute through and drop the rest:

```text
filter add chan=#test sender="^bob" throttle=60
filter get 0
# r0 en drop chan=#test sender="^bob" throttle=60 pass=12 hits=5 air=2140
```

Reading that line: `pass=12` messages got through (one per minute), `hits=5`
excess messages were dropped, and `air=2140` is the retransmit airtime those
drops saved. Like every `sender=` rule, this matches the message *text*, not a
verified identity — a renamed sender evades it (the honest limit of content
matching on group text).

### Cutting advert noise

See [Cutting advert noise](#cutting-advert-noise-rate-limiter) — the one-liner
is `filter ratelimit advert 48`.

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

`path=` only ever matches **flood** traffic. A packet addressed directly to this
repeater carries an *itinerary* between its two endpoints rather than the list of
repeaters that relayed it, and a TRACE packet's path holds collected SNR values,
not relay IDs — so `route=direct` packets never satisfy `path=`, even when their
path bytes would match.

### Keeping unwanted regions off a channel

Flood packets can carry a region scope. To forbid plain, scope-less flood
traffic on a channel:

```text
filter add chan=#mychan region=unscoped
```

To exclude a specific region from a channel — here, dropping `#mychan` traffic
that arrived via region `Foo` while every other region still passes:

```text
filter add chan=#mychan region=Foo
```

Region names must already exist on the repeater (`region def ...`). Direct
packets have no region and are never affected.

### Allowlist within a channel (let one sender through)

Drop everything on `#foo` **except** messages from Alice. Put the exception
rule above the catch-all drop — rules are tried top to bottom, so Alice's
messages match the exception first and the drop is never consulted for them:

```text
filter add chan=#foo sender=^Alice$ action=forward   # rule 0: Alice passes
filter add chan=#foo                                 # rule 1: everyone else is dropped
```

Added the exception after the drop? Reorder without retyping it —
`filter move <from> <to>` moves a rule so it ends up **at** the target index:

```text
filter list                    # the drop (rule 0) sits above Alice's rule (rule 1)
filter move 1 0                # Alice's rule ends up at index 0, above the drop
```

To preview how much the drop would remove before enforcing it, see
[shadow mode](#trying-a-rule-before-enforcing-it-shadow-mode).

### Turning a rule off temporarily

Disable rather than delete, so you can re-enable it later:

```text
filter disable 2     # keep rule 2 but skip it
filter enable 2      # put it back
```

## Examples for every condition

Complete, independent commands. Anything not specified is unrestricted. Rules
default to dropping — add `action=forward` if you want to just count matches
first (see [shadow mode](#trying-a-rule-before-enforcing-it-shadow-mode)).

### `type`

| Command | What matches |
|---|---|
| `filter add type=advert` | Adverts, regardless of route |
| `filter add type=txt` | Group text on any channel, without needing its key |
| `filter add type=data route=flood` | Group data AND flood route |
| `filter add type=txt,data snr=[*,-8.5]` | (Group text OR group data) AND weak signal |
| `filter add type=req` | Requests (`REQ` packets) |
| `filter add type=response` | Responses to requests |
| `filter add type=anonreq` | Anonymous requests |
| `filter add type=msg` | Direct (1:1) text messages |
| `filter add type=ack` | ACK packets |
| `filter add type=req,anonreq` | Either request kind |

Names can be comma-combined, `type=any` (or omitting `type` entirely) covers
every packet type, and an advert does not match `type=txt,data`.

#### Encrypted payload types

Most payloads the repeater relays are **encrypted end-to-end**, so a rule can
only see the envelope: `req`, `response`, `anonreq` and `msg` packets carry no
readable sender or text. `sender=`, `text=` and `chan=` cannot match them —
the repeater rejects such a rule outright (`Err - sender=/text=/chan= only
match group traffic`) instead of accepting one that could never fire.
Envelope predicates — `type`, `route`, `hops`, `len`, `snr`, `path`, `hsize`,
`region` — all work normally on these types.

A note on visibility: the filter only sees traffic this repeater would relay.
Direct-routed ACKs, answered requests and delivered messages are consumed by
the endpoints and never reach a repeater, so `type=ack` mostly sees flood
ACKs — the ones re-broadcast across the mesh.

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
| `filter add sender=^Alice$\|^Bob$` | Name is exactly `Alice` **or** `Bob` — up to 8 alternatives, see [alternation](#alternation) |

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
| `filter add text=^PING$\|^PONG$` | Text is exactly `PING` **or** `PONG` — see [alternation](#alternation) |

The last pattern matches `ID:123` and `ID: 042`, but not `ID:12` or `ID:1234`.

### `prob`

A rule with `prob=N` decides only about N% of the packets its conditions all
match. On a "failed roll" the rule steps aside and evaluation continues with
the next rule, exactly as if the conditions had not matched — when every
matching rule fails its roll, the packet passes. This is *dosing*: apply
pressure without a hard cutoff.

- Omit `prob=` for the default: 100% (always decides).
- It works on both actions: a `drop` rule with `prob=75` drops 3 of 4 matching
  packets; a `forward` probe with `prob=75` terminates the list on 3 of 4.
- A later static rule acts as the fallback for the packets a dosed rule
  lets through — e.g. a `prob=80` drop followed by an unconditional rule
  expresses "80% pressure, guaranteed floor".
- The roll is **deterministic per packet**: the same packet always gets the
  same verdict from the same rule, so counters are stable and repeatable.
  Note what that means for retransmits: the roll is derived from the payload and
  packet type, so a copy forwarded by a neighbour to reach this repeater rolls
  **identically** to the first copy — it is the same message. A genuinely new
  message (different body or timestamp) is a new packet and rolls afresh.
- Determinism is scoped to the rule **as stored**: the roll is salted with a
  digest of the rule's own fields, so editing a `prob=` rule (or reading the
  same config under firmware whose rule record differs) re-rolls it. `prob` is
  dosing, not a stable contract — use a plain drop rule where "exactly these
  packets" matters.
- `prob=0` is rejected — a 0% rule is a disabled rule; use
  `filter disable <idx>` instead.

| Command | Effect |
|---|---|
| `filter add chan=#chat sender="^Bot" prob=50` | Halve the bot's delivered traffic on `#chat` without cutting its owner off |
| `filter add hsize=1 prob=75` | Degrade ambiguous 1-byte-hash relaying to 25% pass-through, nudging nodes to upgrade |
| `filter add chan=#auction prob=70` | Shed 70% of a busy event channel's load; users see degradation, not silence |
| `filter add type=advert action=forward prob=10` | Shadow-mode *sampling*: count a representative 10% of adverts without enforcing anything |

Note: `hits` counts **decisions**, not condition matches — a packet the rule
matched but then stepped aside on (a failed roll, or within a
[`throttle=`](#throttle) budget — see below) is not counted (and not shown in
`filter get`).

### `throttle`

A rule with `throttle=N` lets **at most one matching packet every N seconds
slip past the rule untouched**; every other matching packet inside those N
seconds triggers the rule's action (default: `drop`). `throttle` and `prob`
answer the same question about a packet that matched all of a rule's
conditions — *does the rule get to decide this packet?* — and a rule that
steps aside behaves exactly as on a failed `prob` roll: evaluation continues
with the next rule, exactly as if the conditions had not matched. This is a
*rate gate*: "one per minute" instead of a mute.

- Omit `throttle=` for the default: no limit.
- It works on both actions: a `drop` rule with `throttle=60` drops the excess;
  a `forward` probe with `throttle=60` forwards everything and counts exactly
  the excess (see [shadow mode](#trying-a-rule-before-enforcing-it-shadow-mode)).
- **One budget per rule.** The rule's whole matched stream shares it — scope
  the rule with `sender=`/`chan=`/etc. to scope the budget. **Careful:**
  `chan=#test throttle=60` with no `sender=` limits the *channel* to one
  message per minute total — first come, first served, and one busy sender
  could starve everyone else.
- Over-rate firings **do not extend** the window: a matching stream sustains
  exactly one pass per N seconds however hard it is pushed ("1 per minute"
  always, not "1 per quiet minute"). The boundary is exact: at N seconds it
  is the next pass.
- The rate state is RAM-only and resets on reboot — the first match after a
  reboot is a free pass.
- Combined with `prob=`: *prob filters what the rule sees; throttle meters
  what it sees* — a failed roll never touches the budget.

| Command | Effect |
|---|---|
| `filter add chan=#test sender="^bob" throttle=60` | Bob's `#test` messages: at most one per minute; the rest are dropped |
| `filter add sender="^Bot" throttle=10` | Slow a chatty bot to one message per 10 s instead of muting it |
| `filter add type=data throttle=600` | A misbehaving telemetry sender gets one report per 10 minutes relayed |
| `filter add type=advert region=XX throttle=3600` | Adverts from a chatty region at one per hour through this repeater |
| `filter add chan=#test sender="^bob" action=forward throttle=60` | Shadow measure: `hits` counts exactly what a `drop` version would catch |

Note: `hits` counts **decisions** — for a throttle rule exactly the over-rate
firings (the would-be drops); within-budget passes show as `pass=` in
`filter get` and are not hits.

## Trying a rule before enforcing it (shadow mode)

Not sure a rule is right? Add it with `action=forward`: matching packets are
**counted but still forwarded**. Watch the counters, then enforce. Where the
probe sits matters:

- **Preview a drop:** put the `forward` probe immediately **before** the drop
  rule it shadows — it counts exactly what the drop would catch and, meanwhile,
  forwards (first match wins, so the drop never runs while the probe is
  active). When satisfied, flip the probe's action to `drop` (or delete it).
- **Preview a throttle:** a `forward` probe with `throttle=` counts exactly
  what the throttled drop would catch — its `hits` *are* the excess rate.
  Probe and drop twin meter their own budgets, so the probe placed first
  disarms the twin just like the warning below; keep only the probe while
  measuring.
- **Log what passes:** put a catch-all `forward` rule at the **end** of the
  list — it only sees packets no earlier rule matched, so it tallies surviving
  traffic without short-circuiting anything (e.g. `filter add type=txt
  action=forward` as the last rule).

```text
filter add chan=#test action=forward
# ...later, check:
filter stats           # hit counter grows; packets still forwarded
# when satisfied, replace it with an enforcing rule:
filter del 0
filter add chan=#test
```

Two things to remember:

- `forward` rules count hits but don't drop anything, and they don't stop the
  [rate limiter](#cutting-advert-noise-rate-limiter) from acting.
- Don't leave an overlapping `forward` probe in place after adding the real
  drop rule: a misplaced early probe silently disarms later drop rules, since
  the earlier rule matches first and the drop never fires.

The probe's worth is also visible in **RF terms**: `filter stats` includes
`air:<ms>:<percent>%` — an *estimate* of the time-on-air the packets dropped so
far would have consumed had they been relayed (rule drops and rate-limiter
drops). It is the repeater's own airtime estimate applied to the received packet
length, not a measurement of what the channel was doing, and not a count of
confirmed transmissions. Treat it as a comparative figure between "with the rule"
and "without it", not as a channel-utilisation figure. `filter get <idx>` shows the
per-rule share as `air=<ms>`. A shadow `forward` probe itself bills nothing
(`air=0`, since its packets are still relayed) — flip it to `drop` and those
same hits start accumulating the estimate, which is usually the number that
matters on a shared channel:

```text
filter stats           # e.g. lim:118 abort:0 air:214500:36%; hits: 0:42
```

The percentage is saved airtime divided by total estimated airtime evaluated
by the enabled filter, rounded to the nearest whole percent. It includes rule
and advert-limiter drops and is airtime-weighted, not packet-weighted. With no
evaluated airtime it shows `0%`. It measures the filter's estimated savings,
not actual transmitted airtime or channel utilization: packets allowed by the
filter may still be blocked by other forwarding checks.

All airtime counters are RAM-only and reset on reboot or a statistics reset,
like every counter.

## Managing the repeater remotely

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

## Limits and good-to-knows

| Limit | Value |
|---|---|
| Rules | 16 |
| Channels in the store | 16 (names up to 15 characters) |
| Sender pattern length | 23 characters |
| Text pattern length | 47 characters |
| Alternatives per pattern | 8 (`\|`-separated) |
| Advert rate-limit window | 0–720 hours (0 = off) |
| CLI reply length | short (~160 bytes) — use `filter get <idx>` for detail |

- Overly long or complex patterns are **rejected with an error**, not silently
  shortened. Keep patterns short and specific.
- Pattern lengths count the **whole** pattern, every `|` and anchor included:
  `sender=` fits about three short exact names (`^Alice$|^Bob$|^Carol$` is 21 of
  23 characters) and `text=` about six. Need more? Add a second rule, or use
  one broader alternative such as `^Bot`.
- Rules, channels, and settings survive reboots. Counters and the advert cache
  do not — they start fresh after every reboot.
- An edit is written to flash about **3 seconds** after you make it (a burst of
  commands costs one write, not one each). Power the repeater off within that
  window and that edit is lost — everything before it is safe.
- Saving is staged, so a crash or power cut mid-write cannot destroy your rules.
  The repeater writes a scratch copy, reads it back to confirm it, keeps your
  previous config as a backup, and only then swaps it in. If the main config ever
  fails to load, the backup is used and a repaired copy is written at the next
  save. So the worst case is that the most recent edit is lost, not the whole
  rule list. (This is recovery in the firmware, not a guarantee about the flash
  itself: a power cut *inside* the filesystem's own write can still damage the
  underlying storage.)
- The advert window and `throttle=` are measured on the repeater's own uptime,
  which keeps counting correctly no matter how long it stays on — including past
  the ~49.7-day point where a 32-bit millisecond counter wraps. State is
  RAM-only, so both start fresh after a reboot.
- A `len=` larger than the largest packet the radio can carry is accepted as
  valid syntax but can never match a real packet — the payload is bounded by the
  packet buffer, so keep length predicates within what a message can actually be.
- Very complicated patterns can be slow to match. Prefer short, distinctive
  patterns like `^BEACON` over long wildcard chains. The `aborted` counter in
  `filter stats` grows if a pattern gives up mid-match; simplify it if you see
  that. Several alternatives multiply that cost — see
  [Alternation](#alternation).
- When a pattern gives up, **that rule simply does not match** — it does not
  disable the whole filter. A later rule, including a plain drop, can still
  decide the packet. So a growing `aborted` counter means "some of your patterns
  stopped being checked", not "the filter is off"; check which rules carry
  patterns before assuming traffic is being handled.
- Rules are evaluated **top to bottom, first match wins** over the whole list —
  packet-level and content conditions live in the same ordered list. Older
  fork firmware ran content rules in a separate second pass; with interleaved
  configs the observable differences are: a matching `forward` or content rule
  now shields later drop rules, and a packet-level rule listed before a
  content rule now decides first for decrypted group text/data. The rule order
  in `filter list` is now honored exactly as listed.
- `forward` rules on decrypted group traffic count their hit at the moment the
  message is read (one scan instead of two). Counters are RAM-only and reset
  on reboot, so this self-heals.
- Configs are compatible in both directions: the stored action value did not
  change with the `logonly` → `forward` rename, so firmware that still says
  `logonly` reads and forwards a `forward` rule. That is *format* compatibility —
  the stored byte means the same thing. It is not a claim that old firmware
  behaves identically: a two-pass implementation evaluated rules differently, so
  the same config could decide differently on old code. If the exact outcome
  matters, check it on the firmware you actually run.
- One stored pattern is read differently by firmware from 2026-10 onwards: an
  unescaped `|` used to mean a literal pipe and now means OR — write `\|` if
  you want a literal pipe. A pattern built on a bare quantifier (`*Bot`) or a
  misplaced anchor (`A^B`) is accepted as before, but now matches nothing
  instead of matching or not depending on whichever pattern the engine compiled
  before it.
- **What a filter rule is, and is not.** Rules match on *what a packet says*,
  not on who is allowed to send it. A sender name is chosen freely by whoever
  writes the message, a channel key is shared by everyone holding it, and the
  protocol's message tag is two bytes. So a `sender=` or `chan=` rule is a
  **content policy**, not authentication: it reduces noise, it does not prove an
  identity. Treat it that way, and do not rely on it as a security boundary.
- **Keeping remote admin reachable.** A management request addressed *to this
  repeater* is answered directly, so your own filter — and the battery gate —
  never block logging in to it. Remote admin still depends on the path working:
  a drop rule on an *intermediate* repeater between your app and this one can
  silence the reply before it arrives. So before enabling a broad drop rule,
  add a higher-priority `forward` rule for the traffic you need relayed — e.g.
  `filter add chan=<admin channel> action=forward` — and test it from the app
  while you still have serial access. A rule exception here is about the
  *relay path*, not about authorising the login: nothing in this filter makes a
  management packet authenticated.

## Writing sender/text patterns

`sender=` and `text=` use a small pattern language (**TinyRegex**) built into
the firmware. It is *not* JavaScript, Python, or PCRE regex — patterns copied
from those systems often mean something different here. This section is the
complete reference.

### Rules of thumb

1. Type the pattern as-is: `sender=^Bot$`, **not** `sender=/^Bot$/i`.
2. Matching is **case-sensitive** and searches anywhere unless anchored.
3. `^` at the start and `$` at the end make the match exact.
4. Use `.*` for "anything", not a shell-style `*`.
5. There are **no groups, counted repeats, or flags**.
6. `|` separates alternatives: `sender=^Alice$|^Bob$` — see
   [Alternation](#alternation) for anchors, the 8-alternative cap, and the
   escape for a literal pipe.

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
| `A\|B` | Either alternative (up to 8) | `^Alice$\|^Bob$` | Matches `Alice` or `Bob`; not `Alice2` |

Tips:

- A quantifier applies only to the character right before it: `ab+` repeats
  the `b`, not `ab`.
- A quantifier needs something to repeat in front of it, and `^`/`$` anchor
  only at the edges of a pattern, so `*Bot` and `A^B` match nothing at all.
  `Bot\d+` and `^A.*B$` are what you want.
- `[Bot]` means one of the characters `B`, `o`, `t` — not the word `Bot`.
- `\s` includes tabs and line breaks. If you want exactly one space, quote the
  pattern and type the space: `text="^RX OK$"`.
- To match punctuation like `.` `[` `\` literally, put a backslash in front.

### Alternation

`|` at the top level of a `sender=`/`text=` pattern means OR, so one rule can
cover several exact values:

```text
filter add chan=#test sender=^BotA$|^WeatherBot$
```

- **Anchors bind to their own alternative.** `^Alice|Bob$` means *starts with
  Alice* **or** *ends with Bob* — not "is Alice or Bob". Put both anchors on
  each side (`^Alice$|^Bob$`) when you mean an exact match.
- **There are no groups.** `|` splits wherever it appears at the top level, even
  between parentheses: `^(Alice|Bob)$` means *starts with `(Alice`* **or** *ends
  with `Bob)`* — neither Alice nor Bob. Write `^Alice$|^Bob$`; to match the
  literal name `(Alice|Bob)`, escape the pipe: `^(Alice\|Bob)$`.
- **At most 8 alternatives** per pattern; exceeding it is rejected. An empty
  alternative (`A|`, `|A`, `A||B`) is rejected too, because a blank branch would
  match everything. Both errors name the pattern they came from:
  `Err - too many alternatives (max 8) in sender regex`,
  `Err - empty alternative in sender regex`.
- **A literal pipe needs an escape:** `text=a\|b` matches `a|b`, and `|` inside
  brackets (`[|]`) is a literal pipe too. A pipe inside one of this guide's
  tables is written `\|` — that is the plain `|` you type, escaped so the table
  renders; the literal-pipe escape cannot be shown inside a table, so that case
  is spelled `[|]` there.
- **Cost:** alternatives are tried in turn, and each one that fails to match is
  a full match attempt — `PING|PONG` costs about twice what `PING` alone does
  on every packet. Keep the branches cheap and anchored (`^PING$|^PONG$`); a
  pattern that runs out of match budget gives up fail-open and shows up as
  `aborted` in `filter stats`. The whole pattern, not each alternative, must
  fit the pattern-length limit — see [Limits](#limits-and-good-to-knows).
- Only the first alternative that matches matters; the rest are not tried.

### What doesn't work

These familiar features **do not exist** here — and a few are silently treated
as ordinary text rather than rejected:

| You might try | What actually happens | Do this instead |
|---|---|---|
| Groups: `sender=^(Alice\|Bob)$` | The `|` splits even inside the parentheses, so this matches a name *starting with* `(Alice` or *ending with* `Bot)` — not Alice, not Bob | `sender=^Alice$\|^Bob$` |
| Quantifier with nothing to repeat: `sender=*Bot`, `sender=a**` | Accepted, but matches nothing — there is no symbol for the quantifier to repeat. Before 2026-10 it matched or not depending on whichever pattern matched before it | `sender=Bot\d*` |
| Anchor away from an edge: `sender=A^B` | Accepted, but matches nothing — `^` and `$` only anchor at the edges. Same caveat | `sender=^A.*B$` |
| Counted repeat: `\d{3}` | Matches literal `{3}` | `\d\d\d` |
| Case-insensitive: `/i`, `(?i)` | Not supported; always case-sensitive | `^[Bb][Oo][Tt]$` |
| Word boundary: `\b` | Matches the literal letter `b` | See the word-boundary recipe below |
| Lookahead: `(?=...)` | Not supported | Two fragments → two rules, or `RX.*OK` for ordered text |
| `\n`, `\t` escapes | Match the letters `n`, `t` | `\s` for whitespace |
| Newlines / multiline mode | No per-line matching | Patterns apply to the whole field |

`filter add sender=^Alice|Bob$` is accepted, but it means *starts with
Alice* **or** *ends with Bob* — `Alice2`, `xxBob`, and `AliceBob` all match it.
For an exact match of either name, write `filter add sender=^Alice$|^Bob$`.

After copying a pattern from another tool, verify with `filter get <idx>` and a
test message.

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

**OR between names:** one rule, alternatives separated by `|`:

```text
filter add chan=#test sender=^BotA$|^BotB$|^BotC$
filter add chan=#test sender=^Bot     # covers the whole family
```

See [Alternation](#alternation) for the rules that apply to every alternative —
the cap, the empty-branch rejection, and anchors that bind per alternative.

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
  whole emoji and emoji length eats into the [pattern-length limits](#limits-and-good-to-knows).
  Exact literal matches are safest for non-ASCII names.
- There is no case-insensitive option. Use explicit choices like `[Bb]` — and
  note that accented letters can't be handled that way; match them literally.

### Troubleshooting

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
| A channel is missing from `filter chan list` | The store holds more than fits one reply — the listing ends with `next=N`; run `filter chan list N` for the rest |
| Rule gets no hits | Is the filter on (`filter on`)? Is the rule enabled? Did an earlier rule match first? |
| Sender/text never matches | Can the repeater decrypt the channel? Is it group text? Are you matching the right field (sender without colon, text without name)? |
| Matches too broadly | Add `^`/`$` anchors; escape literal dots; remember `sender=Bot` matches `MyBot2` |
| Combined rule matches nothing | Every condition must be true — test each one alone; check `route=`/case |
| Pattern from an online tester misbehaves | Remove `/slashes/`, flags, groups, `{counts}`, `\b` — see the table above |
| `Err - empty alternative in sender regex` | A `|` with nothing on one side — write `^A$\|^B$`, never end a pattern with a bare pipe |
| `Err - too many alternatives (max 8) in text regex` | Fewer alternatives per pattern, a broader one (e.g. `^Bot`), or a second rule |
| Rule never fires, pattern looks odd (`*Bot`, `a**`, `A^B`) | Such a pattern matches nothing by design — nothing to repeat, or an anchor that is not at an edge. Rewrite it (`Bot\d*`, `^A.*B$`) |
| Wrote `^(A\|B)$` and nothing matches | There are no groups: the `|` splits anyway, so it means *starts with `(A`* **or** *ends with `B)`*. Write `^A$\|^B$` |
| "Bad/long regex" error | Pattern too long (see [Limits](#limits-and-good-to-knows)) or broken syntax; shorten or simplify |
| `aborted` counter grows | Pattern too complex — simplify it |
| Drop rule never fires | An earlier overlapping rule (often a `forward` probe) is matching first |
| A rule vanished after a reboot | The saved config was truncated or corrupt. Rules load only if each record is one this firmware could have written; the records that survived keep their indices, and everything from the first bad record on is dropped. Add the missing rules again with `filter add` |
| A rule with `chan=` matches nothing after removing a channel | Expected: deleting the last channel a rule named leaves the mask empty, and the rule stays inert rather than becoming a catch-all. Point it at a channel that exists, or drop the `chan=` |
