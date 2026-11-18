# LoRa OTA — User Guide

Over-the-air updates are driven by the
**[MeshCoreOTA](https://github.com/jhuebert/MeshCoreOTA)** app. The firmware
side only receives, verifies and applies an update — there is no need to
assemble OTA operations by hand. The app's own
[user guide](https://github.com/jhuebert/MeshCoreOTA#readme) covers the steps
in detail; this page is a short overview for repeater operators.

---

## Action required: ESP32 repeater partition change

The first release with the larger OTA staging headroom moves the 28 ESP32
repeater targets still using the 4 MB `default.csv` partition table to
`min_spiffs.csv`. App slots grow from 1280 KiB to 1920 KiB, and SPIFFS shrinks
from 1408 KiB to 128 KiB. Normal repeater config uses only around 10 KiB, so
day-to-day behaviour is unchanged; a debug build with `MESH_PACKET_LOGGING`
enabled fills the smaller filesystem much sooner.

This change **cannot be delivered over OTA**: SPIFFS moves from `0x290000` to
`0x3D0000`, and the old region becomes part of `app1`. Upgrading one of these
boards needs a **full erase and a flash of the merged image**. Existing identity
and settings are lost unless you back them up first.

### Affected targets

```text
Generic_E22_sx1262_repeater
Generic_E22_sx1262_repeater_bridge_espnow
Generic_E22_sx1268_repeater
Generic_E22_sx1268_repeater_bridge_espnow

LilyGo_T3S3_sx1262_repeater
LilyGo_T3S3_sx1262_repeater_bridge_espnow
LilyGo_T3S3_sx1276_repeater
LilyGo_T3S3_sx1276_repeater_bridge_espnow
LilyGo_TBeam_1W_repeater
LilyGo_TBeam_1W_repeater_bridge_espnow

Station_G2_repeater
Station_G2_repeater_bridge_espnow
Station_G2_logging_repeater
Station_G2_logging_repeater_bridge_espnow
Station_G3_ESP32_repeater
Station_G3_ESP32_logging_repeater

Tenstar_C3_sx1262_repeater
Tenstar_C3_sx1268_repeater

ThinkNode_M2_Repeater
ThinkNode_M2_Repeater_bridge_espnow
ThinkNode_M5_Repeater
ThinkNode_M5_Repeater_bridge_espnow

Ebyte_EoRa-S3_Repeater
Xiao_C3_repeater

nibble_zero_connect_repeater_
nibble_zero_connect_repeater_bridge_espnow_
nibble_screen_connect_repeater_
nibble_screen_connect_repeater_bridge_espnow_
```

The other roles for these boards (companion radio, room server, KISS modem,
terminal chat) share the base layout and need the same reflash. Observer builds
keep their own partition tables, and all other boards are unaffected.

### Upgrading an affected board

1. Back up the private key with `get prv.key` over the serial CLI, and record
   your settings, channels, filter rules and fleet tags (there is no config
   export yet).
2. Fully erase the board and flash the **merged** image for this release — not
   an app-only update.
3. Restore the key with `set prv.key <hex>` and re-enter the prefs, channels,
   rules and tags.
4. Confirm the node keeps its identity and reconnects to the mesh, then check
   `ota status` and run one stage → apply cycle.

If the node comes back with a different identity, the key was not restored:
reflash with the backed-up `prv.key` before letting it rejoin the mesh.

## What you need

- A repeater running an OTA-enabled build of this fork on an ESP32 board with a
  dual app partition. Other boards report the feature as unsupported.
- A stock MeshCore companion the app can reach — Bluetooth on Android, macOS
  or Linux, or over the network (TCP) on any platform. The companion does not
  need custom firmware.
- Admin access to the repeater. A blank password is enough when the companion
  is already approved as an administrator.
- The app: **[MeshCoreOTA releases](https://github.com/jhuebert/MeshCoreOTA/releases)**.

## Updating a repeater

1. Connect the app to the companion, pick the repeater and sign in.
2. Choose a firmware version. The app picks the image for the repeater's board
   and downloads it; you can also import your own `.bin`.
3. Choose where the transfer runs — the live mesh, a temporary private preset,
   or the repeater's second preset. The app restores the radios afterwards.
4. Review the plan, start the transfer, then confirm the update once the node
   reports that the image is staged and verified.

The app sends only what changed when it can match the running image, and it
confirms success by reading back the new image's hash after the reboot. A lost
reply is treated as unknown, not as success.

## If something goes wrong

Nothing boots until you confirm. If the new image cannot start its radio on
first boot, the repeater flips back to the previous firmware by itself. These
CLI commands are available for diagnostics and recovery:

```text
ota status      # session state and progress
ota abort       # discard a staged session
ota rollback    # flip back manually
```

The app remains the normal way to run an update.

## For implementers

The node-side contract is in
[`examples/simple_repeater/OtaUpdate.h`](./examples/simple_repeater/OtaUpdate.h).
It is documented for maintaining clients, not as a procedure to follow by
hand.
