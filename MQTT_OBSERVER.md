# MQTT Observer Firmware

MQTT observer builds publish packets successfully parsed from the radio receive
path. The observer hook runs before forwarding policy, so the packet filter,
battery gate, repeat switch, hop/loop limits, and region policy do not suppress
an observation. This is a best-effort live feed, not a packet archive.

## Build target and status

The optional targets are defined in [`observer_targets.ini`](./observer_targets.ini),
which is also the allowlist used by `build.sh`:

```sh
pio run -e Xiao_S3_WIO_repeater_observer_mqtt
```

Only the XIAO S3 WIO target is listed initially. It compiles successfully, but
has not yet had the required on-device memory, RF-load, TLS, broker, and OTA
acceptance tests. Treat it as experimental until those tests are completed.
Standard repeater targets do not depend on the MQTT library or compile the
observer sources. Optional release builds can be selected separately with
`scripts/local-release-build.sh --observers <release-url-or-tag>`; the normal
repeater release set excludes observer targets.

The MIT-licensed PsychicMqttClient is pinned to upstream commit
`9e3005571918fdd83a2521243c501d1af137b0b6` (tag 0.2.4). It supports
MQTT/TLS and MQTT over WebSockets/TLS on this ESP32 Arduino framework. TLS uses
the Arduino ESP32 certificate bundle and requires a valid device clock. JWT authentication uses
the MeshCore Ed25519 identity and the broker-specific `aud` value.

## Configure

The observer is disabled on a fresh install. Configure Wi-Fi, a custom broker,
and the three-letter IATA/topic component, then enable it:

```text
set wifi.ssid YourNetwork
set wifi.pwd YourWiFiPassword
set mqtt.server wss://broker.example:443/mqtt
set mqtt.audience broker.example
set mqtt.iata SEA
set mqtt on
get mqtt.status
```

Use `mqtts://` for MQTT over TLS, `wss://` for MQTT over secure WebSockets,
`mqtt://` for trusted private networks, or `ws://` for trusted private networks.
Secure URLs keep hostname/certificate verification enabled; do not use plain
transport on an untrusted network. The URL must not include embedded
`username:password@` credentials.

For a broker using username/password instead of MeshCore JWT, set
`mqtt.username` and `mqtt.password`, and clear the audience with
`set mqtt.audience`. An open/custom broker may omit both. JWT mode takes
precedence whenever an audience is configured; it uses username
`v1_<PUBLIC_KEY_HEX>` and a 24-hour token signed by the node's identity. The
worker waits for NTP time before connecting and renews tokens before expiry.
The token/private key are never displayed by status or get commands.

Other controls:

```text
set mqtt off                 # stop MQTT and Wi-Fi activity
set mqtt on
set mqtt.status off|on       # periodic status topic
set mqtt.packets off|on       # parsed packet topic
set mqtt.raw off|on           # raw-radio frame topic
get mqtt.status
get wifi.status
get mqtt.server
get mqtt.password             # reports only whether one is configured
get wifi.pwd                  # reports only whether one is configured
```

SSID and password values are the rest of the command line; they are not quoted.
An empty value clears a string setting. Settings are written lazily about three
seconds after a change. Commands run through the repeater's existing CLI
permission boundary. Secret commands return generic confirmations and
credential reads are masked. Fleet script logs redact MQTT/Wi-Fi credential
commands. The settings are stored in `/mqtt_prefs` using a versioned,
CRC-checked staged save, but the filesystem is not encrypted: an administrator
with filesystem or firmware access can recover the credentials.

## Topics and payloads

The topic root matches the MeshCore observer convention:

```text
meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/status
meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/packets
meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/raw
```

`DEVICE_PUBLIC_KEY` is the node's uppercase hexadecimal Ed25519 public key.
The `packets` payload has `type: "PACKET"`, `direction: "rx"`, an ISO-8601 UTC
`timestamp`, `origin`, `origin_id`, the MeshCore packet `hash`, route (`F` or
`D`), packet and payload lengths, on-air packet bytes in `raw`, and received
`SNR`/`RSSI` (plus the receive score). Direct-route paths are included as a
lowercase hex array. Numeric values use the same string representation as the
reference observer payload.

The `raw` payload has `type: "RAW"` and `data` containing the original received
radio frame as uppercase hexadecimal. Status payloads use the reference fields
for online state, timestamp, origin/device identity, model, firmware, radio,
and client version.

Packet and raw publishes are QoS 0 and non-retained. Status is low-rate (five
minutes) and QoS 1, non-retained for the custom endpoint. Status is sent on
connect. A configured broker may apply its own retention policy, but this
firmware does not request retained status messages for custom endpoints.

## Live-only delivery and capture boundary

The radio receive hook copies the raw frame, parsed packet, signal readings,
and receive timestamp before dispatcher-owned packet storage is reused. A
four-entry RAM queue hands snapshots to the MQTT worker. Queue overflow drops
the oldest pending snapshot to favor recent mesh traffic. Packet/raw events
are accepted only while MQTT is connected; disconnect purges queued snapshots,
and MQTT reconnect starts with newly received packets. No packet event,
queue entry, or offline history is written to flash or replayed after reboot.

`get mqtt.status` reports enabled/configuration state, Wi-Fi and clock/JWT
readiness, broker connection state, received/published/drop counters, queue
depth, and heap figures. `received` counts successfully parsed packets,
`published` counts successful packet/raw topic publishes, and `drop` combines
disconnected, queue-overflow, invalid-time, and publish failures.

Capture starts at `logRx()`, after the dispatcher has parsed the radio frame and
obtained a packet object. Invalid frames are intentionally excluded. The
repeater currently uses a 32-packet pool; if the pool is exhausted, the
successful-parse hook is not reached and that reception is not observed. This
first version does not change packet-pool reservation or relay-queue policy.
It also has not yet been stress-tested on hardware under saturated outbound
traffic. Observation is independent of the filter, battery gate, forwarding
switch, and later relay decisions, but not of successful parsing or available
packet-pool capacity.

## Build measurements and remaining validation

On the checked-out firmware and PlatformIO environment, `pio run -t size`
reports 1,161,613 bytes of flash use for the standard XIAO S3 repeater and
1,338,257 bytes for the observer image, an increase of 176,644 bytes. The
PlatformIO static RAM report is 76,696 bytes for the standard image and
85,336 bytes for the observer image. `-fstack-usage` reports a largest new frame of
1,584 bytes in JWT creation and 880 bytes in packet capture, below the repository's
2 KiB frame gate. These build figures do not replace runtime heap or task-stack
high-water measurements. The observer still
needs on-device checks for internal heap/largest block through TLS connect and
reconnect, task stack high-water mark, OTA and partition behavior, sustained RF
load, a compatible JWT broker, and no-replay behavior across broker outages.
No broker credentials or hardware were available for those acceptance tests.
