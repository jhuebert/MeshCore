# Ham Mode: User Guide

Ham mode lets a licensed amateur radio operator send traffic that anyone can read at amateur power, while encrypted traffic stays at a lower unlicensed power. The repeater picks the power for each packet just before it transmits. Packet contents and on-air formats are unchanged, so stock nodes and apps see nothing different. Ham mode is off by default: a fresh install, or any failure loading its config, behaves like stock firmware.

## What it does

There are two transmit powers:

- `set tx` is the normal, unlicensed power. Every encrypted packet uses it.
- `ham tx` is the amateur power. Only publicly readable packets use it, and only while ham mode is active.

A packet is publicly readable only when this firmware can positively identify it as such. Everything else gets `set tx`. Group classification uses the protocol's one-byte channel hash and two-byte MAC. Both must match, but these short fields cannot rule out a collision.

| Public (uses `ham tx`) | Encrypted (uses `set tx`) |
|---|---|
| Adverts (signed plaintext, carries the station ID) | Direct messages, requests and responses |
| ACKs (a checksum, no message content) | Anonymous requests, returned paths, raw custom packets, unknown types |
| Traces and control packets (plaintext) | Group traffic that does not match a known public key |
| Multipart ACKs (low nibble is ACK) | Non-ACK multipart packets |
| Group text or data whose MAC verifies against the Public channel or one of your listed hashtag channels | |

Only the transmit power of each physical transmission differs. Routing, forwarding, the node's name in adverts, the advert schedule, and how other nodes or apps behave are unchanged.

## Before you enable it

- You must hold an amateur radio license and operate within its privileges. This firmware is a tool, not a license and not legal advice.
- Amateur privileges do not allow obscuring the meaning of messages (47 CFR 97.113(a)(4)). Ham mode therefore gives higher power only to traffic anyone can read. Encrypted traffic always stays at `set tx`.
- FCC 97.119(a) requires your call sign to be transmitted at least every 10 minutes during a communication. This firmware schedules an ID on activation and on a 10-minute timer. The call sign travels in the node name. Check it on a receiver: the scheduler cannot guarantee when a packet airs.
- Part 15 devices are limited to 1 W (30 dBm) conducted output, with a dB-for-dB reduction above 6 dBi of antenna gain (47 CFR 15.247(b)(3)-(4)). A high-gain antenna can push radiated power over that limit on any board, not just an amplified one. On boards with a power amplifier, the `set tx` number is often a drive level rather than measured output; Station G2 and G3 use a `LORA_TX_POWER` of 7 dBm to drive the PA. This fork enforces no ceiling and claims no compliance.

**Note:** You are responsible for the power levels you set and for identifying your station on air.

## Quick start

Run these commands over serial or as an admin over the mesh. The powers are examples: substitute drive levels appropriate for your board and antenna. In particular, 22 dBm exceeds the Station G2's 19 dBm maximum drive.

```
ham callsign N0CALL
set name N0CALL Test
set tx 7
ham tx 22
ham on
```

`ham` now reports an active state like:

```
on - call N0CALL, ham tx 22, set tx 7, tags 0, next ID 0s
```

The `next ID` value counts down in seconds. It is 0 while the first ID is due, then about 598s once that advert has been queued (10 minutes less the 1.5 s send delay). The first ID is eligible for transmission after that delay.

## Behaviour

**Callsign gate.** Ham mode is active only when all three are true: you ran `ham on`, a callsign is set, and that callsign appears as a whole word in the live node name. Whole word means the characters around it are not letters or digits, so `N0CALL` matches `N0CALL Test` but not `N0CALLS Test`. Matching is case insensitive. `set name` takes effect immediately because the current name is checked on every use. The whole callsign must fit in the transmitted UTF-8 name: 31 bytes without position, 23 with position. Put the callsign first if the full name contains it but `ham` reports `waiting`.

**Power switching.** When active, public packets use `ham tx` and encrypted packets use the live `set tx`. Changing `set tx` immediately changes the power of encrypted traffic, even while ham mode is on; public traffic keeps using `ham tx`. While ham mode is off or waiting, every packet uses `set tx`.

**Station ID.** Activation makes the first ID due immediately. Creating a local advert, including a scheduled advert or `advert.zerohop`, restarts the timer. This happens before transmission: other queued traffic can go first, a busy radio can delay the ID, and a failed transmission can leave a gap longer than 10 minutes. Relaying someone else's advert does not restart the timer. The ID is an ordinary signed zero-hop advert with your callsign in the name, so it feeds mesh clock sync like any other advert.

**Hashtag channels.** Add a publicly known channel with `ham channel add #name`. The name must match the app exactly, including case. It is at most 14 characters after the `#`, the same limit MeshCore puts on hashtag channels. An unlisted hashtag is treated as encrypted and gets `set tx`.

## Command reference

All commands take the `ham` prefix. Replies are bounded to 157 bytes.

Usage, printed for `ham help` and for an unknown or malformed command:

```
Err - usage: ham | ham on | ham off | ham callsign [CALL] | ham tx [N] | ham channel add|remove|list [#name] | ham channels | ham stats
```

`ham` / `ham status` reports the current state. Active:

```
on - call N0CALL, ham tx 22, set tx 7, tags 2, next ID 598s
```

Off: `off - all packets use normal tx power`. Waiting: `waiting - no callsign` or `waiting - node name must include N0CALL`.

`ham on` / `ham off` enables or disables ham mode: `OK - ham on` or `OK - ham off`. `ham on` stores the enabled flag; activation still needs a valid callsign in the node name.

`ham callsign [CALL]` with no value shows `callsign is N0CALL` or `no callsign set`. With a value it stores it uppercased. The value must be one word of 3 to 15 characters:

```
OK - callsign set to N0CALL
Err - callsign must be one word, 3..15 chars
```

The reply adds a warning when the node name is over 23 bytes, the name budget in adverts that include position.

`ham tx [N]` shows or sets the power for publicly readable traffic, from -9 to 30 dBm:

```
ham tx is 22 dBm
OK - ham tx set to 22 dBm
Err - ham tx must be a number from -9 to 30
```

This never changes `set tx`, the power for encrypted traffic.

`ham channel add|remove|list [#name]`: `add` takes one word, adds a leading `#` if needed, rejects duplicates and allows at most 8 names. `remove` takes the exact name. `list` shows `Public` first, then your names. `ham channels` is an alias for `ham channel list`.

```
OK - #test now uses ham power
OK - #test removed
Public, #test, #Nebraska
Err - channel name must be one word, max 14 characters after #
Err - #test is already on the list
Err - #test is not on the list
Err - channel list full (max 8); remove one first
```

`ham stats` shows RAM-only counts of packets classified for transmission since boot, plus the last driver-confirmed power setting (`unknown` after a failed power write) and ID countdown. These are attempts, not confirmed transmissions; the power setting is not a measurement of radiated power.

```
12 public, 34 private; applied 7 dBm; next ID 598s
```

## Interactions with other features

- **Filter:** fully decoupled. Ham mode has its own channel list and never reads `/filter_cfg`; the filter does not know ham mode exists. You may list the same hashtag in both, but each list is edited separately.
- **radio2:** one power pair covers both presets. radio2 has no per-preset power, and ham mode adds none.
- **RXPS:** allowed. radio2 and RXPS remain mutually exclusive.
- **Battery gate:** independent. Ham ID adverts still fire while forwarding is suspended, so disable ham mode on battery-critical nodes if you do not want that RF activity.
- **Fleet:** fleet traffic and its acknowledgements are group texts encrypted with the fleet channel's private PSK. Ham classification checks group traffic against the Public channel and your listed hashtag channels, so neither fleet datagrams nor fleet ACKs match and both use `set tx`.
- **Observer:** capture happens before forwarding policy and only TX power changes, so there is no schema change.
- **OTA:** update operations ride inside requests and responses, so they are encrypted and use `set tx`. A low private power slows updates.
- **Mesh clock:** ID adverts are ordinary signed adverts and feed clock sync like any other.

## Persistence and defaults

Settings live in `/ham_cfg`. If the main record is missing or invalid, a valid backup can restore its settings, including `on`. With no usable record the defaults are: ham mode off, no callsign, `ham tx` at the board's `LORA_TX_POWER`, and no tags. Counters and the ID timer are RAM-only and reset on boot.

This is a new format. Settings from the source fork's `/ham_prefs` are not read or migrated; a node coming from that firmware starts with ham mode off and you must re-enter its settings here.

## Troubleshooting

- `ham` says `waiting - no callsign`: run `ham callsign <CALL>`.
- `ham` says `waiting - node name must include <CALL>`: run `set name <CALL> Test` with the same callsign as a whole word.
- Wrong power level: remember that `ham tx` is public power and `set tx` is private power. `ham` shows both. A public message and a direct message should show two distinct levels on a meter or SDR.
- A hashtag message still uses private power: the name must be listed with `ham channel add`, match exactly including case, and the sender must use the same channel key.
- To verify: check that `ham` says `on`, watch for a zero-hop advert after activation, send one public and one direct message, and confirm `ham stats` increments both counters.

## Acknowledgements

This feature adapts the ham capability built and published by **KF0WXX and the
Nebraska Mesh community** in
[NebraskaMesh/MeshCore-Ham](https://github.com/NebraskaMesh/MeshCore-Ham)
(MIT licensed), whose original implementation is
[commit `6e716038`](https://github.com/NebraskaMesh/MeshCore-Ham/commit/6e716038).
The public/private classification
model and its fail-safe rule, the hardcoded Public key and hashtag derivation,
the activation gate, the per-packet TX power hook, the command names and the
public/private counters all come from that work. This fork rewrites it into the
repeater-filter architecture with fork-owned files, CRC-protected settings,
native tests, whole-word callsign validation and a timer-driven station ID.
