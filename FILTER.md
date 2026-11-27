# Packet Filter: User Guide

> Maintained on the [jhuebert fork](https://github.com/jhuebert/MeshCore): prebuilt
> firmware is on the fork's Releases page (`v*` tags).
>
> This repeater can also run an opt-in [chat bot](BOT.md) that answers `#test`
> chatter on its own keyed channel.

## What it does

The filter decides whether the repeater should **relay** a packet onward or drop it. It governs *forwarding*, not every packet that arrives. Each rule is a list of conditions; a packet that meets **all** of them is handled by that rule's action. If no rule matches, the packet is forwarded as usual. The filter only ever removes traffic you explicitly target. Rules, channels and settings are saved on the repeater and survive reboots.

- `chan=`, `sender=` and `text=` look at the *decrypted* message, so they only fire on traffic the repeater can actually read. Other conditions work on every packet the repeater would relay, including adverts.
- **Traffic addressed to this repeater is handled, not filtered.** A management request for the node itself is answered directly; there is no relaying decision to make. The filter never gates your ability to log in or read status.
- **A packet addressed directly to this repeater is never decrypted.** Group traffic that arrives over a direct (endpoint-to-endpoint) route does not pass through the repeater's decryption, so `chan=`, `sender=` and `text=` cannot match it here even when you have the key. Use packet-level conditions (`route=`, `type=`, `chanhash=`) for that traffic.

## Quick start

Silence one noisy channel, keeping everything else:

```text
filter add chan=#memes
filter on
filter stats          # watch hits climb as #memes traffic arrives
```

Rules default to **drop**, and the filter is already **on** on a fresh node, so `filter on` here makes the state explicit rather than relying on a default. To undo: `filter del 0`. Rules are numbered **starting at 0**: the first rule is rule 0 (the repeater prints the number when it adds a rule). Or use `filter clear` to remove all rules.

## How rules work

Every condition in a single `filter add` command must be true for that rule to act. Adding more conditions narrows the rule:

```text
filter add chan=#test sender=^SpamBot$ text=^BEACON hops=[2,*]
```

This drops messages on `#test` from `SpamBot` whose text starts with `BEACON` and that have already hopped at least twice. If even one condition fails, this rule does nothing.

Want "either/or"? Use two independent rules, so either one can act. To drop messages from `SpamBot` **or** messages starting with `BEACON` on `#test`, repeat the shared channel in each rule:

```text
filter add chan=#test sender=^SpamBot$
filter add chan=#test text=^BEACON
```

Inside a single `sender=`/`text=` pattern, `|` is the OR. The full pattern language is in [FILTER_PATTERNS.md](./FILTER_PATTERNS.md).

**Note:** some values accept comma-separated alternatives, an OR inside that one condition (`chan=#local,#weather`, `type=txt,data`). Only `type`, `chan`, `hsize` and `region` support this; `sender=Alice,Bob` searches for the literal text `Alice,Bob`, it is not a list. Rules are evaluated **top to bottom**: the first enabled rule whose conditions all match decides the packet's fate, and later rules are not consulted. A rule with `action=forward` is terminal too: it counts the hit and forwards the packet, skipping the rest of the list. Decryption-dependent conditions (keyed `chan=`, `sender=`, `text=`) are skipped for packets that never decrypt (adverts, wrong or missing keys), so a later packet-level rule still decides those. Name each condition once per rule: a repeated key replaces the earlier value rather than adding another condition (`type=advert type=txt` is just `type=txt`, `chan=#a chan=#b` is just `chan=#b`, a repeated `path=` replaces the whole chain). `chanhash=` is a separate condition, so it survives a repeated `chan=`. A rule without `type=` applies to **all** packet types, including adverts and telemetry. Invalid input is rejected with `Err - ...`; no half-added rule is left behind.

### What you can match on

Each `filter add` takes one or more of these `key=value` conditions, separated by spaces. Values containing spaces go in double quotes: `text="^RX in place"`.

| Predicate | Syntax | What it matches | Example |
|---|---|---|---|
| `type` | name or comma list; `any` wildcard | Packet category. `txt` means **group text**, `data` means **group data**. `any` wins if it appears beside named types | `type=txt,data snr=[*,-8.5]` |
| `route` | `flood` or `direct` | How the packet travelled; omit to match either | `route=flood len=[180,*]` |
| `hops` | interval | Flood hop count (direct packets never match) | `hops=(2,5)` |
| `len` | interval | Payload size in bytes, not characters and not the whole radio frame | `type=txt len=[*,80]` |
| `snr` | interval, dB | Received signal strength at your repeater | `snr=[*,-8.5]` |
| `path` | `[^]HEX>HEX>…[$]` | Repeater IDs on the flood path, flood traffic only | `route=flood path=^A1>B2$` |
| `hsize` | `1..4` (comma-combine) | Path ID size used by the packet. `4` is accepted but reserved by the protocol and never appears on air | `hsize=1,2` |
| `chan` | channel name(s) | Keyed channel, proven by decryption | `chan=#local,#weather` |
| `chanhash` | 2 hex digits | On-air channel tag, no key needed | `chanhash=7A` |
| `region` | region name(s), `unscoped` (comma-combine) | Flood region the packet arrived in; direct packets never match | `chan=#local region=Foo,unscoped` |
| `sender` | pattern | Sender name in group text, e.g. `SpamBot` from `SpamBot: hello` | `sender=^Alice$\|^Bob$` |
| `text` | pattern | Message text in group text | `text=^PING$\|^PONG$` |
| `prob` | `1..100` | Match probability: the rule decides only that percentage of matching packets. The roll runs before `throttle`, is deterministic per packet, and is salted with a digest of the rule's stored fields | `chan=#chat sender="^Bot" prob=50` |
| `throttle` | seconds, `1..65535` | Rate gate: the rule decides only matches that exceed one per N seconds; one per N seconds slips past. One budget per rule, with a free pass after boot or a stats reset | `chan=#test sender="^bob" throttle=60` |
| `action` | `drop` (default) or `forward` | `drop` discards the packet; `forward` is terminal and lets it through. Stored value is unchanged from the old `logonly` keyword | `chan=#foo sender=^Alice$ action=forward` |

`chan`, `sender` and `text` are content conditions: they are checked after the message is decrypted. Everything else is checked before forwarding and works on every packet, including adverts.

Interval syntax, used by `hops`, `len` and `snr`:

| You type | Meaning |
|---|---|
| `3` | exactly 3 |
| `[2,*]` | 2 or more (endpoints included) |
| `(2,5)` | more than 2 and less than 5 (endpoints excluded) |
| `[*,-4]` | −4 or less (handy for `snr`) |

`hops=[2,*]` means at least 2 hops, `len=[*,80]` at most 80 bytes, `snr=[*,-8.5]` a signal of −8.5 dB or weaker. Fractional SNR values use quarter-dB steps: `.00`, `.25`, `.50`, `.75`; a value that is not a quarter is rounded to the nearest one (`snr=-8.5` means −8.50 dB, `snr=-8.6` becomes −8.50). SNR is stored as signed quarter-dB values, so the usable range is −32768…32767 quarter-dB, far beyond any real radio reading. Plain numeric predicates (`hops=`, `len=`) are unsigned and top out at 32767; the endpoints you can usefully write are much lower.

### Prob and throttle

**`prob=N`.** About N% of packets whose conditions all match are decided by the
rule. On a failed roll the rule steps aside exactly as if the conditions had not
matched; if every matching rule fails, the packet passes. This is dosing, not a
cutoff. Omit for 100%. It works on both actions: `drop` with `prob=75` drops 3 of
4; `forward` with `prob=75` ends the list on 3 of 4. A later static rule is the
fallback: `prob=80` then an unconditional rule means "80% pressure, guaranteed
floor".

**Deterministic per packet.** The same packet always gets the same verdict from
the same rule. The roll comes from the payload and packet type, so a neighbour's
copy rolls identically to the first copy while a new message rolls afresh. The
roll is salted with a digest of the rule's stored fields: editing a `prob=` rule,
or reading it under firmware with a different rule record, re-rolls it. `prob` is
not a stable contract; use a plain drop when exact selection matters. `prob=0`
is rejected; use `filter disable <idx>`.

**`throttle=N`.** At most one matching packet every N seconds steps aside
untouched; other matching packets inside the window trigger the rule's action
(default `drop`). A stepping-aside pass behaves exactly like a failed `prob`
roll. Omit for no limit. On `drop` the excess is dropped; on `forward` everything
is forwarded and only the excess is counted.

**One budget per rule.** The whole matched stream shares it, so scope with
`sender=` or `chan=`. `chan=#test throttle=60` without `sender=` limits the
channel to one message per minute in total, first come first served, and one busy
sender can starve the rest. Over-rate firings never extend the window: at N
seconds it is the next pass. Rate state is RAM-only and resets on reboot, so the
first match after boot is a free pass.

**Together.** Prob filters what the rule sees; throttle meters it. The prob roll
runs first and a failed roll never touches the budget. `hits` counts decisions,
not condition matches: a failed roll or an in-budget pass is neither counted nor
shown in `filter get`. On a throttle rule `hits` are the over-rate firings;
in-budget passes appear as `pass=`.

### Channels, senders and message text

| Topic | Rule |
|---|---|
| Built-in channel | `Public` uses the well-known Public key. |
| Hashtag channel | `#name` needs no key: the name derives it, as in companion apps. Writing `chan=#memes` adds it automatically. |
| Private channel | Add a key first with `filter chan add <name> <psk-hex>` (32 or 64 hex digits, 16 or 32 bytes), then reference the name. |
| Channel name | Cannot contain `,`, `=`, `"` or control characters, because a comma-separated `chan=` list cannot represent them. Spaces are fine but must be quoted: `chan="two words"`. Names stored from an older config are left alone and shown by `filter chan list`. |
| On-air hash | One byte. Core accepts at most **4 distinct keys** per hash, and only differing keys count; several names sharing a key are free. `filter chan add` warns `multiple keys on this hash; core tries 4` when some keys may never be tried. |
| Names are literal | `chan=#test.*` does not mean "all test channels". Deleting the last channel a rule names empties its `chan=` list and the rule stops matching anything, deliberately; it never becomes a catch-all. Re-point or remove the rule. |
| `chan=` vs `chanhash=` | `chan=` needs a stored key and is a strong identity check. `chanhash=` matches a 1-byte on-air tag that other channels can collide with; use it only without the key. `chan` alone also matches group data; adding `sender` or `text` restricts it to group text. |
| Store size | At most 32 channels. `filter chan list` shows them; `filter chan del <name>` frees a slot. A `chan=` list can name them all; its length is bounded by the CLI command length (about 180 characters), not a rule limit. An oversized command is refused whole with `Err - command too long`, never acted on as a truncated prefix. |

For `SpamBot: hello`, `sender=` matches the name without the colon (`SpamBot`)
and `text=` matches the message only (`hello`). Use both in one rule when you
need both. These apply only to group text, never adverts or binary group data.
Matching is case-sensitive and searches anywhere unless anchored:

| You want | Pattern | For `SpamBot: hello` |
|---|---|---|
| Name *contains* `SpamBot` | `sender=SpamBot` | Matches; also `MySpamBot2` |
| Name *starts with* `SpamBot` | `sender=^SpamBot` | Matches; also `SpamBot2` |
| Name is *exactly* `SpamBot` | `sender=^SpamBot$` | Matches; not `SpamBot2` or `spambot` |

Patterns use a small regex-like language, not JavaScript or PCRE. For the full
syntax, alternation, quoting, field-splitting edge cases, Unicode and
troubleshooting, see [FILTER_PATTERNS.md](./FILTER_PATTERNS.md).

## Advert rate limiter

Independent of the rules, you can rate-limit flood adverts per originating node: *each node's advert is forwarded at most once every N hours.*

- **Admitted traffic only.** The window counts adverts this repeater actually
  admitted for relay, not every advert it heard. An advert refused afterwards
  (by `set off`, a hop limit, an unknown region, loop detection or the battery
  gate) does not use up the node's budget, so turning forwarding back on does
  not leave it invisible for the rest of the window. An advert queued for
  transmission that never reaches the air still counts as admitted; already
  queued relays are not cancelled by turning the filter or battery gate off.
- **Window.** 0 (off) to 720 hours (30 days), with minute-granular edges: an
  advert can be re-admitted up to a minute before the full N hours elapse. Use
  it on a well-connected repeater to stop re-flooding periodic adverts while
  still passing each node once per window.

```text
filter ratelimit advert 48     # each origin forwarded at most every 48 h
filter ratelimit clear         # optional: forget history, start clean
filter ratelimit               # show current window and cache usage
```

## Command reference

All commands begin with `filter`. They work identically over serial and remote admin (see [Managing the repeater remotely](#managing-the-repeater-remotely)).

| Command | Effect |
|---|---|
| `filter` | Status line, e.g. `on; rules 1/32; chans 2/32; ratelimit advert 0h; cache 0/512; limiter 0; aborted 0`: filter on/off, rules used out of 32, channels stored out of 32, advert rate-limit window and cache fill, limiter and regex-abort counters |
| `filter on` / `filter off` | Enable/disable the whole filter (rules are kept) |
| `filter add <cond>=<val> ...` | Add a rule (space-separated conditions; see [What you can match on](#what-you-can-match-on)) |
| `filter list [<start-idx>]` | One readable line per rule, e.g. `0: drop advert`; a listing cut for space ends with a `...` line: re-run with the index it implies (see below). State and rule count come from `filter`, per-rule counters from `filter stats` |
| `filter stats` | Totals: limiter drops, regex aborts, airtime saved with percentage, and a per-rule hit count |
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
| `filter ratelimit advert <hours>` | Set the window (0-720 h; 0 = off) |
| `filter ratelimit clear` | Empty the advert cache |
| anything else | Usage line listing the commands |

**Reading `filter list` output.** Each line is one rule, written as you would
type it after `filter add` (minus the verb), numbered by index:

```text
0: drop advert
1: forward chan=#weather
2: disabled drop chan=#test sender=^bob throttle=60
```

The number before `: ` is the index, then the action (`drop`/`forward`) plus its
conditions, in evaluation order. An enabled rule says nothing about its state;
enabled is the default, while a disabled rule prints `disabled`.

**Markers and counters.** `filter get <idx>` shows the same line for one rule
plus its counters (`hits=`, `pass=` when `throttle=` is set, and `air=`).
`filter list` never shows counters: use `filter` for on/off state and rule
count, and `filter stats` for per-rule hits. A rule too large for one line is
cut at a condition boundary and marked `..`; `filter get <idx>` shows more of it.
When a listing will not fit one reply it ends with a line containing only `...`
(three dots); run `filter list <last-index + 1>` to continue. With no argument,
`filter list` starts at rule 0.

**Indexes.** Commands that take a rule number want the index, the number before
the `:` in `filter list`, not the whole line. Indexes count from **0**: the first
rule is rule 0, the second rule 1, and so on. The repeater prints the index when
you add a rule (`OK - rule 0 added`), so `filter get 0` is correct and
`filter get 0: drop advert` is not. Deleting a rule shifts later rules down:
delete rule 1 of 3 and old rules 2 and 3 become 1 and 2. Re-check `filter list`
after deletions.

**Inspection, not input.** `filter get <idx>` is inspection output, not `add`
input: `add` rejects the `hits=`, `air=` and `pass=` metadata it prints. The
repeater replies with short lines; `filter get` gives the most detail about a
rule. Counters and per-rule rate state reset to zero on reboot, so each
`throttle=` rule grants one free pass after a reboot; the rules do not.

**Listing format change.** `filter list` and `filter get` changed shape in
2026-10: the packed `<idx><e|d><D|F><digest>` listing and the `r0 en`/`r0 dis`
prefix are gone, and a rule's digest is no longer printed anywhere. Indexes are
unchanged, so anything that only passes an index keeps working.

### Reading `filter stats`

`filter stats` reports limiter drops (`lim:`), regex aborts (`abort:`), estimated saved airtime (`air:<seconds>s:<percent>%`) and per-rule hits (`hits: <idx>:<count>`):

```text
filter stats           # e.g. lim:118 abort:0 air:214s:36%; hits: 0:42
```

**Airtime estimate.** `air` is an estimate of the time-on-air the dropped
packets would have consumed had they been relayed (rule drops and rate-limiter
drops). Seconds are rounded down from the repeater's millisecond estimate
applied to the received packet length. It is neither a measurement of the
channel nor a count of confirmed transmissions: treat it as a comparison
between "with the rule" and "without it", not as channel utilisation.

**Per-rule share.** `filter get <idx>` shows it as `air=<seconds>s`. A shadow
`forward` probe bills nothing (`air=0s`, since its packets are still relayed);
flip it to `drop` and those hits start accumulating the estimate.

**Percentage.** Saved airtime divided by total estimated airtime evaluated by
the enabled filter, rounded to the nearest whole percent. It includes rule and
advert-limiter drops and is airtime-weighted, not packet-weighted; with no
evaluated airtime it shows `0%`. It measures the filter's estimated savings, not
actual transmitted airtime or channel utilisation, because allowed packets may
still be blocked by other forwarding checks.

**Upper bound.** A packet the filter drops is billed whether or not this
repeater would have relayed it anyway (flood hop limit, forwarding disabled,
loop detection). The filter is asked first, so it cannot know what the
repeater's own checks would have done. All airtime counters are RAM-only and
reset on reboot or a statistics reset.
## Examples for every condition

Complete, independent commands. Anything not specified is unrestricted. Rules default to dropping; add `action=forward` to count matches first without enforcing them (see Common setups below). Lines starting with `#` are comments, do not send them to the repeater.

Packet types. Names can be comma-combined, `type=any` (or omitting `type`) covers every packet type, and an advert does not match `type=txt,data`:

```text
filter add type=advert
filter add type=txt
filter add type=data route=flood
filter add type=txt,data snr=[*,-8.5]
filter add type=req
filter add type=response
filter add type=anonreq
filter add type=msg
filter add type=ack
filter add type=req,anonreq
filter add type=path
filter add type=trace
filter add type=control
filter add type=raw
filter add type=multipart
```

Most payloads the repeater relays are **encrypted end-to-end**: `req`, `response`, `anonreq` and `msg` packets carry no readable sender or text. `sender=`, `text=` and `chan=` cannot match them; the repeater rejects such a rule outright (`Err - sender=/text=/chan= only match group traffic`) instead of accepting one that could never fire. Envelope predicates (`type`, `route`, `hops`, `len`, `snr`, `path`, `hsize`, `region`) all work normally. The filter only sees traffic this repeater would relay: direct-routed ACKs, answered requests and delivered messages are consumed by the endpoints, so `type=ack` mostly sees flood ACKs, and a TRACE that reached its destination or a zero-hop CONTROL is consumed locally, so those rules mostly see *relayed* trace/control traffic.

```text
# route: route=direct with hops or region gives a rule that never matches
filter add route=flood
filter add route=direct
filter add route=flood len=[180,*]

# hops: direct packets never match, even with hops=[0,*]
filter add hops=0
filter add hops=3
filter add chan=#wardriving hops=[2,*]
filter add hops=(2,5)

# len: payload bytes, not displayed characters or the whole radio frame
filter add len=100
filter add len=[180,*]
filter add type=txt len=[*,80]

# snr: measured at your repeater; probe before you drop weak traffic
filter add snr=[*,-8.5]
filter add snr=(-8.5,0]
filter add type=advert snr=[5,*]

# path: own syntax, ID prefixes separated by >, anchored with ^ and/or $
filter add route=flood path=A1
filter add route=flood path=^A1
filter add route=flood path=A1$
filter add route=flood path=A1>B2
filter add route=flood path=^A1>B2$
filter add route=flood path=^A1$

# hsize: neither a hop count nor the channel tag size
filter add hsize=1
filter add hsize=1,2

# chan: chan alone matches group data too; sender/text restrict to group text
filter add chan=Public
filter add chan=#noisy
filter add chan=#local,#weather
filter add chan=#test sender=^Bot$ text=^BEACON

# chanhash: exactly two hex digits, no 0x, one tag per rule
filter add chanhash=7A
filter add type=txt chanhash=7A

# region: names must already exist (region def ...); region=* means unscoped only
filter add region=unscoped
filter add region=Foo
filter add region=Foo,Bar
filter add chan=#local region=Foo,unscoped

# sender: match the name without its colon
filter add sender=SpamBot
filter add sender=^SpamBot$
filter add chan=#test sender=^Bot[0-9]+$
filter add sender="^Test Bot$"
filter add sender=^Alice$\|^Bob$

# text: match the message only
filter add text=BEACON
filter add text=^BEACON
filter add text=^BEACON$
filter add text="^RX in place"
filter add chan=#test text=^ID:\s*\d\d\d$
filter add text=^PING$\|^PONG$

# prob: dose traffic instead of cutting it off
filter add chan=#chat sender="^Bot" prob=50
filter add hsize=1 prob=75
filter add chan=#auction prob=70
filter add type=advert action=forward prob=10

# throttle: one pass per N seconds, rest are dropped
filter add chan=#test sender="^bob" throttle=60
filter add sender="^Bot" throttle=10
filter add type=data throttle=600
filter add type=advert region=XX throttle=3600
filter add chan=#test sender="^bob" action=forward throttle=60
```

`path` details: for the path `10>A1>B2>30`, `A1>B2` matches, but `^A1>B2` and `A1>B2$` do not, and `A1>30` never matches because the entries are not adjacent. ID prefixes can collide; add `hsize=3,4` if you need the full three bytes compared. `path` only ever matches **flood** traffic: a direct-routed packet carries an *itinerary* between its endpoints rather than the list of repeaters that relayed it, and a TRACE packet's path holds collected SNR values, so `route=direct` packets never satisfy `path=` even when their path bytes would match. The `text=^ID:\s*\d\d\d$` example matches `ID:123` and `ID: 042`, but not `ID:12` or `ID:1234`. The region examples assume `Foo` and `Bar` already exist on the repeater; direct traffic never matches any region condition.

## Common setups

1. **Silence one channel on a shared repeater.** A `#memes` channel is eating airtime; forward everything else:

   ```text
   filter add chan=#memes
   filter on
   ```

2. **Keep local traffic local (hop gating).** Carry `#wardriving` but do not relay it across the wider mesh once it has hopped twice:

   ```text
   filter add chan=#wardriving hops=[2,*]
   ```

   Zero or one hop (originators and first relays) still passes.

3. **Block a bot by name.** Scope the rule to the channels where it is a problem:

   ```text
   filter add chan=#local,#weather sender=^BotName$

   # match anywhere in the name instead of exactly:
   filter add chan=#local,#weather sender=BotName

   # match the sender AND the message text:
   filter add chan=#local sender=^BotName$ text="^RX in place"
   ```

4. **Slow a sender instead of muting them.** Let one message per minute through and drop the rest:

   ```text
   filter add chan=#test sender="^bob" throttle=60
   filter get 0
   # drop chan=#test sender=^bob throttle=60 hits=5 pass=12 air=2s
   ```

   `pass=12` messages got through, `hits=5` excess messages were dropped, and `air=2s` is the retransmit airtime those drops saved. Like every `sender=` rule, this matches the message *text*, not a verified identity: a renamed sender evades it.

5. **Cut advert noise.** Apply the per-origin rate limiter:

   ```text
   filter ratelimit advert 48
   ```

6. **Isolate one upstream repeater.** Drop anything whose recorded flood path *starts* with repeater `A1B2C3`:

   ```text
   filter add route=flood path=^A1B2C3

   # only adverts it relayed:
   filter add type=advert route=flood path=^A1B2C3
   ```

   The first path entry is the earliest recorded relay, not necessarily your immediate neighbour. To match the *most recent* relay instead, use `path=A1B2C3$`. IDs can be written with 2, 4, 6, or 8 hex digits per entry; chain up to four with `>`, and use `^`/`$` to anchor the first/last entry.

7. **Keep unwanted regions off a channel.** Forbid plain, scope-less flood traffic, or exclude a named region:

   ```text
   filter add chan=#mychan region=unscoped
   filter add chan=#mychan region=Foo
   ```

   Region names must already exist on the repeater (`region def ...`). Direct packets have no region and are never affected.

8. **Allowlist within a channel.** Drop everything on `#foo` except Alice, by putting the exception above the catch-all drop:

   ```text
   filter add chan=#foo sender=^Alice$ action=forward   # rule 0: Alice passes
   filter add chan=#foo                                 # rule 1: everyone else is dropped
   ```

   Added the exception after the drop? Reorder without retyping it: `filter move <from> <to>` moves a rule so it ends up **at** the target index:

   ```text
   filter list                    # rule 0 is the catch-all drop, rule 1 Alice's forward
   filter move 1 0                # Alice's rule ends up at index 0, above the drop
   ```

9. **Turn a rule off temporarily.** Disable rather than delete, so you can re-enable it later:

   ```text
   filter disable 2     # keep rule 2 but skip it
   filter enable 2      # put it back
   ```

10. **Try a rule before enforcing it (shadow mode).** Add it with `action=forward`: matching packets are counted but still forwarded. Watch the counters, then enforce:

    ```text
    filter add chan=#test action=forward
    filter stats           # hit counter grows; packets still forwarded
    # when satisfied, replace it with an enforcing rule:
    filter del 0
    filter add chan=#test
    ```

    Put the `forward` probe immediately **before** the drop rule it shadows: it counts exactly what the drop would catch and, while it is active, first match wins means the drop never runs. Flip the probe's action to `drop`, or delete it, when satisfied. A `forward` probe with `throttle=` counts exactly the excess rate; keep only the probe while measuring, since probe and drop twin meter their own budgets. To log what passes, put a catch-all `forward` rule at the **end** of the list, e.g. `filter add type=txt action=forward`, so it tallies surviving traffic without short-circuiting anything. Do not leave an overlapping `forward` probe in place after adding the real drop rule: the earlier rule matches first and silently disarms the drop. `forward` rules do not stop the [rate limiter](#advert-rate-limiter) from acting.

## Managing the repeater remotely

Everything above works over the mesh. From a companion device that has the repeater as a contact:

1. Log in with the admin password (once per session).
2. Send `filter ...` commands as CLI messages to the repeater contact.

Example with `meshcore_py` over a companion TCP port:

```python
await mc.commands.send_login_sync(rep, ADMIN_PASSWORD, min_timeout=30)
await mc.start_auto_message_fetching()
await mc.commands.send_cmd(rep, "filter add chan=#wardriving hops=[2,*]", dst_type=2)
await mc.commands.send_cmd(rep, "filter stats", dst_type=2)
```

This is the normal management path for a repeater on a tower: the serial port is only needed for initial flashing and emergencies. For scheduled jobs and tags across many repeaters, see [FLEET.md](./FLEET.md).

## Interactions

- The battery gate can suspend forwarding; suspension skips content rules. Check `battery` on this node if a rule appears not to run.
- The advert rate limiter and `forward` rules are independent: a `forward` rule does not stop the limiter from dropping an advert.
- With every fork feature off, the repeater follows stock firmware behaviour. `filter off` branches every gate to that behaviour.

## Limits

| Limit | Value |
|---|---|
| Rules | 32 |
| Channels in the store | 32 (names up to 15 characters) |
| Sender pattern length | 31 characters |
| Text pattern length | 63 characters |
| Alternatives per pattern | 8 (`\|`-separated) |
| Advert rate-limit window | 0-720 hours (0 = off, 30 days at maximum) |
| CLI reply length | one short line, 157-byte reply bound; use `filter get <idx>` for detail |

**Patterns.** Long or complex patterns are rejected with an error, never
silently shortened. Length counts the whole pattern, including `|` and anchors:
`sender=` fits about four short exact names (`^Alice$|^Bob$|^Carol$|^Dan$` is 28
of 31 characters) and `text=` about eight. Add a rule, or use a broader
alternative such as `^Bot`.

**Runtime state.** Rules, channels and settings survive reboots. Counters, the
advert cache and airtime counters are RAM-only and start fresh. Windows use the
repeater's own uptime, so they stay correct across the ~49.7-day wrap of a 32-bit
millisecond counter.

**Saving.** An edit is written about 3 seconds later; a burst of commands costs
one write. Power off inside that window loses only the last edit. The save is
staged: scratch copy, read-back check, previous config kept as backup, then swap.
If the main config fails to load, the backup is used and repaired on the next
save. A power cut inside the filesystem's own write can still damage storage.

**Pattern cost.** Complex patterns match slowly, and alternatives multiply the
cost. Prefer short distinctive patterns such as `^BEACON`. A pattern that gives
up (`aborted` in `filter stats`) makes only that rule not match; later rules,
including a plain drop, still decide the packet.

**Length predicates.** A `len=` above the radio's largest packet is valid
syntax that can never match, because the payload is bounded by the packet
buffer.

**Rule order.** First match wins over the whole ordered list; packet-level and
content conditions share that list. Older fork firmware ran content rules in a
second pass, so with interleaved configs a `forward` or content rule now shields
later drops, and a packet-level rule listed first now decides first for
decrypted group traffic. `filter list` order is now honoured exactly. `forward`
rules on decrypted group traffic count their hit when the message is read;
counters are RAM-only and self-heal on reboot.

**Compatibility.** The stored action byte did not change with the `logonly` to
`forward` rename, so old and new firmware read the same value. That is format
compatibility, not identical behaviour: two-pass firmware could decide
differently. Check outcomes on the firmware you run. From 2026-10, an unescaped
`|` means OR instead of a literal pipe; write `\|` for a literal pipe. A bare
quantifier (`*Bot`) or misplaced anchor (`A^B`) is still accepted but now
matches nothing.

**Not authentication.** Rules match what a packet says, not who sent it: sender
names are chosen freely, channel keys are shared, and the protocol tag is two
bytes. A `sender=` or `chan=` rule is a content policy, not an identity check.
Do not treat it as a security boundary.

**Remote admin.** Management requests addressed to this repeater are answered
directly, so your filter and the battery gate never block logging in.
Intermediate repeaters can still drop the reply, so before a broad drop rule add
a higher-priority exception (`filter add chan=<admin channel> action=forward`)
and test from the app while you have serial access. This is about the relay
path, not login authorisation.

The binary `/filter_cfg` layout, record sizes, CRC, version migration and staged-save mechanics are a tooling-only reference in [FILTER_CONFIG_FORMAT.md](./FILTER_CONFIG_FORMAT.md).
