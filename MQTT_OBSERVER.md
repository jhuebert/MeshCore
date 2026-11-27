# MQTT Observer Firmware: User Guide

An optional build of the `simple_repeater` firmware that publishes the traffic a
repeater hears to one or two MQTT brokers. Point it at a broker, a public
analyzer or your own Mosquitto, and the node becomes a live feed for map,
coverage and packet-analysis tools with no computer attached. It is a
**best-effort** observation feed, not a packet archive.

The observer is a separate **ESP32-only** firmware flavour with its own build
targets (see [Firmware](#firmware)), configured over the same repeater CLI as
everything else (serial or [remote admin](./FILTER.md#managing-the-repeater-remotely)).
A standard repeater build does not link the MQTT library at all.

The topic convention and the status, packet and raw JSON payloads are defined in
[MQTT Schema: Reference](./MQTT_SCHEMA.md).

## What it does

The observer captures from two points on the receive path:

| Feed | What it captures |
|---|---|
| Receive | Every parsed packet, with SNR/RSSI and, for direct routes, the path. |
| Transmit | This node's own transmissions, adverts only by default. |

Capture happens **before** the packet filter, battery gate, repeat enablement,
hop/loop limits and region policy, so a packet the repeater refuses to forward
is still observed. Your filter cannot change observation, and the observer
cannot change filtering. A frame that never parsed is invisible; see
[Limits and good-to-knows](#limits-and-good-to-knows).

Each enabled broker gets three feeds: online status, parsed packets (rx and tx),
and the original radio frame.

## Firmware

Observer targets live in `observer_targets.ini` and cover the supported ESP32
observer boards:

- **Heltec**: T190, Wireless Tracker V1.1 and V2, V3, WSL3, V4, V4 ExpansionKit
- **LilyGo**: T3-S3 (SX1262), T-Beam 1W, T-Beam S3 Supreme (SX1262), T-Beam
  SX1262, T-Beam SX1276
- **RAK**: 3112
- **Station**: G2, G3
- **ThinkNode**: M7
- **Seeed**: XIAO S3 WIO

Observer targets ship in the **same release** as the plain repeater firmware:
there is no separate observer channel. Flash one like any other build; on this
fork the reference target is `Xiao_S3_WIO_repeater_observer`. To build locally:

```sh
pio run -e Xiao_S3_WIO_repeater_observer                 # one target
scripts/local-release-build.sh <release-url-or-tag>      # all, observers included
```

The T3-S3 and T-Beam 1W observer targets use a `min_spiffs.csv` partition table,
and Station G2/G3 use `default_16MB.csv`, to fit the larger app. Converting an
existing node to one of those may require a merged image and can reset
filesystem-backed settings. Back up anything you care about first.

Observer builds always create the worker task (1 Hz idle, Wi-Fi off with no
broker configured).

The MIT-licensed PsychicMqttClient is pinned to commit
`9e3005571918fdd83a2521243c501d1af137b0b6`. TLS uses the CA bundle embedded in
the firmware with certificate validation on, which needs a valid device clock (the observer
syncs NTP itself).

## Quick start

The observer ships **disabled**: no broker is enabled and no Wi-Fi is set. On a
node you have serial (or remote) access to:

```text
set wifi.ssid YourNetwork
set wifi.pwd YourWiFiPassword
set mqtt.iata SEA
set mqtt1.server wss://mqtt.example.com:443/mqtt
set mqtt1.audience mqtt.example.com
set mqtt1.enabled on
```

Check it:

```text
get mqtt.status      # -> mqtt wifi:up time:ok slots:1/2 rx:0 pub:1 drop:0 q:0/8 heap:...
get mqtt1.status     # -> mqtt1 connected wifi:up q:0
```

## Setting up

Shared `mqtt.x` settings cover Wi-Fi, `IATA`, origin, NTP, status interval and
the capture switches; numbered `mqtt1.*` / `mqtt2.*` settings cover server, port,
credentials, topic layout and the enable switch, matching the reference
observer's CLI. Everything is saved to `/mqtt_prefs` lazily (about three seconds
after a change) and survives reboots.

### Wi-Fi and time

```text
set wifi.ssid <ssid>            # not quoted; the rest of the line is the value
set wifi.pwd <password>
set wifi.powersave none|min|max
get wifi.ssid                   # the stored SSID, or "not set"
get wifi.pwd                    # the stored password, or "not set" (echoes it)
get wifi.powersave              # none, min or max
get wifi.status                 # wifi connected ip:192.0.2.10 rssi:-58
```

**Note:** `get wifi.pwd` prints the Wi-Fi password in cleartext, the same way
`get mqttN.password` prints the broker password, so read it over a trusted link
and do not paste the output anywhere.

`none` keeps the radio fully awake (best for a streamer); `min` and `max` trade
latency for power. The observer manages Wi-Fi and reconnects automatically; do
not run two things that fight over the Wi-Fi interface.

`start ota` serves the update page at `Started: http://192.0.2.10/update` on the
node's own address instead of the `MeshCore-OTA` hotspot. With Wi-Fi credentials
stored it brings the station up itself (up to 10 s to associate) even with both
broker slots off, and leaves the link up. That link is RAM-only: the reboot ending
an OTA session returns the node to Wi-Fi off. Without credentials, or if the
station does not associate, it falls back to the hotspot. `start ota` waits for
the association, so the loop is busy for a few seconds.

The observer gets UTC from NTP before it connects anywhere. Defaults are
`pool.ntp.org` with `time.google.com` and `time.cloudflare.com` as fallbacks;
an invalid clock means no TLS and no JWT, so it retries every 30 s until the
clock is sane. Set your own with:

```text
set mqtt.ntp time.example.com   # "none" restores pool.ntp.org
```

### Identity and location

```text
set mqtt.iata SEA               # 3 letters/digits, auto-uppercased; XXX is reserved
set mqtt.origin Roof Repeater   # optional; not quoted, the rest of the line is the value
```

`IATA` is the nearest airport-style code used in the topic path (see
[MQTT Schema: Reference](./MQTT_SCHEMA.md)). It is an address, not a
credential. Pick whatever your feed's tools expect. `origin` is the human name
shown in status messages; leave it empty to follow the repeater's node name.

### Server addresses and TLS

A broker's `server` may be a full URL or a bare hostname:

```text
set mqtt1.server wss://mqtt.example.com:443/mqtt
set mqtt1.server mqtts://mqtt.example.com:8883
set mqtt1.server broker.example.com          # host only; the port supplies the scheme
set mqtt1.port 8883
```

- Schemes: `mqtts://` (MQTT over TLS), `wss://` (secure WebSockets), `mqtt://`
  and `ws://` (plaintext).
- With a **bare hostname**, port `8883` implies `mqtts://`, `443` implies
  `wss://`, and any other port implies `mqtt://`.
- The default port setting is `1883`, which means "use the scheme's conventional
  port": 1883 for `mqtt`, 8883 for `mqtts`, 80 for `ws`, 443 for `wss`.
- An **explicit port in the URL wins** over `mqttN.port`.
- IPv6 hosts must be bracketed (`mqtts://[2001:db8::1]:8883`).
- Do **not** embed credentials in the URL (`user:pass@host` is rejected); use
  `mqttN.username` / `mqttN.password`.

**Note:** Secure schemes verify the broker certificate; never use the plaintext
ones on an untrusted network.

### Authentication

Each slot picks one of two modes:

**JWT (default when an audience is set).** The observer signs a short-lived
token with the node's MeshCore Ed25519 identity and authenticates as
`v1_<PUBLIC_KEY_HEX>`. This is how the public analyzers identify nodes:

```text
set mqtt1.audience mqtt.example.com   # the broker's expected audience (its host)
set mqtt1.owner <64-hex-public-key>   # optional claim
set mqtt1.email you@example.org       # optional claim
```

The token is valid for 24 hours and is renewed before expiry and before every
reconnect, so an outage cannot leave a dead token.

| Token claim | Value |
|---|---|
| `publicKey`, `aud`, `iat`, `exp`, `client` | Standard claims; `client` is this fork's version string. |
| `owner`, `email` | Optional, from `set mqtt1.owner` and `set mqtt1.email`. |

- `set mqtt1.audience` with no value switches the slot back to username/password.
- Renewing the token or changing the slot's config rebuilds the MQTT client before reconnecting, so the broker sees fresh credentials and session settings.
- The session is retained (`clean_session=false`) with a stable client ID, so a reconnect resumes the previous session instead of asking the broker to discard it. This covers brokers that close a clean reconnect while the old session is still settling.

**Username/password.** For a private broker:

```text
set mqtt1.username observer
set mqtt1.password secret
```

Leave the audience empty. If both are present, the audience (JWT) wins. Clear
it to use the password.

### Capture switches

These are shared across brokers:

```text
set mqtt.rx on|off        # master gate for receiving anything at all
set mqtt.packets on|off   # publish parsed packets
set mqtt.raw on|off       # publish the original radio frame
set mqtt.status on|off    # publish periodic status
set mqtt.tx off|on|advert # this node's own transmissions (see below)
set mqtt.interval 1-60    # status publish interval, in minutes (default 5)
```

`mqtt.tx` behaviour:

- `advert` (default): publish only this node's own adverts, so the node appears
  on maps and analyzers from its own beacons without uplinking every frame it
  repeats.
- `on`: publish every transmission (useful when you want the node's relay
  activity, but it is a lot of traffic).
- `off`: publish no transmissions.

TX messages carry no SNR/RSSI/score (there is no reception to measure) and are
never written as raw frames.

To watch traffic without uploading anything until you are ready, start with
`set mqtt.rx off` then flip it on.

## Command reference

All commands go through the repeater CLI; `N` is the one-based slot (`1` or `2`).
There is no `mqtt3`.

### Shared (`mqtt.`)

| Command | Effect |
|---|---|
| `set mqtt.iata <code>` | 3 letters/digits, auto-uppercased; `XXX` reserved: `OK - MQTT setting saved` |
| `set mqtt.origin <name>` | Human name for status messages; empty follows the node name |
| `set mqtt.ntp <hostname>` | NTP server; `none` restores `pool.ntp.org` |
| `set mqtt.interval <1-60>` | Status interval in minutes: `Err - interval must be 1-60 minutes` otherwise |
| `set mqtt.rx on\|off` | Master receive gate |
| `set mqtt.status on\|off` | Status publish switch |
| `set mqtt.packets on\|off` | Parsed-packet publish switch |
| `set mqtt.raw on\|off` | Raw-frame publish switch |
| `set mqtt.tx off\|on\|advert` | Own-transmission uplink: `Err - tx must be off, on, or advert` otherwise |
| `get mqtt.status` | Combined dashboard (see [Status](#reading-get-mqttstatus)) |
| `mqtt recover` | Drop and rejoin the Wi-Fi station, then reconnect both slots: `OK - Wi-Fi reset requested` |
| `get mqtt.iata` / `mqtt.origin` / `mqtt.ntp` | Current shared values |
| `get mqtt.interval` | Minutes |
| `get mqtt.rx` / `mqtt.status.enabled` / `mqtt.packets` / `mqtt.raw` / `mqtt.tx` | `on`/`off`, or `off`/`on`/`advert` for tx |

### Per-broker (`mqttN.`)

| Command | Effect |
|---|---|
| `set mqttN.enabled on\|off` | Enable/disable the slot: `OK - MQTT1 enabled` |
| `set mqttN.server <host-or-URL>` | Broker address; empty clears: `Err - invalid or oversized MQTT setting` |
| `set mqttN.port <1-65535>` | Port: `Err - port must be 1-65535` otherwise |
| `set mqttN.username <user>` / `password <pass>` | Password auth (audience must be empty) |
| `set mqttN.audience <aud>` | JWT audience; empty clears JWT mode and reverts to password auth |
| `set mqttN.owner <hex>` / `email <addr>` | Optional JWT claims |
| `set mqttN.topic <template>` | Topic layout; empty restores the MeshCore default. The template may only use the known placeholders: `{iata}`, `{device}`, `{token}`, `{type}`. Anything else is refused (`Err - topic placeholder must be {iata}, {device}, {token} or {type}`), because an unknown one would be published verbatim and the whole feed would go to a topic nobody reads |
| `set mqttN.token <token>` | Value for `{token}` in the template |
| `get mqttN.status` | One slot's state: `mqtt1 <state> wifi:up q:0` |
| `get mqttN.diag` | Last broker error and attempt history (see [Diagnostics](#reading-get-mqttndiag)) |
| `get mqttN.enabled` | `on`/`off` |
| `get mqttN.server` | Host label (or `not configured`) |
| `get mqttN.port` | Effective port |
| `get mqttN.username` / `password` / `token` | Value, or `not set`: **echoes the secret** |
| `get mqttN.audience` / `owner` / `email` | Value, or `not configured` |
| `get mqttN.topic` | Template, or the default |

Slot states are `off`, `config` (incomplete), `wifi`, `time` (waiting for a
valid clock), `connecting`, `connected`, `retry`, and `error`.

## Delivery behaviour

The observer is a bounded, best-effort feed that survives a broker outage
without stalling the mesh.

| Aspect | Behaviour |
|---|---|
| Queue | Small RAM queue, default 8 events shared by both slots. When full, the oldest pending event is replaced. Nothing is written to flash or replayed after reboot. |
| Retries | A failed publish is retried up to three times, 300–499 ms apart. A broker disconnected for a full minute flushes the queue. |
| Connect gate | Before connecting, the observer checks headroom in the pool that will hold TLS buffers (PSRAM when present, otherwise internal RAM) plus a small internal-RAM floor for MQTT/lwIP. A secure slot needs much more than a plaintext one. If the pool is fragmented it waits and retries every 30 s, which keeps a TLS slot on a small board from crashing the node. |
| Liveness | Every status message is QoS 1, so a status-enabled node proves its session each interval. With `mqtt.status off`, at most one packets/raw publish per broker per interval is promoted to QoS 1. A probe unacknowledged after 60 s while still connected force-stops the broker and reconnects on the normal ladder. A promoted packet can arrive twice; a slot with no traffic sends no probe. |
| Reconnect | 10/30/60/120/300 s ladder, then every 5 minutes forever, so a broker returns when the internet does. |
| Partial delivery | Both connected slots see each event, but the first publish completes it, so one slot's outage causes no duplicate work. QoS 0 means a rare loss if a connection dies mid-write. |

The receive hook runs **before mesh de-duplication**, so a packet heard on more
than one repeat can be published more than once, useful for per-reception RF
analysis; counts are receptions, not unique packets.

## Recipes

### Feed a public analyzer

See [Quick start](#quick-start): with no username or password, the analyzer
authenticates you by the signed token derived from your node identity.

### Primary plus backup broker

```text
set mqtt1.server wss://primary.example.com:443/mqtt
set mqtt1.audience primary.example.com
set mqtt1.enabled on
set mqtt2.server mqtts://backup.example.com:8883
set mqtt2.username observer
set mqtt2.password secret
set mqtt2.enabled on
```

Two independent slots; a packet only needs to reach one of them.

### Your own Mosquitto

On the broker, allow the observer's user and password. On the node:

```text
set mqtt1.server mqtts://broker.local:8883
set mqtt1.username observer
set mqtt1.password secret
set mqtt1.topic obs/{device}/{type}
set mqtt1.enabled on
```

Plaintext `mqtt://broker.local:1883` also works if the broker is on a trusted
segment, just never on an untrusted network.

### Adverts only, on a busy node

Keep the node visible on a map without uplinking everything it forwards:

```text
set mqtt.rx off
set mqtt.tx advert
set mqtt1.enabled on
```

Flip `set mqtt.rx on` later when you are ready to stream received packets too.

## Status and troubleshooting

### Reading `get mqtt.status`

```text
mqtt wifi:up time:ok slots:1/2 rx:4231 pub:4200 drop:31 q:0/8 heap:118032/98304
```

| Field | Meaning |
|---|---|
| `wifi:` | Station link: `up` / `down` |
| `time:` | Clock valid enough for TLS/JWT: `ok` / `wait` |
| `slots:` | Connected slots / configured slots (the brokers that are enabled, not the build-time maximum) |
| `rx:` | Frames heard since boot (counted before the receive gate) |
| `pub:` | Publish successes since boot |
| `drop:` | Skipped events (queue overflow, disconnect flush, failed publishes) |
| `q:` | Current queue depth / capacity |
| `heap:` | Free / largest internal heap block, in bytes |

`get mqttN.status` narrows this to one slot's state and queue depth.

### Reading `get mqttN.diag`

`status` says *what* the slot is doing; `diag` says *why the last attempt
failed*. The last error is kept after a reconnect, so a slot that has come
back still explains what happened.

```text
mqtt1 diag error err:closed tls:0x0 stk:0x0 cert:0x0 sock:0 att:7 fail:3 age:42s up:0s retry:18s
```

| Field | Meaning |
|---|---|
| `diag <state>` | The slot state (the same names as `status`) |
| `error` state | The last **connect attempt never came up**: the broker refused it (wrong credentials, wrong protocol) or was unreachable. The MQTT library only hands transport errors to the application, so the observer decides this from the attempt's own clock: a connect attempt that has not come up within 30 s is a failure, and the slot says so instead of cycling `connecting` → `retry` forever. The reconnect ladder keeps trying in the background, so a broker that comes back is picked up without a restart: each retry attempt shows `connecting` again, the state returns to `error` between attempts, and it stays `error` only until an attempt succeeds or the configuration changes (a reconfigured slot starts with a clean slate). |
| `err:` | Last failure: `none`, `transport` (TLS/socket), `closed` (the broker closed the session with no transport error, typically a CONNACK refusal, e.g. auth/identity), `mqtt` (an uncategorised client error), `timeout` (the connect attempt never came up in 30 s), `config` (URI/JWT/config failure), `liveness` (the broker stopped acknowledging the QoS 1 probe), or `other/<n>` |
| `tls:` | `esp_tls_last_esp_err`, the ESP-IDF TLS error (hex; `0x0` = none) |
| `stk:` | The TLS stack's own error (mbedTLS; hex) |
| `cert:` | Certificate verification flags (hex bitmask; `0x0` = none) |
| `sock:` | Socket errno from the transport (`0` = none; e.g. `111` = `ECONNREFUSED`, `113` = `EHOSTUNREACH`, `118` = `ETIMEDOUT`) |
| `att:` | Connect attempts since boot |
| `fail:` | Consecutive failed attempts; reset by a successful connection or a configuration change |
| `age:` | Seconds since the last error (`0s` = none since boot) |
| `up:` | Seconds the current connection has been up (`0s` = disconnected) |
| `retry:` | Seconds until the next attempt; `0s` when connected, attempting, or due now |

The pinned MQTT client forwards only transport errors to the firmware, so a
refused session appears as `closed` here; the broker's exact CONNACK code is
not available to the observer. `transport`, `timeout`, `tls`, `cert` and `sock`
point at the path or the TLS session; `config` never opened a socket at all.

### Common problems

| Symptom | Check | Usual cause |
|---|---|---|
| `get mqtt.status` shows `slots:0/2` | `get wifi.status`, `time:` field | Wi-Fi not connected, or clock still `wait` |
| Slot stuck at `config` | `get mqttN.server`, `get mqtt.iata` | No server, or the default topic needs an `IATA` and none is set |
| Slot cycles `connecting` → `retry` | `get mqttN.server`, broker reachability | Wrong host/port, wrong scheme, or the broker is down |
| Slot shows `error` | `get mqttN.diag`, then `get mqttN.server` / `username` / `password` / `audience` | The last connect attempt never came up. `diag` separates a broker refusal (`closed`) from a network/TLS failure (`transport`, `timeout`, `tls`, `sock`). The slot keeps retrying; a configuration change starts a fresh attempt |
| Auth rejected | audience vs username/password | Audience set when you meant password (JWT wins); or the audience/token is wrong |
| No TLS, immediately after boot | `time:` | Clock not yet synced; secure brokers need valid time |
| `drop:` climbing steadily | `q:` | Broker disconnected too long, or an overloaded node, check the broker |
| Node disappeared from the feed | `get mqttN.status` | Status is online-only; expire stale status in the consumer |
| Duplicate packets in the feed | - | Normal: the hook is pre-dedup; multi-repeat receptions are real observations |

## Security model

- **CLI access is the boundary.** Observer commands run through the repeater's
  existing permission boundary, so anyone who can run `set` can change broker
  credentials.
- **`get` echoes secrets.** `get mqttN.password` / `username` / `token` return
  the stored value, and fleet scripts log their run to serial, so protect the
  serial console and any fleet transcripts.
- **The filesystem is not encrypted.** Anyone with filesystem or firmware access
  can recover broker credentials.
- **TLS verifies the broker** for `mqtts://` and `wss://`. Do not drop to
  plaintext on a network you do not control.
- **Your identity is your key.** JWT auth signs with the node's MeshCore
  identity; there is no separate observer credential to leak.

## Limits and good-to-knows

| Limit | Value |
|---|---|
| Broker slots | 2 (`mqtt1`, `mqtt2`) |
| Queue | 8 events shared by both slots; build-time `-DMQTT_OBSERVER_QUEUE_CAPACITY=N` |
| Queue retries | 3, with a 300–499 ms delay |
| Offline flush | after 1 continuous minute with no connected broker |
| Reconnect backoff | 10/30/60/120/300 s, then every 300 s, indefinitely |
| Connect attempt timeout | 30 s |
| Status interval | 1–60 minutes (default 5) |
| JWT lifetime | 24 hours, renewed before expiry and before reconnect |
| MQTT session | retained across reconnects (`clean_session=false`), stable client ID |
| Packet/raw network timeout | 2.5 s |
| Liveness probe | status message (or one packet/raw per interval when status is off); 60 s PUBACK timeout |
| TLS heap gate | ~64 KB free and ~18 KB largest block in the TLS pool (PSRAM when present, otherwise internal), plus ~24 KB free and ~8 KB largest internal for a secure connect |

- **Stale-session recovery.** Some brokers bind the old session or auth state
  to the identity and silently close an in-process reconnect (`err:closed`),
  even with a fresh token and client. After **5 consecutive `closed` failures**
  the observer drops and rejoins Wi-Fi, giving both slots new TCP/TLS state
  without rebooting. The IP may change; the status gap is the rejoin plus one
  connect attempt. `mqtt recover` forces the same reset.
- **ESP32 only.** nRF52, STM32, and RP2040 boards have no observer build.
- **Not included:** broker presets, per-slot packet-type allowlists, neighbour
  queries, a WebConfig portal, SNMP, and alerting. Every packet type is captured
  when the switches allow it.
- **No migration from other observers.** `/mqtt_prefs` is version 5 and
  CRC-checked, with this fork's own `MQO1` record magic, so flashing from another
  MeshCore observer build rejects its config and starts from defaults rather than
  misreading it, so you re-enter the settings once. There is no migration from
  earlier prototype formats.
- **Partial capture is expected** under heavy RF or packet-pool pressure; the
  observer is a window on the mesh, not a guaranteed record of it.
