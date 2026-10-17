# Custom Companion Firmware

Fork-only, unpublished notes for the `custom-companion-easysky` branch. That
branch is `IoTThinks/MeshCore`'s `PowerSaving-v18` — the source behind the
EasySkyMesh PowerSaving18 release — plus a deliberately tiny diff (four
commits, four files) so it stays easy to rebase: it adds two commands to the
companion (client) firmware's CLI.

This is not a release channel — no CI, no release workflow. Build and flash
locally.

## Taking EasySkyMesh updates

`iotthinks` is the source repository; add it once:

```
git remote add iotthinks https://github.com/IoTThinks/MeshCore.git
```

For fixes IoTThinks pushes to the current series:

```
git fetch iotthinks --prune
git rebase iotthinks/PowerSaving-v18
```

For a new release series (the EasySkyMesh release notes name its source branch,
e.g. `PowerSaving-v19`), replay only this branch's commits onto it:

```
git fetch iotthinks --prune
git rebase --onto iotthinks/PowerSaving-v19 iotthinks/PowerSaving-v18
```

Resolve any conflicts, build, then update the fork (a rebase rewrites history):

```
git push --force-with-lease fork custom-companion-easysky
```

## Build

`build.sh` requires a version in the environment:

```
export FIRMWARE_VERSION=v1.18.0-custom
bash build.sh build-firmware Xiao_S3_WIO_companion_radio_ble
```

Both commands below are reached the same way on every companion build: the
serial "CLI rescue" prompt (display long-press) and the app's remote CLI
console (`CMD_RUN_CLI_COMMAND`).

## `set repeat on|off` / `get repeat`

Toggle this companion's repeat (packet forwarding) flag without touching the
radio parameters:

```
get repeat         # > on     /  > off
set repeat on      # OK - repeat is now ON
set repeat off     # OK - repeat is now OFF
```

Only `off` turns it off; anything else is treated as `on`, matching the
repeater/room-server CLI. The flag is persisted and takes effect immediately
(no reboot).

**Why it exists.** The app only enables repeat together with one of its
client-repeat frequencies (433.000 / 869.495 / 918.000 MHz by default), so
toggling repeat from the app forces a frequency change. This command sets the
flag alone, so repeat can be enabled on the frequency you are already using.

**Caveat.** While repeat is on, the app still refuses to change radio
parameters: it re-sends repeat-enabled with the current frequency, which the
firmware rejects. Run `set repeat off` before editing radio settings in the
app.

## `start ota`

Start an over-the-air firmware update, like the repeater/room-server command:

```
start ota
# on Wi-Fi:   Started: http://<lan-ip>/update
# otherwise:  Started: http://192.168.4.1/update
```

- **ESP32, already connected to Wi-Fi** — the OTA page is served on that
  network at the reported address; open it from any device on the same network.
- **ESP32, not connected** — a `MeshCore-OTA` hotspot is started; join it and
  open `http://192.168.4.1/update`.
- **nRF52** — enters BLE DFU (`OK - mac: ...`); use the nRF DFU app.
- **Other platforms** — replies `Error` (no OTA support).

Upload the **non-merged** `.bin`; the `-merged` image is only for a fresh USB
flash. The device reboots into the new firmware when the upload completes.

Enabled on the `Xiao_S3_WIO_companion_radio_{usb,ble,serial,wifi}` targets. To
enable it on another ESP32 companion target, add `-D ENABLE_WIFI_OTA=1` to its
`build_flags` and `${esp32_ota.lib_deps}` to its `lib_deps`; any ESP32 target
can opt out with `-D DISABLE_WIFI_OTA=1`.

If Wi-Fi is configured but failing to connect, the companion's reconnect
attempts may briefly disturb the hotspot.

## Scope

Only these two commands are ours; everything else is IoTThinks' PowerSaving
firmware. The diff against `PowerSaving-v18` is three code files plus this
document:

- `examples/companion_radio/MyMesh.cpp` — the two CLI commands.
- `src/helpers/ESP32Board.cpp` — serve the ESP32 OTA page on the current Wi-Fi
  network when connected, otherwise the usual `MeshCore-OTA` hotspot.
- `variants/xiao_s3_wio/platformio.ini` — enable OTA on the Xiao S3 WIO
  companion targets.
