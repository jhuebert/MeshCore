# Repeater Power Profile — User Guide

Three knobs that cut an idle repeater's current by an order of magnitude:
**light sleep** (`powersaving`), the radio's **receive duty cycle**
(`radio.rxps`) and the **CPU clock** (`cpu.freq`). All three are runtime
settings — persisted, tunable over the CLI, no rebuild required.

Measured on a Seeed XIAO ESP32-S3 + Wio-SX1262 (SF7 / 62.5 kHz, US preset,
bench PSU, WiFi off):

| State | Current |
|---|---|
| Stock: 240 MHz, continuous RX, no sleep | 54 mA |
| `powersaving on` (light sleep) | 6 mA |
| + `radio.rxps` level 6 (73 % RX duty, 16-symbol profile) | 4 mA |
| + `cpu.freq 80` | 4 mA idle (3.75 mA on a DMM), 32 mA awake |

The CPU knob does not show at idle — the CPU is asleep almost all the time —
but it cuts every awake window roughly in half (54 → 32 mA in the two-minute
window after boot, and during any packet work).

## Attribution

Only one of the three knobs is this fork's own work, and the credit matters:

- **`powersaving` (light sleep)** is **upstream MeshCore**
  ([meshcore-dev/MeshCore](https://github.com/meshcore-dev/MeshCore)) and is
  used here unchanged.
- **`radio.rxps` (the receive duty cycle)** comes from **EasySkyMesh** —
  [IoTThinks/MeshCore](https://github.com/IoTThinks/MeshCore), branch
  `PowerSaving-v18` (the source behind the EasySkyMesh PowerSaving18 release,
  by Kevin Le). The policy, level ladder, preamble guard, CLI and radio-wrapper
  integration are theirs; this fork's copies of `RXPowerSaving.*` and
  `RXPowerSavingCLI.*` are byte-identical to their tree, and the host tests are
  their cases wired into this repo's test runner.
- This fork added the port into the repeater/observer firmware, the persisted
  **`cpu.freq`** setting, the `PowerProfile` glue, the pre-feature defaults,
  and this guide.

---

## Purpose

A repeater's radio has to listen continuously, and continuous RX is what keeps
a solar or battery node from surviving a cloudy week. The profile attacks the
two costs that dominate idle:

- **the CPU**, which is idle but awake (light sleep removes it), and
- **the radio's RX**, which only has to listen for the *preamble* of an
  incoming packet to know a packet is arriving (the duty cycle removes the
  time spent listening when nothing is on air).

Each knob is independent and each can be reverted with one command. The
defaults reproduce the pre-power-feature behaviour exactly — light sleep off,
RX duty cycle off (continuous RX), CPU at 240 MHz — so an existing installation
sees no change until it opts in. `set radio.rxps on` and `powersaving on` are
one command each; `cpu.freq 0` restores the board default.

## Behaviour

### Light sleep — `powersaving`

With `powersaving on`, the node enters ESP32 light sleep whenever it has no
pending work. The first sleep waits **2 minutes** after boot; afterwards the
loop sleeps in **30 s** chunks and wakes earlier on two events:

- a LoRa packet (the radio's IRQ line is a GPIO wake source), and
- any scheduled job that comes due.

The radio stays in RX while the MCU sleeps, so packets are not missed — the
chip wakes on the preamble/first IRQ and the message is read normally. On
NRF52 boards the same setting uses the platform's low-power path instead.

`powersaving off` returns the node to a busy loop at the board's default
clock. A reboot re-reads the setting; the sleep itself starts again 2 minutes
after boot.

### RX duty cycle — `radio.rxps`

The SX126x, LLCC68 and LR11x0 radios can listen in **duty-cycled windows**:
the radio wakes for a short window, looks for a preamble, and sleeps if there
is none. Once a preamble is detected the chip stays awake for the whole packet,
so a window only has to be shorter than the sender's preamble — the rest of the
time is free.

The setting is expressed as a **level**, and a level is a promise about the
preamble, not a raw timer value:

> Level *N* means: at least *N* symbols of the sender's preamble land inside an
> open RX window, in the worst case where the preamble starts the instant a
> window closes.

The sleep period follows directly (`sleep = preamble − catch`), and the chip's
capture cost (≈6 symbols on SX126x, ≈8 on LR11x0) is a validation floor: no
level may ask for fewer symbols than the radio needs to latch a preamble.
Levels run **1–8** on the guarded scale. `9` (`overdrive`) and `10`
(`riskyWorkingMax`) run outside the datasheet's timer condition on purpose:
`9` is measured lossless and a few percent cheaper, `10` is the measured
working edge and drops roughly 2 % of packets. Neither is a default.

The profile has to match the *sender's* preamble length, because that is what
the receiver can see. MeshCore sends 32 symbols at SF ≤ 8 and 16 above, so the
level scale has a ladder per profile and the preset commands assume the
worst case (16). `max preamble 32` and `level <N> preamble <32>` pick the
longer, cheaper profile when the whole network is on it.

While RXPS is armed the driver re-arms the cycle after every packet, before
transmit, and after a radio reconfiguration. With RXPS enabled the noise floor
is sampled during a short continuous-RX calibration instead of in passing, so
`stats-radio` keeps reporting a meaningful floor.

`radio.rxps off` returns the radio to continuous RX immediately. Changing the
radio parameters (`set radio …`) re-normalises the periods for the new spreading
factor and bandwidth and reports `reboot to apply` as usual.

### CPU frequency — `cpu.freq`

`set cpu.freq <mhz>` caps the CPU clock while the node is awake. Supported
values are **40, 80, 160 and 240**; `0` restores the frequency the board booted
with. The setting is applied immediately and again at boot.

Repeater work is I/O-bound: packet handling is dominated by SPI and airtime,
and the filter's pattern matcher is bounded by a **step budget**, not a time
budget — at 80 MHz the pathological worst case grows from about 1 ms to about
3 ms against packets that hold the channel for hundreds of milliseconds. WiFi
on the ESP32-S3 needs at least 80 MHz, so 80 is the useful floor for observer
builds.

A UART console is re-initialised after a clock change so its baud rate follows
the new bus clock; USB CDC consoles are unaffected.

## CLI reference

| Command | Effect |
|---|---|
| `powersaving` | Status: `on` / `off` |
| `powersaving on` / `powersaving off` | Enable / disable light sleep |
| `get radio.rxps` | Duty-cycle status: desired/effective, armed, supported, level, catch margin, preamble, periods |
| `set radio.rxps off` | Continuous RX |
| `set radio.rxps on` (or `conservative`) | Level 3, 16-symbol profile |
| `set radio.rxps balanced` | Level 6, 16-symbol profile |
| `set radio.rxps max` | Level 8 (guarded floor), 16-symbol profile |
| `set radio.rxps overdrive` | Level 9 — lossless but outside the datasheet timer condition |
| `set radio.rxps riskyWorkingMax` | Level 10 — measured working edge, drops ~2 % |
| `set radio.rxps level <1-10>` | Pick a level directly |
| `set radio.rxps level <1-10> preamble <16\|32>` | Pick a level and the preamble profile |
| `set radio.rxps max preamble <16\|32>` | Preset with an explicit profile |
| `set radio.rxps <rx_us> <sleep_us>` | Raw periods (advanced; no guard) |
| `get cpu.freq` | Status: `> 80` or `> default` |
| `set cpu.freq <0\|40\|80\|160\|240>` | Cap the CPU clock (`0` = board default) |

`get radio.rxps` reports the levels the hardware actually runs with, not only
the requested ones: `effective=armed` means the cycle is programmed, `err`
is the last driver error, `fail` counts consecutive arm failures, and
`catch=N.N` is the preamble margin the configured periods really give.

## Persistence

All three live in the normal preference file (`/prefs.json`), so they survive
reboots and OTA updates:

| Key | Setting |
|---|---|
| `power.pwr_sav_en` | light sleep |
| `power.cpu_mhz` | CPU frequency (`0` = default) |
| `radio.rxps_en`, `radio.rxps_rx_us`, `radio.rxps_sleep_us`, `radio.rxps_level`, `radio.rxps_preamble` | duty cycle |

A full flash erase returns every one of them to the defaults. The duty-cycle
periods are stored alongside the level, so a stored level still reports the
periods it was calculated with.

## What this does not do

- **No deep sleep.** A repeater has to hear packets; the MCU can only sleep
  between them. `poweroff` (deep sleep, wake on power cycle only) exists for
  parking a node, not for running one.
- **No wireless interface changes.** This profile does not touch WiFi power
  save or the observer's broker connection; an observer that keeps a station
  link will spend more than the numbers above.
- **No flash/PSRAM rail power-down.** The ESP32 light sleep used here does not
  cut VDD_SPI; the Arduino core is not built with
  `CONFIG_ESP_SLEEP_POWER_DOWN_FLASH`, which is worth roughly another 0.5–1 mA
  but needs a framework rebuild.
- **No network-level tricks.** A longer transmit preamble would let every node
  sleep proportionally longer, but it costs airtime for every sender on the
  mesh, so it is not a per-node setting.

## Choosing a level

| Situation | Setting |
|---|---|
| Battery/solar node, mixed network | `balanced` (level 6) — the default |
| Best battery life, mixed network, can accept a rare miss | `max` (level 8) |
| Whole network on 32-symbol preambles (SF ≤ 8) | `set radio.rxps level 6 preamble 32` — much cheaper than the 16-symbol default |
| Need every packet, radio must never sleep | `off` |

Measured delivery at level 6 on a 0-hop path: **8/8** direct CLI round trips
acknowledged. The guard math is what makes that hold at every level; the
capture margin shrinks as the level rises, which is why 9 and 10 are named
rather than numbered into the default scale.

## Examples

```
> powersaving
on

> get radio.rxps
> desired=on,effective=armed,supported=yes,err=0,fail=0,level=6,catch=10.2,preamble=16,rx=25313,sleep=11882

> set radio.rxps balanced
OK - on,level=6,preamble=16,rx=25313,sleep=11882

> set cpu.freq 80
OK - 80 MHz

> get cpu.freq
> 80
```
