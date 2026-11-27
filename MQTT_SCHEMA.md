# MQTT Schema: Reference

This is the reference appendix for the
[MQTT Observer Firmware: User Guide](./MQTT_OBSERVER.md). It defines the topic
convention and the status, packet and raw JSON payloads published by each
enabled broker.

The payloads follow the reference observer's schema, so feeding the same tools
other MeshCore observers feed needs no changes.

## Topic convention

Each enabled broker gets three feeds on the MeshCore topic convention:

```text
meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/status     # online status
meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/packets    # parsed packets, rx and tx
meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/raw        # the original radio frame
```

`DEVICE_PUBLIC_KEY` is the node's uppercase-hex Ed25519 public key.

## Topic templates

By default every broker uses the MeshCore convention:

```text
meshcore/{IATA}/{DEVICE_PUBLIC_KEY}/{status|packets|raw}
```

A per-broker template can reshape it. `{iata}`, `{device}`, `{token}`, and
`{type}` are substituted (`{type}` is `status`, `packets`, or `raw`; `{token}`
is the slot's `mqttN.token` value):

```text
set mqtt1.topic meshcore/{iata}/{device}/{type}
set mqtt1.topic meshrank/uplink/{token}/{device}/{type}
set mqtt1.token my-site-token
```

A broker is not contacted until its slot is `enabled on`, its server is set,
Wi-Fi is configured, and (for the default/{iata} template) an `IATA` is set.
The topic placeholder rule is in the
[main guide's command reference](./MQTT_OBSERVER.md#per-broker-mqttn).

## Status

Publish `status: "online"` (QoS 1, non-retained) on connect and every
`mqtt.interval` minutes:

```json
{"status":"online","timestamp":"2026-10-16T20:05:00.123456+00:00","origin":"Roof Repeater",
 "origin_id":"<UPPERHEX-PUBKEY>","model":"Station G2","firmware_version":"...",
 "radio":"869.525,62.5,8,5","client_version":"meshcore-jhuebert/<version>","repeat":true,
 "stats":{"battery_mv":4050,"uptime_secs":86400,"noise_floor":-110}}
```

`model` is the board name (`getManufacturerName()`, e.g. `Station G2`) and `repeat` is
this node's relay state, so analyzers that key off `repeat` show the node as a
repeater. `stats` carries the live device readings: `battery_mv` (0 when the board
reports no battery), `uptime_secs`, and `noise_floor` (dBm).

Status messages report online on connect and periodically. The observer does not
publish an offline message when a broker connection drops; consumers should
expire stale status based on their own timeout or broker presence information.

## Packet

`direction` is `rx` for a packet the node heard and `tx` for one it sent.
Numeric fields are strings, matching the reference schema (a receiver's tools
expect them that way). Receive messages add `SNR`/`RSSI` and a `score`; direct
routes add a `path` array of lowercase hex hop tokens:

```json
{"timestamp":"2026-10-16T20:05:01.004000+00:00","hash":"AB12...","origin":"Roof Repeater",
 "type":"PACKET","direction":"rx","time":"20:05:01","date":"16/10/2026","len":"38",
 "packet_type":"4","route":"D","payload_len":"32","raw":"<UPPERHEX-WIRE>",
 "origin_id":"<UPPERHEX-PUBKEY>","SNR":"-7.5","RSSI":"-92","score":"1234",
 "path":["ab","01"]}
```

## Raw

`type: "RAW"` with the original radio frame as uppercase hex:

```json
{"origin":"Roof Repeater","origin_id":"<UPPERHEX-PUBKEY>",
 "timestamp":"2026-10-16T20:05:01.004000+00:00","type":"RAW","data":"<UPPERHEX>"}
```

## Timing and QoS

- Packet and raw publishes are QoS 0, non-retained, sent synchronously with a
  2.5-second network timeout.
- Status is QoS 1; a failed status publish is retried no faster than every 30
  seconds per slot.

The broker applies its own retention policy to published messages.
