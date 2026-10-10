# MQTT Observer Firmware

MQTT observer builds publish successfully parsed packets heard over radio. The
receive hook runs before forwarding policy, so packet filters, the battery gate,
repeat enablement, hop/loop limits, and region policy do not suppress
observations. This is a best-effort, live feed—not a packet archive.

## Build target and status

The optional targets are defined in [`observer_targets.ini`](./observer_targets.ini),
which is also the allowlist used by `build.sh`:

```sh
pio run -e Xiao_S3_WIO_repeater_observer_mqtt
```

Only the XIAO S3 WIO target is currently listed. It compiles, but still needs
on-device memory, RF-load, TLS, broker, and OTA acceptance testing. Treat it as
experimental until those checks are complete. Standard repeater targets do not
depend on the MQTT library or compile observer sources. Optional release builds
can be selected separately with `scripts/local-release-build.sh --observers
<release-url-or-tag>`.

The MIT-licensed PsychicMqttClient is pinned to commit
`9e3005571918fdd83a2521243c501d1af137b0b6`. TLS uses the ESP32 Arduino CA
bundle with certificate validation and needs a valid device clock. JWT
authentication uses the MeshCore Ed25519 identity and broker-specific audience.

## Configure

The observer is disabled on a fresh install. This build uses the same shared
MQTT command names as agessaman's observer. Since it has one broker, the
reference's slot settings (`mqtt1.*`) are unnumbered `mqtt.*` settings:

```text
set wifi.ssid YourNetwork
set wifi.pwd YourWiFiPassword
set mqtt.server wss://broker.example:443/mqtt
set mqtt.audience broker.example
set mqtt.iata SEA
set mqtt on
get mqtt.status
```

The server can also be configured as a hostname and port separately:

```text
set mqtt.server broker.example
set mqtt.port 1883
```

Use `mqtts://` for MQTT over TLS, `wss://` for MQTT over secure WebSockets,
`mqtt://` for trusted private networks, or `ws://` for trusted private
networks. Secure URLs retain hostname and certificate verification. Do not use
plain transport on an untrusted network. URLs must not contain embedded
`username:password@` credentials. With a full URL, an explicit URL port takes
precedence; otherwise `mqtt.port` supplies the port. Host-only values infer
`mqtts://` for port 8883, `wss://` for 443, and `mqtt://` otherwise.

For username/password authentication, set `mqtt.username` and `mqtt.password`
and clear the JWT audience. JWT mode takes precedence whenever an audience is
configured; it uses username `v1_<PUBLIC_KEY_HEX>` and a 24-hour Ed25519-signed
token. The worker waits for valid time before connecting and renews the token
before expiry. Optional `mqtt.owner` and `mqtt.email` values are included in
JWT claims. Secrets are not echoed by set/get replies or fleet-script logs.

Supported commands:

```text
set mqtt.origin <name>           # empty follows the repeater node name
set mqtt.iata <code>             # 3 letters/digits, auto-uppercased; XXX reserved
set mqtt.server <host-or-URL>
set mqtt.port <1-65535>
set mqtt.username <username>
set mqtt.password <password>
set mqtt.audience <JWT audience> # empty clears JWT mode
set mqtt.owner <public-key-hex>  # optional JWT claim
set mqtt.email <email>           # optional JWT claim
set mqtt.topic <template>        # optional custom topic layout
set mqtt.token <token>           # optional {token} topic-template value
set mqtt.ntp <hostname>          # none restores pool.ntp.org
set mqtt.interval <1-60>         # status interval in minutes
set mqtt.rx on|off               # gate received packet/raw events
set mqtt.status on|off           # status topic
set mqtt.packets on|off          # parsed packet topic
set mqtt.raw on|off              # raw radio frame topic
set wifi.powersave none|min|max
set mqtt on|off
get mqtt.status
get mqtt.iata
get mqtt.origin
get mqtt.server
get mqtt.port
get mqtt.audience
get mqtt.owner
get mqtt.email
get mqtt.username
get mqtt.password                # configured/not set only
get mqtt.token                   # configured/not set only
get mqtt.topic
get mqtt.ntp
get mqtt.interval
get mqtt.rx
get mqtt.packets
get mqtt.raw
get wifi.status                  # connection, IP, RSSI
get wifi.ssid
get wifi.powersave
```

This focused image does not include presets, additional broker slots, packet
allowlists, TX uplink, neighbor queries, WebConfig, SNMP, or alerting. It
reports all MeshCore packet types received when `mqtt.rx` and packet/raw
publishing are enabled; it does not couple observation to repeater filtering.

SSID/password values are the rest of the command line; do not quote them. An
empty value clears a string setting. Settings are saved lazily about three
seconds after a change. Credentials are stored in `/mqtt_prefs` using a
versioned, CRC-checked staged save; version-1 settings migrate to version 2
without losing Wi-Fi or broker credentials. The filesystem is not encrypted: an
administrator with filesystem or firmware access can recover credentials.
Commands pass through the repeater's existing CLI permission boundary.

## Topics and payloads

By default, topics follow the MeshCore convention:

```text
meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/status
meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/packets
meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/raw
```

`DEVICE_PUBLIC_KEY` is the uppercase hexadecimal Ed25519 public key. A custom
`mqtt.topic` template supports `{iata}`, `{device}`, `{token}`, and `{type}`
(`status`, `packets`, or `raw`).

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
2.5-second network timeout. Status is QoS 1, non-retained for this custom
endpoint, sent on connect and every five minutes by default (`mqtt.interval`
changes the interval). The broker may apply its own retention policy.

## Live-only delivery and resilience

The radio receive hook copies raw bytes, the parsed packet, RF measurements,
and receive timestamp before dispatcher-owned packet storage is reused. A
four-entry RAM queue transfers snapshots to the MQTT worker. Overflow evicts
the oldest entry to favor recent traffic. Events enter the queue only while the
broker is connected; disconnect purges pending entries, and reconnect resumes
with newly heard packets. Entries older than ten seconds are dropped instead
of delivered late. No packet event, queue entry, or offline history is written
to flash or replayed after reboot. Publish work, TLS, NTP, and reconnects run
outside the receive and forwarding path.

`get mqtt.status` reports enabled/configuration state, Wi-Fi and clock/JWT
readiness, broker state, received/published/drop counters, queue depth, and
free/largest heap figures. The four-entry queue and age limit are intentionally
small: delayed packet delivery is not useful for the intended observer feed.

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
hardware checks for internal heap/largest block during Wi-Fi/TLS/reconnect,
worker stack use, a compatible local broker, outage recovery, and no late or
replayed events before treating the target as production-qualified.
