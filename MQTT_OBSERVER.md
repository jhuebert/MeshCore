# MQTT Observer Firmware

MQTT observer builds publish successfully parsed packets heard over radio. The
receive hook runs before forwarding policy, so packet filters, the battery gate,
repeat enablement, hop/loop limits, and region policy do not suppress
observations. This is a best-effort feed, not a packet archive.

## Build target and status

The optional targets are defined in [`observer_targets.ini`](./observer_targets.ini),
which is also the allowlist used by `build.sh`:

```sh
pio run -e Xiao_S3_WIO_repeater_observer
```

Observer-enabled Repeater targets cover the supported Observer board list:
Heltec T190, Tracker V1.1 and V2, V3, WSL3, V4 and V4 ExpansionKit; LilyGo
T3-S3 SX1262, T-Beam 1W, T-Beam S3 Supreme SX1262, T-Beam SX1262 and SX1276;
RAK 3112; Station G2 and G3; ThinkNode M7; and XIAO S3 WIO.
Room Server targets are intentionally not included. All 17 Repeater target
environments compile, but still need per-board memory, RF-load, TLS, broker, and
OTA acceptance testing; treat them as experimental until those checks are
complete. The T3-S3 and T-Beam 1W observer targets use `min_spiffs.csv`, and
Station G2/G3 use `default_16MB.csv`, to fit the larger app. Those partition
overrides can require a merged image when converting an existing node and may
reset filesystem-backed settings. Standard repeater targets do not depend on
the MQTT library or compile observer sources. Optional release builds can be
selected separately with
`scripts/local-release-build.sh --observers <release-url-or-tag>`.

The MIT-licensed PsychicMqttClient is pinned to commit
`9e3005571918fdd83a2521243c501d1af137b0b6`. TLS uses the ESP32 Arduino CA
bundle with certificate validation and needs a valid device clock. JWT
authentication uses the MeshCore Ed25519 identity and a per-broker audience.

## Configure

The observer is disabled on a fresh install and supports two independent MQTT
broker slots. Broker-specific settings always use numbered commands (`mqtt1.*`,
`mqtt2.*`) to match agessaman's CLI; unnumbered `mqtt.*` commands are only for
shared settings such as IATA, origin, and publish switches. Broker credentials
and topic templates are independent. Wi-Fi, IATA, origin, NTP, status interval,
and packet/status/raw enable switches are shared.

```text
set wifi.ssid YourNetwork
set wifi.pwd YourWiFiPassword
set mqtt1.server wss://first.example:443/mqtt
set mqtt1.audience first.example
set mqtt2.server mqtts://second.example:8883
set mqtt2.username observer
set mqtt2.password secret
set mqtt.iata SEA
set mqtt1.enabled on
set mqtt2.enabled on
get mqtt.status
get mqtt1.status
get mqtt2.status
```

For a username/password connection, set that slot's `username` and `password`
and leave its `audience` empty. JWT mode takes precedence when that slot has an
audience configured; it uses username `v1_<PUBLIC_KEY_HEX>` and a 24-hour Ed25519-signed token. The
worker waits for valid time before connecting and renews tokens before expiry.
Optional per-slot `owner` and `email` values are included in JWT claims. Secrets
are not echoed by set/get replies or fleet-script logs.

A server may be configured as a hostname and port separately. Use `mqtts://`
for MQTT over TLS, `wss://` for secure WebSockets, `mqtt://` for trusted private
networks, or `ws://` for trusted private networks. Secure URLs retain hostname
and certificate verification. Do not use plain transport on an untrusted
network. URLs must not contain embedded `username:password@` credentials. With
a full URL, an explicit URL port takes precedence; otherwise that slot's
`mqttN.port` supplies the port. Host-only values infer `mqtts://` for port 8883,
`wss://` for 443, and `mqtt://` otherwise.

Supported commands include:

```text
set mqttN.server <host-or-URL>
set mqttN.port <1-65535>
set mqttN.username <username>
set mqttN.password <password>
set mqttN.audience <JWT audience>  # empty clears JWT mode
set mqttN.owner <public-key-hex>   # optional JWT claim
set mqttN.email <email>            # optional JWT claim
set mqttN.topic <template>         # optional per-broker topic layout
set mqttN.token <token>            # optional {token} template value
set mqttN.enabled on|off           # enable/disable that slot
set mqtt.origin <name>             # empty follows the repeater node name
set mqtt.iata <code>               # 3 letters/digits, auto-uppercased; XXX reserved
set mqtt.ntp <hostname>            # none restores pool.ntp.org
set mqtt.interval <1-60>           # shared status interval, in minutes
set mqtt.rx on|off                 # shared received-packet/raw capture gate
set mqtt.status on|off             # shared status publish switch
set mqtt.packets on|off            # shared parsed-packet publish switch
set mqtt.raw on|off                # shared raw-frame publish switch
set wifi.powersave none|min|max
get mqtt.status                    # combined connection/queue summary
get mqttN.status                   # one slot's connection state
get mqttN.enabled
get mqttN.server
get mqttN.port
get mqttN.audience
get mqttN.owner
get mqttN.email
get mqttN.username
get mqttN.password
get mqttN.token
get mqttN.topic
get mqtt.iata
get mqtt.origin
get mqtt.ntp
get mqtt.interval
get mqtt.rx
get mqtt.status.enabled
get mqtt.packets
get mqtt.raw
get wifi.status                    # connection, IP, RSSI
get wifi.ssid
get wifi.pwd
get wifi.powersave
```

`N` is the one-based slot number (`1` or `2` in this build). The worker and
`/mqtt_prefs` config support two broker slots. Adding more slots requires a
config format version change. The observer does not include presets, packet
allowlists, TX uplink, neighbor queries, WebConfig, SNMP, or alerting. It reports
all MeshCore packet types received when the shared RX and packet/raw switches
are enabled.

SSID/password values are the rest of the command line; do not quote them. An
empty value clears a string setting. Settings are saved lazily about three
seconds after a change. The experimental `/mqtt_prefs` record is version 4 and
CRC-checked; the two broker records make its payload 1,508 bytes (1,517 bytes
including the record header and CRC). No migration from earlier prototype
formats is included because this observer has not been deployed and there are
no device configs to preserve. Credential values are returned by `get` commands
and appear in fleet-script serial logs; protect CLI access and logs accordingly.
The filesystem is not encrypted; an administrator with filesystem or firmware
access can recover credentials. Commands pass through the repeater's existing
CLI permission boundary.

## Topics and payloads

By default each enabled broker uses the MeshCore topic convention:

```text
meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/status
meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/packets
meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/raw
```

`DEVICE_PUBLIC_KEY` is the uppercase hexadecimal Ed25519 public key. A
per-broker `mqttN.topic` template supports `{iata}`, `{device}`, `{token}`, and
`{type}` (`status`, `packets`, or `raw`).

The packet schema includes `type: "PACKET"`, `direction: "rx"`, an ISO-8601
UTC timestamp, origin and device identity, MeshCore packet hash, route, wire
length, payload length, raw packet bytes, and receive SNR/RSSI and score. Direct
route paths are lowercase hex arrays. Numeric fields retain the reference
observer's string formatting. The raw schema uses `type: "RAW"` and the
original radio frame as uppercase hex. Status includes the reference's online,
timestamp, identity, model, firmware, radio, and client-version fields. The
client version is `meshcore-jhuebert/<firmware-version>` to distinguish this
fork from other MeshCore clients.

Packet/raw publishes are QoS 0, non-retained, and sent synchronously with a
2.5-second network timeout. Status is QoS 1, non-retained, sent on connect and
every five minutes by default (`mqtt.interval` changes the shared interval).
Failed status publishes are retried no faster than every 30 seconds per slot.
The broker may apply its own retention policy.

## Bounded queue and delivery behavior

The receive hook copies raw bytes, the parsed packet, RF measurements, and
receive timestamp before dispatcher-owned packet storage is reused. A bounded
RAM queue transfers snapshots to the MQTT worker, including while both brokers
are temporarily disconnected. The default capacity is 8 events and can be
overridden at build time with `-DMQTT_OBSERVER_QUEUE_CAPACITY=<count>`; this
capacity is shared by all broker slots and does not change `/mqtt_prefs`. The queue holds an event at its head
while the synchronous publish attempt is in progress, so capture-side overflow
cannot evict that in-flight event. When full, the oldest pending event is
replaced; with capacity 1, a new event is dropped while the sole entry is in
flight.

On a failed attempt, the queue retries up to three times with a short
300–499 ms delay. Pending events are flushed after one continuous minute with
no connected broker. No packet event, queue entry, or offline history is written
to flash or replayed after reboot. Publish work, TLS, NTP, and reconnects run
outside the receive and forwarding path.

Both connected slots are offered each queued event. Following the reference
observer's queue policy, the event is considered delivered when any connected
slot successfully publishes either the structured packet or raw frame; a
partial success is not retried for the other slot. Thus the queue protects
observations during transient send failures, but does not guarantee delivery to
every configured broker during a per-slot outage. QoS 0 is also inherently
best-effort: a connection loss after the broker accepts a publish but before
the sender can confirm the write may result in a duplicate retry or an
unrecoverable loss.

`get mqtt.status` reports Wi-Fi/clock state, connected slot count, received and
published counters, drops, queue depth/capacity, and free/largest heap figures.
`get mqttN.status` reports the connection state for an individual slot.

Capture begins at `logRx()`, after a packet object has been allocated and the
radio frame parsed. Invalid frames are excluded. The repeater currently uses a
32-packet pool; pool exhaustion can prevent the parsed-packet hook from being
reached. This build does not change packet-pool reservation or outbound relay
policy, and hardware saturation testing remains outstanding. Observation is
independent of later forwarding decisions, but not of successful parsing or
packet-pool availability.

## Build measurements and remaining validation

The observer target has been compile-tested on the XIAO S3 WIO. Build success
does not establish runtime TLS heap margin, worker stack high-water, OTA
behavior, or capture resilience under sustained RF/outbound pressure. Complete
hardware checks for internal heap/largest block during two simultaneous
Wi-Fi/TLS connections and reconnects, queue behavior under broker outages,
worker stack use, compatible local brokers, and no late/replayed events before
treating the target as production-qualified.
