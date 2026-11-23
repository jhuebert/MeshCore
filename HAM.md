# Ham mode: User Guide

Ham mode lets a licensed amateur radio operator send traffic that anyone can
read at amateur power, while encrypted traffic stays at a lower unlicensed
power. The repeater chooses the power for each packet just before it transmits.
Packets are not changed, so stock nodes and apps see nothing different. Ham
mode is **off by default**: a fresh install, or any failure loading its config,
behaves like stock repeater firmware.

## What it does

There are two transmit powers:

- `set tx` is the normal, unlicensed power. Every encrypted packet uses it.
- `ham tx` is the amateur power. Only publicly readable packets use it, and
  only while ham mode is active.

A packet is publicly readable only when this firmware can positively identify
it as such. Everything else is treated as encrypted and gets `set tx`. This
fail-safe rule means a mistake can never send private traffic at amateur power.

| Public (uses `ham tx`) | Encrypted (uses `set tx`) |
|---|---|
| Adverts (signed plaintext, carries the station ID) | Direct messages, requests and responses |
| ACKs (a checksum, no message content) | Anonymous requests, returned paths, raw custom packets, unknown types |
| Traces and control packets (plaintext) | Group traffic that does not match a known public key |
| Multipart ACKs (low nibble is ACK) | Non-ACK multipart packets |
| Group text or data whose MAC verifies against the Public channel or one of your listed hashtag channels | |

What does **not** change: packet contents, on-air formats, the node's name in
adverts, the normal advert schedule, routing and forwarding, and how other
nodes or apps behave. Only the transmit power of each physical transmission
differs. With ham mode off, no power switching happens at all.

## Before you enable it

- You must hold an amateur radio license and operate within its privileges.
  This firmware is a tool, not a license and not legal advice.
- Amateur privileges do not allow obscuring the meaning of messages
  (47 CFR 97.113(a)(4)). Ham mode therefore gives higher power only to
  traffic anyone can read. Encrypted traffic always stays at `set tx`.
- FCC 97.119(a) requires your call sign to be transmitted at least every
  10 minutes during a communication. This firmware sends your call sign in a
  zero-hop advert when ham mode activates and at least every 10 minutes after
  that. The call sign is the node name, so set the name to include it.
- Part 15 devices are limited to 1 W (30 dBm) conducted output, with a
  dB-for-dB reduction above 6 dBi of antenna gain (47 CFR 15.247(b)(3)-(4)).
  A high-gain antenna can push radiated power over that limit on any board,
  not just an amplified one. On boards with a power amplifier, the `set tx`
  number is often a drive level rather than measured output; Station G2 and G3
  use a `LORA_TX_POWER` of 7 dBm to drive the PA. Measure your real output with
  your actual antenna and choose both powers yourself. This fork enforces no
  ceiling and claims no compliance.

## Quick start

From a fresh install, over serial or as an admin over the mesh:

```
ham callsign N0CALL
set name N0CALL Test
set tx 7
ham tx 22
ham on
```

`ham` should now report:

```
on - call N0CALL, ham tx 22, set tx 7, tags 0, next ID 300s
```

The `next ID` value counts down from 10 minutes. A zero-hop advert carrying
`N0CALL` goes out about 1.5 seconds after `ham on`.

## How it behaves

**Callsign gate.** Ham mode is active only when all three are true: you ran
`ham on`, a callsign is set, and that callsign appears as a whole word in the
live node name. Whole word means the characters around it are not letters or
digits, so `N0CALL` matches `N0CALL Test` but not `N0CALLS Test`. Matching is
case insensitive. `set name` takes effect immediately because the current name
is checked on every use.

**Three status words.** `ham` reports `on` when active, `off` when ham mode is
disabled, or `waiting` when it is enabled but not yet gated on. `waiting`
either says `no callsign` or tells you which callsign the node name must
include. While off or waiting, every packet uses `set tx`.

**Dynamic power switching.** When active, public packets use `ham tx` and
encrypted packets use the live `set tx`. Changing `set tx` immediately changes
the power of encrypted traffic, even while ham mode is on; public traffic keeps
using `ham tx`. The radio is only reprogrammed when the chosen power actually
changes, which avoids needless duty-cycle churn when RXPS is armed.

**Station ID.** Activation arms a timer immediately: the first ID advert
reaches the air about 1.5 seconds later. Every advert that reaches the air,
including the ID advert itself, scheduled adverts and `advert.zerohop`, pushes
the timer out another 10 minutes. The normal advert schedule is untouched. The
ID is an ordinary signed zero-hop advert with your callsign in the name, so it
feeds mesh clock sync like any other advert.

**Hashtag channels.** Add a publicly known channel with `ham channel add #name`.
The name must match the app exactly, including case. It is at most 14
characters after the `#`, the same limit MeshCore puts on hashtag channels.
An unlisted hashtag is treated as encrypted and gets `set tx`.

## Command reference

All commands take the `ham` prefix and are handled by the same dispatcher as
`filter`, `battery`, `fleet` and `radio2`. Any `ham` command arriving over the
mesh is accepted only from an admin client, via the core's `isAdmin()` check in
`MyMesh::handleCommand()`. Serial and fleet-script commands run with serial
privilege. Replies are bounded to 157 bytes.

Usage, printed for `ham help` and for an unknown or malformed command:

```
Err - usage: ham | ham on | ham off | ham callsign [CALL] | ham tx [N] | ham channel add|remove|list [#name] | ham channels | ham stats
```

### `ham` / `ham status`

Reports the current state. Active:

```
on - call N0CALL, ham tx 22, set tx 7, tags 2, next ID 300s
```

Off: `off - all packets use normal tx power`.
Waiting: `waiting - no callsign` or `waiting - node name must include N0CALL`.

### `ham on` / `ham off`

Enables or disables ham mode: `OK - ham on` or `OK - ham off`. `ham on` stores
the enabled flag; activation still needs a valid callsign in the node name.

### `ham callsign [CALL]`

With no value, shows `callsign is N0CALL` or `no callsign set`. With a value,
stores it uppercased. The value must be one word of 3 to 15 characters:

```
OK - callsign set to N0CALL
Err - callsign must be one word, 3..15 chars
```

The reply adds a warning when the node name is over 23 bytes, the name budget
in adverts that include position.

### `ham tx [N]`

Shows or sets the power for publicly readable traffic, from -9 to 30 dBm:

```
ham tx is 22 dBm
OK - ham tx set to 22 dBm
Err - ham tx must be a number from -9 to 30
```

This never changes `set tx`, the power for encrypted traffic.

### `ham channel add|remove|list [#name]`

`add` takes one word, adds a leading `#` if needed, rejects duplicates and
allows at most 8 names. `remove` takes the exact name. `list` shows `Public`
first, then your names. `ham channels` is an alias for `ham channel list`.

```
OK - #test now uses ham power
OK - #test removed
Public, #test, #Nebraska
Err - channel name must be one word, max 14 characters after #
Err - #test is already on the list
Err - #test is not on the list
Err - channel list full (max 8); remove one first
```

### `ham stats`

RAM-only counters since boot, plus the applied power and ID countdown:

```
12 public, 34 private; applied 7 dBm; next ID 300s
```

## Interactions with other features

- **Filter:** fully decoupled. Ham mode has its own channel list and never
  reads `/filter_cfg`; the filter does not know ham mode exists. You may list
  the same hashtag in both, but each list is edited separately.
- **Radio2:** one power pair covers both presets. Radio2 has no per-preset
  power, and ham mode adds none. The dispatcher selects the preset before the
  power hook runs.
- **RXPS:** allowed. Because power is only written when the class changes,
  duty-cycle re-arming is kept to a minimum. Radio2 and RXPS remain mutually
  exclusive.
- **Battery gate:** independent. Ham ID adverts still fire while forwarding is
  suspended, so disable ham mode on battery-critical nodes if you do not want
  that RF activity.
- **Fleet:** fleet traffic and its acknowledgements are group texts encrypted
  with the fleet channel's private PSK. Ham classification checks group traffic
  against the Public channel and your listed hashtag channels, so neither fleet
  datagrams nor fleet ACKs match and both use `set tx`.
- **Observer:** capture happens before forwarding policy and only TX power
  changes, so there is no schema change.
- **OTA:** update operations ride inside requests and responses, so they are
  encrypted and use `set tx`. A low private power slows updates.
- **Mesh clock:** ID adverts are ordinary signed adverts and feed clock sync
  like any other.

## Persistence and defaults

Settings live in `/ham_cfg`, a fixed 152-byte record plus a 2-byte CRC-16,
written with a staged save that keeps a `.bak` copy. A missing, corrupt,
overlong or wrong-version record leaves every default in place: ham mode off,
no callsign, `ham tx` at the board's `LORA_TX_POWER`, and no tags. Counters and
the ID timer are RAM-only and reset on boot.

This is a new format. Settings from the source fork's `/ham_prefs` are not read
or migrated; a node coming from that firmware starts with ham mode off and you
must re-enter its settings here.

## Troubleshooting

- `ham` says `waiting - no callsign`: run `ham callsign <CALL>`.
- `ham` says `waiting - node name must include <CALL>`: run
  `set name <CALL> Test` with the same callsign as a whole word.
- Wrong power level: remember that `ham tx` is public power and `set tx` is
  private power. `ham` shows both. A public message and a direct message should
  show two distinct levels on a meter or SDR.
- A hashtag message still uses private power: the name must be listed with
  `ham channel add`, match exactly including case, and the sender must use the
  same channel key.
- Reported power looks low, or the link seems too strong: on amplified boards
  like G2 and G3 the numbers are PA drive levels, not measured watts. On any
  board a high-gain antenna raises radiated power. Measure conducted output with
  the real antenna and check the antenna-gain rule before choosing either power.
- To verify: check that `ham` says `on`, watch for a zero-hop advert after
  activation, send one public and one direct message, and confirm `ham stats`
  increments both counters.

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
native tests, whole-word callsign validation and a timer-driven station ID that
closes the source fork's 10-minute compliance gap. The original contribution
stands behind all of it.

## For developers

The feature is `examples/simple_repeater/HamMode.h/.cpp` plus `HamModeConfig.h`.
Its only upstream edit is the virtual `Dispatcher::onBeforeTransmit()` hook.
Behavior is pinned by the `test/test_ham_mode/` native suite (`native_ham_mode`
and `native_ham_mode_san`). See the code map in AGENTS.md for the other fork
features and their guides.
