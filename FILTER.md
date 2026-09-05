# Repeater Packet Filter — Fork Feature

A remote-configurable packet filter for the `simple_repeater` firmware. Rules can be
added, inspected, and removed over the repeater CLI (locally via serial, or remotely
over the mesh by an authenticated admin) — no rebuild or physical access required.

This is a **personal-fork feature**. All fork-owned code lives in
`examples/simple_repeater/PacketFilter.h/.cpp`, `PacketFilterConfig.h`, and
`TinyRegex.h/.cpp`, plus minimal hook lines in `MyMesh.h/MyMesh.cpp`. No changes to
MeshCore core sources.

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
- **Rules with content predicates are evaluated on the *decrypted* payload**, so
  channel identity and sender/text patterns are cryptographically proven (MAC
  verification), not guessed from on-air bytes.

### Where rules are evaluated

| Phase | Hook | Predicates evaluated |
|---|---|---|
| Packet-level, before forwarding | `MyMesh::allowPacketForward()` | `type`, `route`, `hops`, `len`, `snr`, `path`, `hsize`, `chanhash` |
| Content-level, on decrypted group payloads | `MyMesh::onGroupDataRecv()` | keyed `chan` store, `sender`, `text` |

Rules that contain any content predicate (keyed channel, sender, text) are
*deferred*: they are skipped in the packet phase and decided only after the group
payload is decrypted and MAC-verified. All other rules decide entirely at the
packet level.

A rule that only sets packet-level predicates (e.g. `route=flood hops=[3,*]`)
applies to *all* packet types including adverts, telemetry-carrying packets, and
trace packets — use `type=` to narrow it when needed.

### Trust model for channel identity

- **Keyed channel** (`chan=#name`): matches only if the packet decrypts
  successfully with the stored key — the strongest identity proof. `#`-hashtag
  channels derive their key from the name (`sha256(name)[0..15]`), matching the
  companion protocol, so a rule for `#someday` works with zero key distribution.
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
| `snr` | Received SNR in dB (stored on a quarter-dB grid, signed) |
| `path` | Sliding-window match of 1–4 adjacent repeater pubkey-hash prefixes in the flood path |
| `hsize` | Path hash size (1–4 bytes) used by the packet |
| `chan` | Bitmask over the keyed channel store (identity proven by MAC) |
| `chanhash` | Single 1-byte on-air channel hash |
| `sender` | Regex over the message sender (`"<sender>:"`), GRP_TXT only |
| `text` | Regex over the message text, GRP_TXT only |
| `hits` | Match counter (RAM-only, never written to the config file) |

**Interval syntax** (used by `hops`, `len`, `snr`):

| Form | Meaning |
|---|---|
| `3` | exactly 3 |
| `[2,*]` | ≥ 2 (both endpoints inclusive) |
| `(2,5)` | > 2 and < 5 (both endpoints exclusive) |
| `[*,-4]` | ≤ −4 (useful for `snr`) |
| `*` | unbounded side |

**Regex engine.** Sender and text patterns are evaluated by `TinyRegex`
(`examples/simple_repeater/TinyRegex.*`), a small bounded backtracking matcher —
unanchored by default (`TestBot` matches *contains*), `^...$` to anchor. Patterns
are validated at add time and rejected rather than truncated. Each evaluation runs
under a step budget; if the budget is exhausted the match **fails open** (the
packet is forwarded) and the `aborted` counter is incremented — a pathological
pattern can never wedge the repeater, worst case is one spam message repeated.

Patterns and subjects are matched **byte-wise** on the raw UTF-8 payload: a
literal emoji or other non-ASCII character in a pattern matches that exact
character (e.g. `sender=^John😀$`), but `.` and character classes cover single
bytes, not whole characters — one emoji is 4 bytes, so `J.hn` will not match
`J😀hn` (use `J....hn` or a literal).

**Capacity defaults** (build-time tunables in `examples/simple_repeater/PacketFilterConfig.h`):

| Constant | Default | Notes |
|---|---|---|
| `FILTER_MAX_RULES` | 16 | ~119 B per rule slot |
| `FILTER_MAX_CHANNELS` | 8 | `chan` bitmask is one byte wide |
| `FILTER_CHAN_NAME_LEN` | 16 | channel name storage (NUL incl.) |
| `FILTER_ADVERT_CACHE_SIZE` | 256 | advert rate-limiter cache |
| `FILTER_SENDER_PATTERN_LEN` | 24 | sender regex length (NUL incl.) |
| `FILTER_TEXT_PATTERN_LEN` | 48 | text regex length (NUL incl.) |
| `FILTER_PATH_HASH_SLOTS` | 4 | path-chain hashes per rule |

---

## 3. Keyed channel store

The store maps names to keys and is what makes content rules possible:

- `Public` is pre-provisioned on a fresh node with the well-known Public PSK.
- `#name` channels are **derived**: secret = `sha256("#name")[0..15]`, exactly as
  companion apps derive hashtag channel keys. Add them with no PSK argument.
- Non-`#` (private) channels require an explicit base64 PSK (16 or 32 bytes).
- Referencing an unknown `#name` inside a rule (`filter add chan=#foo`) will
  auto-provision it into the store.
- Deleting a channel remaps all rule masks so they keep pointing at the same
  channels (a rule's bit for the deleted channel is dropped).

The store also feeds decryption: while the filter is enabled, group payloads
matching any stored channel hash are offered for decryption (so content rules can
fire on channels the repeater otherwise wouldn't read). With the filter off the
repeater behaves like stock firmware.

---

## 4. Advert rate limiter

Independent of the rule list, flood-route adverts can be rate-limited **per
originating node**: *each node's advert is forwarded at most once every N hours.*

- `filter ratelimit advert <hours>` sets the window (0 = off, max 720 h ≈ 30 days).
- The repeater keeps a 256-entry ring cache of origin pubkey prefixes (first 6
  bytes) with first-seen timestamps (RAM only — cleared on reboot).
- A repeated advert from a known origin inside the window is silently dropped and
  counted in `limiter`. After the window expires the origin is re-admitted and the
  window restarts.
- The limiter only runs on flood-routed adverts, and only when the rule list has
  not already dropped the packet. Logonly adverts are also subject to it.
- The cache can be emptied without changing the window
  (`filter ratelimit clear`), e.g. after a repeater has been offline for a while.

Use case: on a well-connected repeater, stop re-flooding everyone's periodic
adverts while still forwarding each node's advert once per window so it stays
reachable through you.

---

## 5. Persistence

All rules, channels, the enabled flag, and the ratelimit window are saved to
`/filter_cfg` on the repeater's filesystem (binary format, version-guarded) —
**the file holds configuration only**. Stats are not part of the file format: rule
records on disk end at the `hits` field, so counters and the advert cache exist
only in RAM and reset on reboot. Saves are lazy: 3 seconds after the last
configuration change, so bursts of CLI commands produce a single write.

---

## 6. CLI reference

All commands begin with `filter`. Replies are short (the remote CLI reply buffer is
160 bytes). Works identically over serial and remote-admin (see §8).

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
| `filter chan add <name> [<psk-b64>]` | Add a channel; PSK optional for `#` names |
| `filter chan del <name>` | Remove a channel (rule masks are remapped) |
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
| `chan` | channel name(s) | Keyed store match, MAC-proven; `#` names auto-provision |
| `chanhash` | 2 hex chars | Bare 1-byte on-air channel hash (no decryption proof) |
| `sender` | regex | Sender name in `"<sender>: <text>"` (GRP_TXT only) |
| `text` | regex | Message text (GRP_TXT only) |
| `action` | `drop` \| `logonly` | Default `drop` |

Rules containing `chan`, `sender`, or `text` are content rules (evaluated after
decryption). All other combinations are packet-level.

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

Invalid input is rejected with `Err - ...` and no half-added rule is left behind.

### Hit counters

`filter stats` shows `hits` per rule plus the limiter and regex-abort counters.
All counters are RAM-only: they start at zero after every reboot, and `filter
clear` resets them with the rules. Use `logonly` rules as long-term probes to
measure what a `drop` *would* remove before enforcing it.

---

## 7. Use-case walkthroughs

### 7.1 Silencing one channel on a shared repeater

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

### 7.2 Anti-wardriving (hop-count gating)

You operate a public repeater and want to carry local `#wardriving` traffic but
refuse to be a free relay for it across the wider mesh: anything that has already
hopped at least twice shouldn't go further.

```text
filter add chan=#wardriving hops=[2,*] action=drop
filter on
```

Two hops or more: dropped. Zero or one hop (originators and first relays): passes.

### 7.3 Bot sender blocklist (content rules)

A bot floods several channels you *can* decrypt. Match on the sender inside the
decrypted payload, scoped to the channels where it's a problem:

```text
filter chan add #local
filter chan add #weather
filter add chan=#local,#weather sender=^OBot$ action=drop

# substring match instead of exact:
filter add chan=#local,#weather sender=Mule action=drop
```

### 7.4 Shadow mode before enforcement

Before actually dropping `#test`, observe what would be caught:

```text
filter add chan=#test action=logonly
# ...later, check telemetry:
filter stats           # rule hit counter grows; packets still forwarded
# when satisfied, switch to enforcing:
filter del 0
filter add chan=#test action=drop
```

### 7.5 Cutting advert noise

Your repeater relays the same nodes' adverts every few minutes all day. Cap the
relay:

```text
filter ratelimit advert 48        # each origin forwarded at most every 48 h
filter ratelimit clear            # optional: forget history, start clean
filter ratelimit                  # ratelimit advert 48h; cache 0/256
```

### 7.6 Isolating one upstream repeater

One neighbouring repeater (pubkey-hash prefix `A1B2C3`) keeps injecting junk into
flood traffic. Drop anything whose path chain *starts* with it:

```text
filter add path=^A1B2C3 action=drop
```

Or only adverts it relayed:

```text
filter add type=advert path=^A1B2C3 action=drop
```

### 7.7 Temporary experiment without losing the production config

```text
filter add len=[200,*] action=logonly     # probe oversized payloads
# ...decide you don't need it:
filter del 3
```

Disabled vs deleted: `filter disable 2` keeps the rule for later
(`filter enable 2`), useful for A/B testing a predicate.

---

## 8. Remote administration

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

## 9. Behaviour notes and limits

- **First match wins** — put more specific rules before broader ones.
- Content rules can only match traffic the repeater can actually decrypt; a keyed
  channel rule doubles as an implicit "decrypt this channel" registration.
- `sender`/`text` predicates apply to group text payloads only; a matching rule
  with these predicates never fires on binary group data.
- `chanhash` is a convenience for key-less cases; prefer keyed `chan` matching.
- The advert cache is RAM-only and cleared on reboot (adverts are re-admitted once
  after a restart — a deliberate fail-open).
- Regex evaluations are step-budgeted and fail open (see §2); watch the `aborted`
  counter if you use heavy patterns like `.*.*.*`.
- All counters (`hits`, `limiter`, `aborted`) and the advert cache are memory-only:
  they reset on reboot (the config file does not store them) or on config wipe,
  and the cache can also be emptied via `filter ratelimit clear`.
- Reply strings are capped at 160 bytes (remote CLI buffer); `filter list` shows a
  short digest per rule and `filter get <idx>` the full detail.
