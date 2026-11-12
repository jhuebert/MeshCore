# Custom Companion Firmware

Fork-only, unpublished notes for the `custom-companion-easysky` branch. That
branch is `IoTThinks/MeshCore`'s `PowerSaving-v18` — the source behind the
EasySkyMesh PowerSaving18 release — plus a deliberately small diff so it stays
easy to rebase: it adds a handful of commands to the companion (client)
firmware's CLI, LED/buzzer muting, a set of power-tuning switches and the
radio2 second preset.

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

### Best tested profile

One-shot block for everything measured to hold up: ~5 mA idle on the Xiao S3
WIO with no phone and no Wi-Fi, 100% of 60 CLI round trips with a phone
connected, and 29/30 while the app was actively syncing. Paste the commands,
reboot, then check the reply of each `get`.

```
set radio.rxps level 6       # RX duty cycle, 10.2 symbols caught (~2 mA of the budget)
set lightsleep always        # light sleep whenever the active profile allows it
set lightsleep tick 100      # 10 loop wake-ups/s instead of 40: the biggest single win
set ble.adv 1000             # advertising interval; slowest one that still connects quickly
set ble.tx 3                 # advertising TX power
set ble.conn 200             # idle connection interval
set ble.lat 4                # idle slave latency (also the iOS-safe cap)
set ble.sync 30              # fast interval while the app is talking
set ble.quiet 5              # seconds of BLE silence before dropping to conn/lat
set radio.rxgain off         # non-boosted RX gain: 1-2 dB less sensitivity, ~0.5 mA
set led off                  # status LED
set wifi.enabled 0           # Wi-Fi off unless an OTA is coming
reboot

get lightsleep               # > always tick=100
get ble                      # > adv=1000 tx=3 conn=200 lat=4 sync=30 quiet=5
get radio.rxps              # > ...,level=6,...,effective=armed,err=0,fail=0
get radio.rxgain            # > off
get led                     # > off
get wifi.enabled             # > 0
```

`ble.adv` and `wifi.enabled` are read once at boot, which is what the `reboot`
is for; everything else applies immediately. For an OTA, put Wi-Fi back with
`set wifi.enabled 1` then `reboot`, and turn it off again afterwards.

`adv` is the only BLE value swept here (500 / 1000 / 2000); the connection
parameters are the ones the profile above was validated with - `conn 200 / lat 4`
after `quiet`, `sync 30` while the app is active. If the node also repeats
(`set repeat on`), put the RX gain back with `set radio.rxgain on`.

### Best tested profile: t1000e (nRF52 + LR1110)

A different set: the ESP32 knobs (`lightsleep`, `ble.*`, the Wi-Fi OTA page) do
not exist there. An nRF52840 already sleeps in microamps under `powersaving`
(`board.sleep()` -> `sd_app_evt_wait()`), so what is left is the receiver - and
the LR11x0 does support the same RX duty cycle. This block comes from the t1000e
board definitions and the LR11x0 wrapper rather than a bench run; the `get`s at
the bottom are what confirms it on a real node.

```
powersaving on              # MCU event-driven sleep between interrupts
set radio.rxps level 6      # RX duty cycle: the big lever on this board
set radio.rxgain off        # non-boosted gain (1-2 dB less sensitivity)
set led off                 # status LED on P0.24
set buzzer off              # also drives PIN_BUZZER_EN low
gps off                     # off unless you need fixes; it swamps everything else

get radio.rxps              # > desired=on,effective=armed,supported=yes,err=0,fail=0,level=6,...
get radio.rxgain            # > off
get led                     # > off
get buzzer                  # > off
powersaving                 # > on
gps                         # > off
```

On this board the LoRa receiver is almost the whole idle budget, so replacing
continuous RX with a ~36% duty cycle is worth much more, relatively, than the
same knob on an ESP32 companion where an awake CPU was the larger cost. Check
`effective=armed` in the `get radio.rxps` reply: `effective=continuous` means the
duty cycle could not be armed and the radio is still listening flat out.

Two traps:

- `set lightsleep ...`, `set ble.*` and the ESP32 web OTA page are ESP32-only.
  nRF52 builds still *accept* those commands and store prefs nothing reads;
  `start ota` on nRF52 puts the node into BLE DFU instead.
- The persisted `rxgain` pref is applied at boot and overrides the build-time
  `RX_BOOSTED_GAIN`, so check `get radio.rxgain` rather than assuming.

With `powersaving on`, `gps on` reports `on (powersaving)` and the GPS is
duty-cycled automatically - but a GPS fix is still far more expensive than
everything in this block combined.

### `set lightsleep off|idle|always` / `get lightsleep`

```
get lightsleep            # > off tick=10
set lightsleep idle       # OK - light sleep is now idle
set lightsleep tick 50    # OK - applies immediately
```

Turns on ESP32 **automatic light sleep** (PM/tickless): the idle task stops the
CPU between events. This is the only knob that removes the ~20 mA an awake CPU
costs; the rest are a few mA each. Applied immediately, no reboot.

The radio's IRQ pin is deliberately *not* used as a wake source: waking on it
needs a level trigger, and a level on a line that stays asserted for a whole
packet storms the CPU (the interrupt watchdog reboots the node). No loss in
practice — a received packet waits in the radio's FIFO until the loop next runs,
i.e. at most one `tick`.

`tick` is how often the idle loop wakes, in ms (default `10`, the responsive
cadence). Fewer wake-ups means less current — each one is an exit from light
sleep plus a mesh/UI/sensor pass — and nothing depends on the tick for latency: a
LoRa packet wakes the CPU through the radio IRQ, BLE connection events are
generated by the BT controller on its own timer, and the app's frames are queued
by the BT task. It stays at 10 ms automatically while the app is syncing (the
fast profile is up) or while the WiFi console is enabled, since that is serviced
from the same loop.

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
get ble            # > adv=0 tx=3 conn=0 lat=0 sync=30 quiet=10
set ble.adv 500    # OK - reboot to apply
set ble.tx -9      # OK - reboot to apply
set ble.conn 200   # OK - applies on next connection
set ble.lat 4      # OK - applies on next connection
set ble.sync 30    # OK - applies on next connection
set ble.quiet 10   # OK - applies immediately
```

| Key | Meaning | Default |
|---|---|---|
| `adv` | advertising interval, ms; `0` = stack default (20-40 ms) | `0` |
| `tx` | TX power, dBm (-24..21); ESP32 default is `+3` | `3` |
| `conn` | idle connection interval, ms; `0` = the phone decides | `0` |
| `lat` | connection events the peripheral may skip while idle | `0` |
| `sync` | connection interval while the app is talking, ms; `0` = never switch | `30` |
| `quiet` | seconds of app silence before dropping back to `conn`/`lat` | `10` |

`adv` and `tx` are read once in `SerialBLEInterface::begin()` (reboot to apply);
`conn`/`lat`/`sync` are re-requested after the link is encrypted, so they land on
the next connection. `conn`/`lat`/`sync` are a *request* — iOS in particular may
keep its own interval, Android usually honours it.

**iOS.** The request is shaped to Apple's accessory guidelines, which the phone
enforces: latency ≤ 4, max interval ≥ min + 15 ms (so a window is requested
rather than min == max), max × (latency + 1) ≤ 2 s, supervision timeout ≤ 6 s.
Your values sit inside those: `conn 200` + `lat 4` is an effective 1 s between
required events, `sync 30` is well above the 15 ms floor. What iOS may still do
is ignore the update and keep 15-30 ms / latency 0 — check with nRF Connect if
the power doesn't drop on an iPhone but does on Android.

**Adaptive timing.** A slow connection interval is what makes light sleep pay
off, but it also makes the app's first sync crawl (every round trip waits for a
connection event). So the link runs on two profiles: it starts on `sync` (fast,
latency 0) when the app connects and whenever the app sends a burst of frames, and
drops to `conn`/`lat` (slow, high latency) once the app has been quiet for `quiet`
seconds. Contacts/channels/message sync therefore run at full speed, and the idle
link costs almost nothing. `set ble.sync 0` turns the switching off and keeps the
idle timing all the time (what you want if you'd rather keep the link slow and
accept a slow first sync).

Slower advertising is the biggest BLE win when no app is connected (roughly a
few mA at 20-40 ms down to well under 1 mA at 300-600 ms); the trade-off is that
the phone takes longer to discover the device. `tx` mostly affects connection
events — -9 dBm is still enough for a room, -12 dBm for a pocket. `conn`/`lat`
cut the radio's on-time per connection event, at the cost of message latency.

Keep the set internally consistent: a slow `conn` with a large `lat` needs a
longer supervision timeout (the firmware sizes it from the two, capped at the
BLE maximum of 6 s), and `lat` above ~4 can make the app feel laggy.

### `get reset`

Not a power knob, but the reason the last boot happened - worth knowing before
blaming the mesh for missing packets:

```
get reset          # > Software reset | Brownout (low voltage) | Interrupt watchdog reset | ...
```

`Interrupt watchdog reset` is how the light-sleep wake bug in this branch was
found: a reset on every received packet looked like deafness, not a crash.

### Measuring

Change one knob at a time, hold each state for a few minutes and read a bench
PSU or inline meter; the RX duty cycle makes an average meter jump between ~3
and ~6 mA, so compare the middle of the range rather than a single sample. A
long `tick` looks calm on the meter but is not free: after every received
packet the radio sits in continuous RX until the loop picks it up, so a busy
mesh pays back part of the saving.

Measured on the Xiao S3 WIO (SX1262, SF7 / 62.5 kHz), idle - no BLE client, no
Wi-Fi, LED off. `radio.rxps` was level 8 throughout; level 6 measures the same
within the meter's resolution.

| Setting | Current |
|---|---|
| stock (`lightsleep off`, `adv 500`, `rxgain on`) | ~25-35 mA |
| `lightsleep always`, `tick 25` | ~9 mA |
| `tick 50` | ~6-7 mA |
| `tick 100` | ~6 mA |
| `tick 100` + `radio.rxgain off` | ~5-6 mA |
| `tick 100`, `rxgain off` + `ble.adv 1000` | ~5 mA |

`tick 250` and `tick 500` measured the same as `tick 100` within the meter's
resolution, so `tick 100` is the floor worth setting. `ble.adv 2000` measured
*higher* than 1000 (more 8 mA spikes), so that is the bottom of its curve too.
`radio.rxgain off` gives up 1-2 dB of sensitivity for the last mA - reasonable
for a companion that does not forward, not for a node that also repeats
(`get repeat`).

`radio.rxps` level 8 is the lossless maximum on the datasheet scale: the guard
needs `sleep <= preamble - capture`, MeshCore sends a 32-symbol preamble
(65.5 ms at SF7 / 62.5 kHz) against an 8.2-symbol capture cost, and level 8's
48.7 ms is exactly that. Levels 9-10 run past the guarantee and trade packets
for ~0.4 mA.

Level 6 is the one to actually run (10.2 symbols caught, sleep 44.6 ms). It
costs ~0.2 mA over level 8 because a shorter sleep forces a longer listen
window to satisfy the SX1262 duty-cycle timer guard (rx 25.3 ms, 36.2% duty vs
32.3%), and it buys 1.8 symbols of capture margin - worth it for a receiver that
has to hear other nodes' timing. Measured delivery was the same as level 8
(59/60 vs 89/90 over 90 commands).

Measured end to end over a 0-hop path with this profile: 59/60 CLI round trips
idle with `rxps 6` (89/90 at level 8), 60/60 with a phone connected over BLE, and
29/30 while the app was actively syncing - a flat ~1% loss from CRC/collisions,
not from BLE or the duty cycle. Median round trip ~2.0 s. No resets across 20+
minutes of soaks. The commands are collected in
[Best tested profile](#best-tested-profile).

### BLE link

The MeshCore Android app (Pixel 7) against `adv 1000`: the node appears in the
scan quickly, connecting takes up to ~1 s longer than at `adv 500` and now and
then shows the app's brief retry screen (2 of 3 reconnects were instant). Once
connected, sync is immediate - the connection-parameter request is accepted -
and the link drops to `conn 200 / lat 4` after `quiet` seconds of silence. A
message sent while the app was backgrounded arrived normally and the app
re-synced on return.


## Radio2 second preset

Radio2 lets this node **hear a second LoRa preset as well as the primary** —
the same feature the repeater branch has, adapted for the companion. One
radio, two presets: it listens on the primary, visits the secondary for
bounded windows, and goes back. It is **off by default**; with it off the
firmware behaves exactly as before.

```
radio2 869.618,62.5,8,5   # enable: <freq MHz>,<bw kHz>,<sf>,<cr>
radio2 status             # on/off, mode, timing, counters
radio2 timing             # visit lengths and split
radio2 off                # disable (keeps the preset, policy and tx knob)
```

Prerequisite, once: **the companion ships with RX duty cycle (`rxps`) on**,
and radio2 refuses to start while it is enabled. Run this first:

```
set radio.rxps off
```

(`radio2 <preset>` then fails with `Err - radio.rxps is on; set radio.rxps off
first` if you forget.) `set radio.rxps ...` is refused while radio2 is on,
with `Err - radio2 is on; set radio2 off first`, except `set radio.rxps off`
itself, which is always available as the recovery path.

### What arrives how

- **Locally originated traffic** (your app-sent DMs, group messages, adverts
  by default) goes on the **local TX preset**, default **primary**:
  `radio2 tx primary|secondary` (get/set). There is deliberately no
  `tx both` — you choose where your messages go.
- **Replies follow the preset the request arrived on** — ACKs, login and
  telemetry replies, CLI answers created while the packet is processed.
  A peer living only on the secondary network is therefore reachable.
  Exception: replies queued with a delay (the `TXT_ACK_DELAY` flood ACKs)
  are created after the receive handler returns and go on the **local TX
  preset** — accepted limitation; it matches the repeater.
- **Replies, adverts and direct traffic are never duplicated** onto the
  other preset.

### What the app sees

Nothing new: no frame changes, no extra commands. RSSI/SNR in received
messages reflect whichever preset was heard last (one radio, one noise
floor — shared). The only indication of which preset carried what:

```
radio2 status     # rx=P/S counters, tx=P/S counters, drop counters
```

and `[radio2]` log tags under a `MESH_PACKET_LOGGING=1` build.

### Repeating is a separate decision

Radio2's cross-preset policy applies to **forwarded** traffic, and the
companion only forwards with `set repeat on` (default **off**). While
repeat is off, the `radio2 policy` matrix has no effect at all — packets are
processed locally as usual. With repeat on, the companion forwards and the
matrix decides what crosses:

```
radio2 policy              # > primary=same,secondary=same
radio2 policy both         # forward flood adverts/group traffic on both presets
radio2 policy other drop   # forward on the *other* preset; drop secondary-origin traffic
```

Actions are per origin preset: `same` (default), `other` (forward on the
other preset), `both` (duplicate on both — extra airtime, opt-in), `drop`
(never forward). Only **broadcast flood** traffic may cross — `ADVERT`, and
`GRP_TXT`/`GRP_DATA` in flood routing. Direct messages, ACKs, path returns,
traces and unicast flood DMs stay on their origin preset always. Policy
drops are counted in `radio2 status` (`drop=`).

```
radio2 advert primary      # default: node adverts always on primary
radio2 advert both         # explicit duplication of node adverts on both presets
```

`radio2 advert both` also duplicates zero-hop adverts (the button path).
It is the one place a message is intentionally sent twice.

### Power and sleep notes

Scanning costs a retune (TCXO re-settle) at every preset switch, and the
radio can no longer sit in continuous low-power RX on one preset. The idle
loop is shortened while scanning: on ESP32 the `lightsleep tick` is clamped
to the scanner's next deadline (still at least 10 ms), so `set lightsleep
tick 1000` does not starve radio2; keep the tick ≤ 100 while scanning
(default 10 is fine). On nRF52 the idle path arms an RTC2 one-shot deadline
(`board.sleepMs()`) instead of sleeping with no timeout. Expect idle
current to rise while scanning is on.

### Troubleshooting

- **`Err - radio.rxps is on; set radio.rxps off first`** — see above.
- **Overlap rejected** — the secondary must not overlap the primary in
  frequency (`Err - radio2 overlaps radio`); pick a channel pair with
  separation ≥ a few bandwidths.
- **Unusable preset** — the pair can't time-share without degrading the
  primary (`Err - radio2 preset cannot be scanned ...`); pick a slower SF,
  narrower BW, or a closer SF together. `radio2 timing` reports the bound
  that failed.
- **`radio.rxps` vs `radio2`** — they are mutually exclusive and refuse to
  run together. `powersaving` (light sleep / `board.sleep(0)`) is
  compatible and required for the low-power path above.
- **Secondary packets missed in bursts** — while the primary is actively in
  use, the secondary already gets only a capped share per switch interval;
  under a busy primary that share is small by design.
- **Persistence** — the preset, policy, advert mode and local TX preset are
  stored in `/radio2_cfg` (v2; survives reboot; `radio2 off` keeps them).
  Changing the preset applies ~2 s later, after outbound queues drain, so an
  in-flight remote reply leaves on the old preset.

**Safety.** Everything the repeater's radio2 docs warn about applies here:
bridging two networks is airtime the whole cell pays for. `radio2 policy
both` doubles the flood cost of adverts and group traffic; enable it
deliberately, on a quiet channel pair, and remember the companion only does
it when `set repeat on` is also set (or for adverts, with `radio2 advert
both`).

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

The page at `/update` is a tiny form; it posts the image to `/fw` without an MD5
and reboots when it is done, so a connection error at the very end is normal.
The browser form only exists for companions (`ENABLE_WIFI_OTA`); the repeater's
ElegantOTA page is unchanged, and its `POST /update` (with the `MD5` form field)
still works on companions too - that is the form to script:
`curl -F MD5=<md5> -F update=@firmware.bin http://<ip>/update`.

ElegantOTA's own page must not be served from a companion: it is a 53 KB gzipped
PROGMEM blob, and streaming it jams this board's TCP stack - the transfer crawls
to ~100 B/s and the node stops answering ARP (still reporting `wifi.status`
`connected`) until it reboots. Uploads themselves are fine.

Enabled on the `Xiao_S3_WIO_companion_radio_{usb,ble,serial,wifi}` targets. To
enable it on another ESP32 companion target, add `-D ENABLE_WIFI_OTA=1` to its
`build_flags` and `${esp32_ota.lib_deps}` to its `lib_deps`; any ESP32 target
can opt out with `-D DISABLE_WIFI_OTA=1`.

If Wi-Fi is configured but failing to connect, the companion's reconnect
attempts may briefly disturb the hotspot.

## Scope

Only these commands and prefs are ours; everything else is IoTThinks'
PowerSaving firmware. The diff against `PowerSaving-v18` is:

- `examples/companion_radio/MyMesh.cpp` — the CLI commands and the radio2 glue
  (onRecvPacket override, TX-preset stamping, advert duplication, sleep bounds).
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
  network when connected, otherwise the usual `MeshCore-OTA` hotspot; keep one
  server per boot, and disable light sleep while an update is being served.
- `variants/xiao_s3_wio/platformio.ini` — enable OTA on the Xiao S3 WIO
  companion targets.
- `src/Packet.h`, `src/Dispatcher.{h,cpp}`, `src/helpers/StaticPoolPacketManager.{h,cpp}`,
  `src/helpers/radiolib/{RadioLibWrappers.{h,cpp},RadioProfileControl.h}` — the fork's
  radio profile support (`radio_profiles` byte, pre-TX profile selection, `retune()`).
- `src/MeshCore.h`, `src/helpers/{ESP32Board.h,NRF52Board.{h,cpp}}` — `sleepMs()` with
  a millisecond/RTC2 one-shot deadline.
- `examples/companion_radio/{Radio2.{h,cpp},Radio2Config.h,CliUtil.h,PersistUtil.h}` —
  the radio2 feature itself (example-local copy, same files as the repeater branch).
- `test/test_radio2/`, `[env:native_radio2{,_san}]` — host tests for the above.
