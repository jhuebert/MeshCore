# Custom Companion Firmware

Fork-only, unpublished notes for the `custom-companion-easysky` branch. That
branch is `IoTThinks/MeshCore`'s `PowerSaving-v18` — the source behind the
EasySkyMesh PowerSaving18 release — plus a deliberately small diff so it stays
easy to rebase: it adds a handful of commands to the companion (client)
firmware's CLI, LED/buzzer muting and a set of power-tuning switches.

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

The commands below are reached the same way on every companion build: the
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

## `set buzzer on|off` / `get buzzer`

Mute or unmute the received-message buzzer — the same flag the triple-press
button combo flips on boards that have a button, but reachable from the CLI:

```
get buzzer         # > on     /  > off
set buzzer off     # OK - buzzer is now OFF
set buzzer on      # OK - buzzer is now ON
```

Only `off` turns it off; anything else is treated as `on`, as with
`set repeat`. Persisted as `buzz_q` and applied to the buzzer by the UI loop on
its next tick, so no reboot — and no button — is needed.

## `set led on|off` / `get led`

Stop the status LED from blinking:

```
get led            # > on     /  > off
set led off        # OK - status LED is now OFF
set led on         # OK - status LED is now ON
```

`on` (the default) is the normal heartbeat: a short flash every 4 s, green when
idle, amber while messages are unread, blue while the app is connected, red on
low battery; boards with an RGB status LED also run their boot/new-message
colour sweeps. `off` silences all of it — the same effect as the
`-D PIN_STATUS_LED`-less builds, but per device and reversible. Persisted as
`led_q` and re-read by the UI every loop, so it takes effect immediately
(no reboot).

## Power tuning

The ESP32 companions are ~20 mA above a repeater because the repeater puts its
MCU into light sleep between packets (`board.sleep()`) while a companion never
does: it has to stay awake for the BLE connection events. Everything below is
persisted and off-by-default (except where noted), so each knob can be tried on
its own with a meter.

### `set lightsleep off|idle|always` / `get lightsleep`

```
get lightsleep       # > off
set lightsleep idle  # OK - light sleep is now idle
```

Turns on ESP32 **automatic light sleep** (PM/tickless): the idle task stops the
CPU between events and wakes for each connection event or for a LoRa packet
(the radio's DIO1 pin is armed as a wake source for the duration of the idle wait
only — it is level-triggered, so leaving it armed would turn every TX into an
interrupt storm). This is the only knob that removes the ~20 mA an awake CPU
costs; the rest are a few mA each. Applied immediately, no reboot.

| Level | Behaviour |
|---|---|
| `off` (default) | never sleeps |
| `idle` | sleeps only while nothing is connected (no BLE link, no TCP client) |
| `always` | also sleeps between the connection events of a live link |

`idle` is the safe one to start with: the big win is while the app is away, and
an active session can never be harmed. `always` goes further but leans on the
BT controller's modem sleep keeping the link alive through every connection
event — worth measuring, not worth trusting blind, because a dropped link also
takes away the CLI you would use to turn it back off (USB flashing or a
configured WiFi console remain).

- Enabling it implies the BT controller's modem sleep even when `powersaving` is
  off. If the platform refuses the PM configuration the firmware logs why and
  stays awake rather than half-applying the profile.
- **USB/Serial companions:** light sleep powers down the USB PHY, so the console
  drops and re-enumerates. Don't use it on the USB/Serial builds — BLE and WiFi
  companions only. (The `usb`/`serial` presets are the reason this is opt-in.)
- With the build's `LPCLK = MAIN_XTAL` (no 32 kHz crystal on these boards) BT
  sleep keeps the main crystal running, so expect ~1-2 mA sleeping rather than a
  µA state. Still an order of magnitude below running.
- The idle loop still ticks every 10 ms, so button presses and queued traffic
  stay responsive.

### BLE: `set ble.adv` / `set ble.tx` / `set ble.conn` / `set ble.lat`

```
get ble            # > adv=0 tx=3 conn=0 lat=0
set ble.adv 500    # OK - reboot to apply
set ble.tx -9      # OK - reboot to apply
set ble.conn 200   # OK - applies on next connection
set ble.lat 4      # OK - applies on next connection
```

| Key | Meaning | Default |
|---|---|---|
| `adv` | advertising interval, ms; `0` = stack default (20-40 ms) | `0` |
| `tx` | TX power, dBm (-24..21); ESP32 default is `+3` | `3` |
| `conn` | preferred connection interval, ms; `0` = the phone decides | `0` |
| `lat` | connection events the peripheral may skip | `0` |

`adv` and `tx` are read once in `SerialBLEInterface::begin()` (reboot to apply);
`conn`/`lat` are re-requested after the link is encrypted, so they land on the
next connection. `conn`/`lat` are a *request* — iOS in particular may keep its
own interval, Android usually honours it.

Slower advertising is the biggest BLE win when no app is connected (roughly a
few mA at 20-40 ms down to well under 1 mA at 300-600 ms); the trade-off is that
the phone takes longer to discover the device. `tx` mostly affects connection
events — -9 dBm is still enough for a room, -12 dBm for a pocket. `conn`/`lat`
cut the radio's on-time per connection event, at the cost of message latency.

Keep the set internally consistent: a slow `conn` with a large `lat` needs a
longer supervision timeout (the firmware sizes it from the two, capped at the
BLE maximum of 6 s), and `lat` above ~4 can make the app feel laggy.

### Measuring

Change one knob at a time, let the display time out (15 s) and the radio settle,
then read a USB power meter or an inline meter for a minute. The order that
usually shows the biggest steps is `lightsleep`, then `adv`, then WiFi off, then
`tx`/`conn`/`lat`.

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

Only these commands and prefs are ours; everything else is IoTThinks'
PowerSaving firmware. The diff against `PowerSaving-v18` is:

- `examples/companion_radio/MyMesh.cpp` — the CLI commands.
- `examples/companion_radio/NodePrefs.h` — the `led_q`, `lsleep` and
  `ble.{adv,tx,conn,lat}` prefs (`buzzer_quiet` already exists upstream; `set
  buzzer` only reaches it from the CLI).
- `examples/companion_radio/main.cpp` — apply the power profile and pass the BLE
  prefs to the interface.
- `examples/companion_radio/ui-{orig,new,tiny}/UITask.cpp` — honour `led_q` in
  the status LED handler and apply `buzzer_quiet` to the buzzer each loop.
- `src/helpers/PowerSaving.{h,cpp}` — companion light sleep (PM config plus the
  radio wake source) next to the existing modem-sleep/DFS profile.
- `src/helpers/esp32/SerialBLEInterface.{h,cpp}` — advertising interval, TX
  power and preferred connection parameters.
- `src/helpers/ESP32Board.cpp` — serve the ESP32 OTA page on the current Wi-Fi
  network when connected, otherwise the usual `MeshCore-OTA` hotspot.
- `variants/xiao_s3_wio/platformio.ini` — enable OTA on the Xiao S3 WIO
  companion targets.
