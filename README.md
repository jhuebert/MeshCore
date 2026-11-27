# MeshCore: Remote-Configurable Repeater Firmware

A feature fork of [MeshCore](https://github.com/meshcore-dev/MeshCore) that
tracks upstream `main` and adds repeater management, power, radio, fleet and
update capabilities. Boards, clients, protocols and the build system are
unchanged; see the
[upstream README](https://github.com/meshcore-dev/MeshCore#readme) for those.

Every feature is opt-in. With all features off, a fresh install behaves like
stock firmware.

## Get started

1. Download a prebuilt image for your board from the
   [Releases page](https://github.com/jhuebert/MeshCore/releases).
2. Flash it as you would stock firmware.
3. Pick the one guide below that matches what you want to do. Most features
   need no rebuild and are configured from the repeater CLI.

> **Action required:** the first OTA release needs a full erase and a merged
> reflash for 28 ESP32 repeater targets: 20 move to `min_spiffs.csv` and 8 16 MB
> boards to `default_16MB.csv`. Back up the private key and settings first.
> See [OTA.md](./OTA.md#action-required-esp32-repeater-partition-change).

## Features

| Feature | What it does | Guide |
|---|---|---|
| Packet filter | Remote-configurable drop rules by packet type, route, hops, channel, region, sender and text, plus per-node advert rate limiting. | [FILTER.md](./FILTER.md) |
| Battery gate | Stops forwarding below a set voltage while the node stays reachable, then resumes automatically. | [BATTERY.md](./BATTERY.md) |
| Power profile | Three persisted CLI knobs take an idle repeater from about 54 mA to 4 mA. | [POWER.md](./POWER.md) |
| Second radio preset | One transceiver visits a second LoRa preset in short windows to bridge or listen on two networks. | [RADIO2.md](./RADIO2.md) |
| Ham mode | Sends publicly readable traffic at amateur power and encrypted traffic at normal power, with callsign IDs. | [HAM.md](./HAM.md) |
| Mesh clock sync | Estimates UTC from signed adverts on a repeater with no GPS, NTP or battery-backed clock. | [MESHCLOCK.md](./MESHCLOCK.md) |
| Fleet management | Broadcasts CLI scripts to tagged repeaters over one keyed channel, with acknowledgements and scheduled jobs. | [FLEET.md](./FLEET.md) |
| MQTT observer | Optional firmware that publishes heard traffic to one or two MQTT brokers for maps and analysis. | [MQTT_OBSERVER.md](./MQTT_OBSERVER.md) |
| LoRa OTA | Updates ESP32 repeaters over the mesh through the MeshCoreRepeaterManager app, with verification and rollback. | [OTA.md](./OTA.md) |
| Online OTA | Observer repeaters download a release over WiFi and apply it after a SHA-256 check. | [ONLINE_OTA.md](./ONLINE_OTA.md) |
| Chat bot | Opt-in responder for `test`, `ping` and `help` on one keyed channel. | [BOT.md](./BOT.md) |

> **Most meshes do not need the bot.** Every reply costs airtime for everyone.
> Enable it only with a reason, and give it its own channel with no other bots.

## Who this is for

- **Shared and community nodes.** Mute noisy senders or channels that eat your
  airtime without asking anyone to change their setup. The packet filter
  follows the policy model of [OpenHop](https://github.com/openhop-dev) and runs
  entirely in the repeater, with no computer or companion app to keep running.
- **Solar and off-grid sites.** The node goes quiet while it can still answer
  you, and wakes itself when there is sun again.
- **Fleet operators.** Push settings to many repeaters with one broadcast
  instead of one admin session each: preset cutovers, advert hygiene, filter
  policy, rollouts and rollbacks.
- **Licensed amateur operators.** Send publicly readable traffic at amateur
  power while encrypted traffic stays at unlicensed power.

## Credits

- **Power:** `powersaving` is upstream. RXPS comes from EasySkyMesh /
  [IoTThinks/MeshCore](https://github.com/IoTThinks/MeshCore) `PowerSaving-v18`,
  with the SX1262 post-preamble-timer fix from `Dev-PS18` (PR #27 by Dom4n). This
  fork added `cpu.freq`, the port and the guide.
- **Ham:** adapted from
  [NebraskaMesh/MeshCore-Ham](https://github.com/NebraskaMesh/MeshCore-Ham) by
  KF0WXX for the Nebraska Mesh community.

## Release channels

| Channel | Branch | Based on | Tags |
|---|---|---|---|
| dev | `repeater-filter` | `repeater-filter-stable` | `v*-dev.N` |
| stable | `repeater-filter-stable` | upstream `main` releases | `v*` |

The STM32WLE5 boards (wio-e5, wio-e5-mini, the wio-e5 bridge, RAK3172 and Tiny
Relay) no longer fit the fork's repeater firmware and ship no build. Observer
builds ship in the same releases as the plain repeater firmware.
