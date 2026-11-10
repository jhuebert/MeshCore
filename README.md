# MeshCore — Repeater Firmware with a Remote-Configurable Packet Filter

This is a feature fork of [MeshCore](https://github.com/meshcore-dev/MeshCore).
It tracks upstream automatically (synced weekly) and adds seven
repeater-focused capabilities on top of stock firmware. Everything else —
supported boards, clients, protocols, build system — is identical to upstream;
see the
[upstream README](https://github.com/meshcore-dev/MeshCore#readme) for the
general project, hardware support, and getting-started guide.

## What this fork adds

### 📡 Packet filter

A remote-configurable drop-rule engine for the `simple_repeater` firmware.
Rules match on packet type, route type, region, hop count, channel, and —
on decrypted payloads — channel identity and sender/text patterns. Rules are
managed over the repeater CLI, locally via serial or **remotely over the mesh**
by an authenticated admin. No rebuild, no physical access.

Independently of the rules, flood-route adverts can be rate-limited per
originating node: each node's advert is forwarded at most once per window
(up to 30 days), taming advert storms on busy channels while every node
stays discoverable.

→ Full guide, including the complete predicate reference and CLI commands:
**[FILTER.md](./FILTER.md)**

### 🔋 Battery gate

A configurable low-battery cut-off: below a set voltage the repeater stops
forwarding (but stays reachable and keeps advertising); when the battery
recovers it resumes automatically. Debounce + hysteresis keep it from
flapping, and the admin can check voltage and tune thresholds over the mesh.

→ Full guide: **[BATTERY.md](./BATTERY.md)**

### ⚡ Power profile

Three runtime knobs take an idle repeater from ~54 mA to ~4 mA: ESP32 light
sleep (`powersaving`), the radio's receive duty cycle (`radio.rxps`) and the CPU
clock (`cpu.freq`). All are persisted and revertible from the CLI — the
duty-cycle levels are chosen so the datasheet's preamble-capture guarantee still
holds, so the savings do not quietly turn into lost packets.

Credit where it is due: `powersaving` is upstream MeshCore, and the RXPS
duty-cycle subsystem is ported from **EasySkyMesh**
([IoTThinks/MeshCore](https://github.com/IoTThinks/MeshCore), `PowerSaving-v18`);
this fork added the `cpu.freq` setting, the port and the guide.

→ Full guide: **[POWER.md](./POWER.md)**

### 📻 Second radio preset

One transceiver, two LoRa presets. The repeater keeps its normal preset as the
protected home channel and visits a second preset for short windows, so it can
repeat adverts and group flood traffic between two nearby networks — or simply
listen to a second preset. Direct messages, ACKs, path returns and traces never
cross, replies go back on the preset the request arrived on, and the existing
packet filter applies to both. Off by default: with radio2 off the node behaves
like stock firmware.

→ Full guide, including the policy recipes, CLI reference and tradeoffs:
**[RADIO2.md](./RADIO2.md)**

### 🕒 Mesh clock sync

A repeater with no GPS, NTP, or battery-backed RTC can estimate UTC from signed
adverts it hears. When enabled (`clock mesh on`), it waits for a batch of distinct
sources to agree on the median before stepping the RTC; it is off by default.
Advert timestamps earlier than the firmware build timestamp are rejected and
never enter the batch. Public adverts are not a root of trust, so this is for
plausible time on clockless nodes, not authoritative timekeeping.

→ Full guide: **[MESHCLOCK.md](./MESHCLOCK.md)**

### 🛰️ Fleet management (remote CLI scripts)

Broadcast CLI jobs to a whole fleet of repeaters through one private keyed
channel. Set once per repeater (`fleet chan set` + tags + `fleet on`), a group
text message on that channel executes as a small script — deferred, and at
most once per job key per boot — on every repeater whose tags the job targets.
Every admin command works remotely: radio settings, filter rules, node names,
reboots. Re-send jobs freely to catch nodes that were offline; duplicates are
ignored automatically; tags address sub-fleets (by location, board type, or
rollout stage) and `!ack` makes nodes answer in-channel.

The channel key is the credential: holding the PSK is equivalent to
serial-console access on every member, so PSK hygiene is the whole security
model. The feature supersedes the battery gate — fleet jobs run, and
fleet-channel traffic relays, even while a low battery has suspended
forwarding.

→ Full guide, including recipes, the two-phase and scheduled-cutover
patterns, and the security model: **[FLEET.md](./FLEET.md)**

### 📶 MQTT observer

An optional firmware build that publishes the traffic a repeater hears to one
or two MQTT brokers, turning the node into a live feed for map, coverage, and
packet-analysis tools — no computer or companion app attached to it. It
captures **before** the packet filter and battery gate, so it observes packets
the node will not forward, and it uplinks its own adverts by default so the
node appears in the feed. ESP32 boards only; two broker slots with
Ed25519-signed JWT or username/password auth, TLS with a valid clock, and a
bounded best-effort queue that rides out broker outages.

→ Full guide, including the build targets, broker setup, CLI reference, and the
message schema: **[MQTT_OBSERVER.md](./MQTT_OBSERVER.md)**

## Why run this instead of stock repeater firmware?

Stock repeaters forward everything they hear, until the battery dies. This
fork is for operators who want policy and resilience at the node:

- **Shared / community nodes** — mute noisy senders or channels that eat your
  airtime, without asking anyone to change their setup.
- **OpenHop-style policy, nothing extra to run** — the rule engine is modelled
  after the policy filters in [OpenHop](https://github.com/openhop-dev) and
  lives entirely in the repeater firmware, so you get about the same
  functionality with just the node itself — no computer, companion app, or
  other hardware needed.
- **Solar / off-grid sites** — the node goes quiet *while it can still answer
  you*, instead of disappearing dark, and wakes itself when there's sun again.
- **Fleet operators** — run several repeaters (a region, a club's network, a
  set of towers) and push settings to all of them with one broadcast message
  instead of one admin session each: radio preset cutovers, advert hygiene,
  filter policy, rollouts and rollbacks.

## Getting the firmware

Prebuilt binaries for all supported boards are on the
[Releases page](https://github.com/jhuebert/MeshCore/releases) — except the
STM32WLE5 boards (wio-e5, wio-e5-mini, the wio-e5 bridge, RAK3172 and Tiny
Relay), which no longer fit the fork's repeater firmware and ship no build;
see [RADIO2.md](./RADIO2.md). Two channels:

| Channel | Branch | Based on | Tags |
|---|---|---|---|
| dev | `repeater-filter` | upstream `dev` (merged weekly by CI) | `filter-v*-dev.N` |
| stable | `repeater-filter-stable` | upstream `main` releases | `filter-v*` |

Flash as you would stock firmware, then follow
[FILTER.md](./FILTER.md) to set your first rules over the CLI,
[FLEET.md](./FLEET.md) if you are running a fleet,
[POWER.md](./POWER.md) if the node runs on solar or battery,
[RADIO2.md](./RADIO2.md) if it should listen on a second preset, or
[MQTT_OBSERVER.md](./MQTT_OBSERVER.md) if you flashed an observer build.
Observer builds ship in the same releases as the plain repeater firmware.
