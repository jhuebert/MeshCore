# Repeater Packet Filter — User Guide

A remote-configurable packet filter for the `simple_repeater` firmware. Rules can be
added, inspected, and removed over the repeater CLI (locally via serial, or remotely
over the mesh by an authenticated admin) — no rebuild or physical access required.

---

## 1. Overview

The filter evaluates every packet offered to the repeater and decides whether to
**forward** it, **drop** it, or (in shadow mode) forward it while still counting the
match. The guiding model:

- A **rule** is a conjunction of optional predicates. A packet is dropped if any
  *enabled* rule whose predicates all match is hit. **First match wins** — later
  rules are not evaluated for that packet.
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

Each rule holds:

| Field | Meaning |
|---|---|
| `enabled` | Disabled rules are skipped entirely |
| `action` | `drop` (default) or `logonly` |
| `type` | Payload-type mask: `advert`, `txt`, `data`, or any combination |
| `route` | `flood` and/or `direct` |
| `hops` | Flood path length (number of path hashes); direct traffic never matches |
| `len` | Payload length in bytes |
| `snr` | Received SNR in dB (signed, quarter-dB resolution) |
| `path` | Sliding-window match of 1–4 adjacent repeater pubkey-hash prefixes in the flood path |
| `hsize` | Path hash size (1–4 bytes) used by the packet |
| `chan` | Keyed channel match (identity proven by decryption) |
| `chanhash` | Single 1-byte on-air channel hash |
| `region` | Comma list of flood regions (or `unscoped`); direct traffic never matches |
| `sender` | Regex over the message sender (`"<sender>:"`), group text only |
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

**Regexes.** Sender and text patterns are unanchored by default (`TestBot`
matches *contains*); use `^...$` to anchor. Invalid patterns are rejected when
the rule is added. Each evaluation runs under a step budget; if the budget is
exhausted the match **fails open** (the packet is forwarded) and the `aborted`
counter is incremented — a pathological pattern can never wedge the repeater,
worst case is one spam message repeated.

Patterns are matched **byte-wise** on the raw UTF-8 payload: a literal emoji or
other non-ASCII character in a pattern matches that exact character (e.g.
`sender=^John😀$`), but `.` and character classes cover single bytes, not whole
characters — one emoji is 4 bytes, so `J.hn` will not match `J😀hn` (use
`J....hn` or a literal).

**Limits** (per-rule / global capacities worth knowing when designing rules):

| Limit | Default |
|---|---|
| Rules | 16 |
| Keyed channels | 16 |
| Channel name length | 16 chars |
| Sender regex length | 24 chars |
| Text regex length | 48 chars |
| Path hashes per rule | 4 |
| Region list length | 32 chars |
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

Space-separated `key=value` tokens. Multiple values inside one key use commas
(`type=advert,txt`, `chan=#a,#b`, `hsize=1,2`).

| Key | Values | Matches |
|---|---|---|
| `type` | `advert` `txt` `data` (comma-combine) | Payload type(s) |
| `route` | `flood` \| `direct` | Routing class of the packet |
| `hops` | interval | Flood hop count (see interval syntax, §2) |
| `len` | interval | Payload length, bytes |
| `snr` | interval, signed dB | Packet SNR (e.g. `snr=[-100,-5]`, `snr=-2.25`) |
| `path` | `[^]HEX>HEX>…[$]` | Adjacent flood-path hash prefixes; `^` anchors first, `$` anchors last; 2–8 hex chars per entry, up to 4 entries |
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

### Examples

```text
# Drop everything on a hashtag channel
filter add chan=#noisy action=drop

# Anti-wardriving: drop flood traffic on a channel once it has travelled
filter add chan=#wardriving hops=[2,*] action=drop

# Shadow a suspected spammer without affecting traffic yet
filter add sender=^SpamBot$ action=logonly

# Drop all adverts relayed from one specific upstream repeater (path prefix)
filter add type=advert path=^A1B2C3 action=drop

# Drop group text on Public whose text looks like a flood of test beacons
filter add chan=Public text=^BEACON action=drop

# Damp Direct-of-motion chatter: only very long payloads on flood routes
filter add route=flood len=[180,*] action=drop

# Reject weak fringe links for forwarded group data
filter add type=txt,data snr=[*,-8.5] action=drop
```

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

One neighbouring repeater (pubkey-hash prefix `A1B2C3`) keeps injecting junk into
flood traffic. Drop anything whose path chain *starts* with it:

```text
filter add path=^A1B2C3 action=drop
```

Or only adverts it relayed:

```text
filter add type=advert path=^A1B2C3 action=drop
```

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

- **First match wins** — put more specific rules before broader ones.
- Content rules can only match traffic the repeater can actually decrypt; a keyed
  channel rule doubles as an implicit "decrypt this channel" registration.
- `sender`/`text` predicates apply to group text payloads only; a matching rule
  with these predicates never fires on binary group data.
- `chanhash` is a convenience for key-less cases; prefer keyed `chan` matching.
- Regex evaluations are step-budgeted and fail open (see §2); watch the `aborted`
  counter if you use heavy patterns like `.*.*.*`.
- All counters (`hits`, `limiter`, `aborted`) and the advert cache are memory-only:
  they reset on reboot, and the cache can also be emptied via `filter ratelimit clear`.
- Reply strings are capped at 160 bytes (remote CLI buffer); `filter list` shows a
  short digest per rule and `filter get <idx>` the full detail.
