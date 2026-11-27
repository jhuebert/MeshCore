# LoRa OTA: User Guide

## What it does

Over-the-air updates are driven by the **[MeshCoreRepeaterManager](https://github.com/jhuebert/MeshCoreRepeaterManager) app**. The firmware only receives, verifies and applies an update. The app is the normal way to update a repeater, and its [user guide](https://github.com/jhuebert/MeshCoreRepeaterManager#readme) covers the steps in detail. This page is a short overview for repeater operators.

## Action required: ESP32 repeater partition change

The first release with the larger OTA staging headroom moves the 28 ESP32 repeater targets still using the 4 MB `default.csv` partition table. The baseline app slot is 1280 KiB. It grows to 1920 KiB on 4 MB boards and 6400 KiB on 16 MB boards.

| Board group | Table | App slots | SPIFFS |
|---|---|---|---|
| 4 MB boards (20 targets) | `min_spiffs.csv` | 1920 KiB | 128 KiB at `0x3D0000` |
| 16 MB boards (8 targets) | `default_16MB.csv` | 6400 KiB | 3456 KiB at `0xC90000` |

Both move SPIFFS away from `0x290000`. Normal repeater config uses only around 10 KiB, so day-to-day behaviour is unchanged.

**Note:** This change cannot be delivered over OTA. Upgrading needs a full erase and a flash of the merged image, and existing identity and settings are lost unless you back them up first.

### Upgrading an affected board

1. Back up the private key with `get prv.key` over the serial CLI, and record your settings, channels, filter rules and fleet tags (there is no config export yet).
2. Fully erase the board and flash the merged image for this release, not an app-only update.
3. Restore the key with `set prv.key <hex>` and re-enter the prefs, channels, rules and tags.
4. Confirm the node keeps its identity and reconnects to the mesh, then check `ota status` and run one stage and apply cycle.

If the node comes back with a different identity, the key was not restored: reflash with the backed-up `prv.key` before letting it rejoin the mesh.

### Affected targets

4 MB boards (`min_spiffs.csv`):

```text
Generic_E22_sx1262_repeater
Generic_E22_sx1262_repeater_bridge_espnow
Generic_E22_sx1268_repeater
Generic_E22_sx1268_repeater_bridge_espnow

LilyGo_T3S3_sx1262_repeater
LilyGo_T3S3_sx1262_repeater_bridge_espnow
LilyGo_T3S3_sx1276_repeater
LilyGo_T3S3_sx1276_repeater_bridge_espnow

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

16 MB boards (`default_16MB.csv`):

```text
LilyGo_TBeam_1W_repeater
LilyGo_TBeam_1W_repeater_bridge_espnow

Station_G2_repeater
Station_G2_repeater_bridge_espnow
Station_G2_logging_repeater
Station_G2_logging_repeater_bridge_espnow

Station_G3_ESP32_repeater
Station_G3_ESP32_logging_repeater
```

The other roles for these boards (companion radio, room server, KISS modem, terminal chat) share the base layout and need the same reflash. Observer builds have their own partition settings (the T-Beam 1W observer moved with its board so repeater and observer stay aligned), and all other boards are unaffected.

## What you need

- A repeater running an OTA-enabled build of this fork on an ESP32 board with a dual app partition. Other boards report the feature as unsupported.
- A stock MeshCore companion the app can reach: Bluetooth on Android, macOS or Linux, or over the network (TCP) on any platform. The companion does not need custom firmware.
- Admin access to the repeater. A blank password is enough when the companion is already approved as an administrator.
- The app: **[MeshCoreRepeaterManager releases](https://github.com/jhuebert/MeshCoreRepeaterManager/releases)**.

## Updating a repeater

1. Connect the app to the companion, pick the repeater and sign in.
2. Choose a firmware version. The app picks the image for the repeater's board and downloads it; you can also import your own `.bin`.
3. Choose where the transfer runs: the live mesh, a temporary private preset, or the repeater's second preset. The app restores the radios afterwards.
4. Review the plan, start the transfer, then confirm the update once the node reports that the image is staged and verified.

The app sends only what changed when it can match the running image, and it confirms success by reading back the new image's hash after the reboot. A lost reply is treated as unknown, not as success.

## If something goes wrong

Nothing boots until you confirm. If the new image cannot start its radio on first boot, the repeater flips back to the previous firmware by itself. These LoRa OTA CLI commands are available for diagnostics and recovery:

```text
ota status      # session state and progress
ota abort       # discard a staged session
ota rollback    # flip back manually
```
