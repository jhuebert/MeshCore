# Second Radio Preset (radio2) — User Guide

Give a repeater a second LoRa preset. The same transceiver time-shares between
the normal preset (`radio`) and the new one (`radio2`), so the node can hear two
networks — but only one at a time. The primary preset is protected; the second
one is a bonus.

**Off by default.** With `radio2` off, the repeater behaves like stock
firmware: no extra radio activity, no extra timers, and no change to sleep or
power behaviour. The preset and policy you save are simply remembered.

---

## What it does

- **One radio, two presets.** The repeater listens on the primary preset most of
  the time, visits the secondary preset for short windows, and returns home.
- **Primary wins.** Traffic on the primary preset always gets priority. When the
  primary has been quiet for a while, the secondary gets a larger share.
- **Broadcast flood only.** Adverts, group text and group data heard as flood
  traffic can be repeated on the other preset. Direct messages, ACKs, path
  returns and traces never cross.
- **Replies follow the request.** A reply — CLI, ACK, path return, control
  response, fleet acknowledgement — goes back out on the preset the request
  arrived on.
- **Useful for:**
  - joining two nearby communities that use different radio presets;
  - repeating adverts and group traffic between two presets;
  - listening to a second preset without giving up the primary network.
- **Not a second receiver.** It cannot hear both presets at once. A packet on
  the secondary preset can be missed while the radio is listening on the
  primary — that is the price of one transceiver.

## Quick start

1. Connect to the repeater CLI, locally or remotely.
2. Check that it is off:

   ```text
   radio2
   > off
   ```

3. Choose a secondary preset that does not overlap the primary. The command
   **turns radio2 on** and saves the preset:

   ```text
   radio2 915,125,10,5
   OK - radio2 915,125,10,5
   ```

   The four values are **frequency MHz, bandwidth kHz, spreading factor,
   coding rate** — the same order as `set radio`.

4. Wait a couple of seconds, then check it is running:

   ```text
   radio2 status
   > on; at=primary; mode=active; split=80/20; nf=-117; sw=3; swf=0; rx=4/9; tx=1/6; both=0; bdrop=0; drop=0; abrt=0
   ```

5. Pick what should cross:

   ```text
   radio2 policy same other
   OK - radio2 policy primary=same,secondary=other
   ```

   This example keeps primary traffic on primary and sends secondary-network
   broadcast flood traffic out on primary (a one-way bridge).

6. To turn it off again, keeping the preset and policy:

   ```text
   radio2 off
   OK - radio2 off
   ```

### If radio2 refuses to start

The most common reasons are:

```text
Err - radio.rxps is on; set radio.rxps off first
Err - radio2 conflicts with extra.sf; clear extra SF first
Err - radio2 overlaps radio
Err - radio2 preset cannot be scanned without degrading radio
```

See [Troubleshooting](#troubleshooting).

## Choosing a preset

`radio2 <freq>,<bw>,<sf>,<cr>` accepts the same ranges as `set radio`:

| Value | Meaning | Accepted range |
|---|---|---|
| `freq` | Frequency | 150–2500 MHz |
| `bw` | Bandwidth | 7–500 kHz |
| `sf` | Spreading factor | 5–12 |
| `cr` | Coding rate | 5–8 |

Two rules apply:

- **It must not overlap the primary.** The repeater refuses a preset whose
  tuned band overlaps `radio` (half of each bandwidth, plus a 6 kHz guard).
  Separate frequencies are required, not merely different spreading factors.
- **The pair must be scannable.** The visit lengths are calculated so a
  primary preamble can still be caught after a secondary visit, and a secondary
  preamble can still be caught after a primary visit. Very fast presets, or
  pairs with very different airtime, can fail this test and are refused rather
  than silently degrading the primary.

`radio2 timing` shows the numbers the scheduler will use. Values depend on the
current mode; this example is idle mode (`mode=idle`, after 30 seconds without
primary traffic):

```text
radio2 timing
> primary=49ms; secondary=73ms; split=39/61; switch=3ms; mode=idle
```

If the pair cannot be used, the reply names the bound that failed. It may be
either direction:

```text
> unusable: secondary capture 49ms > primary-safe 20ms
> unusable: primary capture 16ms > secondary-safe 8ms
```

The numbers depend on the radio family: an LR11x0 chip needs a few more preamble
symbols than an SX126x, so a pair that is accepted on one can be refused on the
other. `unusable: no preset configured` means no secondary preset has been set
yet.

The switch time defaults to a conservative 3 ms. Retuning is hardware-specific;
the default is deliberately not aggressive.

## Command reference

| Command | What it does |
|---|---|
| `radio2` | Same as `radio2 status`. |
| `radio2 status` | Show whether radio2 is on, which preset is listening, and the counters. |
| `radio2 timing` | Show the visit and split calculations for the current pair. |
| `radio2 <freq>,<bw>,<sf>,<cr>` | Save a secondary preset and turn radio2 on. |
| `radio2 off` | Turn radio2 off; the preset and policy are kept. |
| `radio2 policy` | Show the two policy actions. |
| `radio2 policy <action>` | Set the same action for both origins. |
| `radio2 policy <primary> <secondary>` | Set the action for primary-heard and secondary-heard packets separately. |
| `radio2 advert` | Show the advert setting. |
| `radio2 advert primary\|both` | Send local adverts on the primary only, or once on each preset. |

Actions are `same`, `other`, `both` and `drop`; see
[How packets are handled](#how-packets-are-handled).

### Preset and switch commands

```text
radio2 915,125,10,5
OK - radio2 915,125,10,5

radio2 off
OK - radio2 off
```

`radio2 off` does not erase the preset, and the radio returns to the primary
preset after the deferred apply. Re-running `radio2 <preset>` turns it back on.
There is no separate `on` command: setting a preset is enabling.

The radio change is **deferred by about two seconds** and waits until no
outbound packet is queued. That lets a remote admin's reply go out on the old
settings before the radio retunes. While waiting, status says:

```text
> on; apply=pending; at=primary
```

### Policy commands

```text
radio2 policy
> primary=same,secondary=same

radio2 policy other
OK - radio2 policy primary=other,secondary=other

radio2 policy same other
OK - radio2 policy primary=same,secondary=other
```

The first word is the action for packets heard on the **primary** preset; the
second is for packets heard on the **secondary**. One word sets both.

### Advert commands

```text
radio2 advert
> primary

radio2 advert both
OK - radio2 advert both
```

`primary` (the default) keeps this repeater's own adverts on the primary
preset. `both` sends one advert on each preset, making the node visible in both
networks. Received adverts are handled by the policy matrix like any other
broadcast flood packet.

`radio2 advert both` is inert while radio2 is off.

### Status fields

A full status line looks like:

```text
on; at=primary; mode=idle; split=39/61; nf=-117; sw=12; swf=0; rx=4/9; tx=1/6; both=0; bdrop=0; drop=0; abrt=0
```

| Field | Meaning |
|---|---|
| `on` / `off` | Whether radio2 is enabled. |
| `apply=pending` | The RF change is waiting for the deferred apply. |
| `at=primary` / `at=radio2` | Which preset the radio is listening on right now. |
| `mode=active` | The primary was heard in the last 30 seconds; primary gets priority. |
| `mode=idle` | The primary has been quiet for 30 seconds; secondary gets its maximum safe share. |
| `lossy` | On the active-mode status line: the primary visit is long enough that a secondary preamble may not survive it. This is expected in active mode. |
| `split=P/S` | Percentage of listening time given to primary and secondary in the current mode. |
| `nf` | The shared noise floor. There is one floor for both presets, as before. |
| `sw` / `swf` | Successful retunes / failed retunes. |
| `rx=P/S` | Packets received on primary / secondary. |
| `tx=P/S` | Transmissions attempted on primary / secondary. |
| `both` | Duplicate copies queued by a `both` policy. |
| `bdrop` | Duplicate copies that could not be queued because the packet pool was full. |
| `drop` | Packets not repeated because of a `drop` policy. |
| `abrt` | Secondary receives abandoned because they ran past the one-packet airtime bound. |

Other forms:

```text
off
off; conflict=radio.rxps
off; conflict=extra.sf
off; conflict=preset
off; conflict=timing
off; conflict=switch
off; paused
```

`off; paused` means a temporary radio change is in progress; radio2 resumes
after the temporary settings revert.

### Error messages

| Reply | What to do |
|---|---|
| `Err - usage: status\|timing\|<freq>,<bw>,<sf>,<cr>\|off\|policy [a] [b]\|advert primary\|both` | The command form was not recognised. |
| `Err - radio2 preset must be <freq>,<bw>,<sf>,<cr>` | Supply exactly four comma-separated values. |
| `Err - radio2 policy must be same\|other\|both\|drop` | Use one of the four action names. |
| `Err - radio2 advert must be primary\|both` | Use `primary` or `both`. |
| `Err - radio2 overlaps radio` | Choose a frequency farther from the primary. |
| `Err - radio2 preset cannot be scanned without degrading radio` | The pair is too fast or too different; choose closer/slower presets. |
| `Err - radio.rxps is on; set radio.rxps off first` | Turn the receive duty cycle off before enabling radio2. |
| `Err - radio2 is on; set radio2 off first` | Turn radio2 off before turning `radio.rxps` on or changing its level. |
| `Err - radio2 conflicts with extra.sf; clear extra SF first` | Clear the side-detector extra SF settings first. |
| `Err - tempradio active; wait for it to revert` | Wait for the temporary radio preset to revert. |

## How packets are handled

Each received packet has an **origin**: primary or secondary. The policy decides
what happens to it.

| Action | Meaning |
|---|---|
| `same` | Repeat only on the preset it was heard on. |
| `other` | Repeat only on the other preset. |
| `both` | Transmit two copies, one on each preset. |
| `drop` | Do not repeat it. The node may still process it locally (for example, answer a CLI request). |

The policy is chosen separately for each origin:

| Heard on | `same` | `other` | `both` | `drop` |
|---|---|---|---|---|
| primary | TX primary | TX secondary | TX both | no TX |
| secondary | TX secondary | TX primary | TX both | no TX |

`both` is two transmissions, not simultaneous. It uses two packet-pool slots
while queued, two airtime charges, and the same duty-cycle budget.

### What may cross profiles

Cross-profile actions (`other` and `both`) apply only to **broadcast flood**
traffic:

- adverts, including zero-hop adverts;
- group text with flood routing;
- group data with flood routing.

Everything else stays on the preset it was heard on:

- direct and transport-direct packets;
- ACKs;
- path returns;
- traces;
- anonymous requests;
- direct messages, requests, responses and unicast flood DMs;
- control/discovery packets;
- raw custom packets.

If the configured action is `other` or `both` but the packet is not
crossable, the effective action is `same`. `drop` still drops it.

That rule is deliberate. A direct message flooded across profiles would need its
ACK or path return to cross too, and the two sides can disagree about paths,
timing and which preset is primary. Keeping private traffic on its origin
avoids that trap.

### Replies

Replies always go back on the preset the request arrived on:

- CLI replies;
- ACKs created by the node;
- path returns;
- control and discover responses;
- fleet acknowledgements, including after `!delay` or `!at`.

There is no “reply preset” setting: origin is the rule.

Locally originated traffic:

- adverts follow `radio2 advert` (primary by default);
- battery and fleet alerts go on the primary preset;
- scheduled fleet scripts originate on primary, unless the script is a reply to
  a received fleet message, in which case the captured origin is used.

### Filters

The packet filter applies to both presets equally. A packet heard on either
preset goes through the same rule list. There is no per-profile filter rule in
this version, so a rule that drops a sender drops it on both.

## How listening time is shared

The primary preset is the node's home. The radio visits the secondary only for
bounded periods.

- **Active mode:** for 30 seconds after any primary packet, the primary gets a
  long visit and the secondary gets a short one. The secondary may miss packets
  that start between visits. This is the “primary is busy” mode.
- **Idle mode:** after 30 seconds without a primary packet, the primary gets the
  minimum visit needed to catch a preamble and the secondary gets the largest
  share that still lets a primary preamble survive to the next primary visit.
- **Primary in progress:** if a primary packet is already being received when a
  secondary visit would start, the visit is deferred. Primary traffic always
  wins.
- **Secondary in progress:** once a secondary packet has started, it is allowed
  to finish, in both modes. Switching away mid-packet would lose it, and the
  primary stands to miss only that one packet. The wait is bounded by the
  largest packet the secondary preset can carry; a receive that somehow exceeds
  that bound is abandoned and counted as `abrt`.
- **Transmit:** the radio never abandons a transmission to switch presets. The
  scheduler waits for it. A transmit targeted at a preset retunes first, then
  sends.

There is no way to listen on both presets at once. The visit lengths are chosen
so the *preamble* of a packet on the other preset survives until the next visit,
not so the whole packet is heard.

## Common setups

### Two separate networks, one repeater

The default:

```text
radio2 policy same same
```

Each network repeats only within itself. This is a repeater that happens to
listen to two networks, not a bridge.

### One-way bridge

From the secondary network into the primary network:

```text
radio2 policy same other
```

Primary traffic stays on primary; broadcast flood traffic heard on secondary is
repeated on primary.

The other direction:

```text
radio2 policy other same
```

### Two-way bridge, one copy per direction

```text
radio2 policy other other
```

Each broadcast flood packet heard on one preset is repeated on the other, and
not repeated back into its origin. This is usually the cleanest bridge.

### Two-way bridge, keep the local copy too

```text
radio2 policy both both
```

Each packet is repeated on both presets — one copy stays in the origin network,
one crosses. This doubles the traffic and the airtime. Use it only when the
origin network genuinely needs the local repeat.

> **Use one bridge.** If two repeaters both use `both` between the same pair of
> networks, traffic can be duplicated. MeshCore will suppress duplicates it has
> already seen and hop limits bound loops, but `other` on a single bridge is the
> safer design.

### Listen but do not repeat

```text
radio2 policy same drop
```

The secondary is monitored for local processing, but nothing heard there is
repeated. The node still answers CLI requests and can run fleet jobs heard on
that preset.

### Be visible in both networks

```text
radio2 advert both
```

One advert on each preset. Received adverts still follow the policy matrix.

## Interactions with other features

### Receive duty cycle (`radio.rxps`)

`radio.rxps` and radio2 cannot both be enabled. The commands refuse in both
directions:

- `radio2 <preset>` is refused while RXPS is enabled;
- `set radio.rxps on` (or a level change) is refused while radio2 is enabled;
- `set radio.rxps off` is always allowed.

If both settings are somehow saved, radio2 does not start; the primary radio
continues with RXPS, and `radio2 status` reports:

```text
off; conflict=radio.rxps
```

Turn RXPS off and re-enable radio2 (or reboot) to recover.

### Powersaving (light sleep)

`powersaving` and radio2 can be used together on **nRF52 and ESP32** boards.
The scheduler gives the MCU a wake deadline so it can sleep between visits.

On other board families there is no sub-second timed wake: radio2 still works,
but the MCU busy-polls and `powersaving` saves no power. For a solar or battery
node, use an nRF52 or ESP32 board.

With radio2 off, the sleep path is exactly the stock path.

### `extra.sf` side detectors

Side detectors are primary-only in this version. If `extra.sf` is configured,
radio2 refuses to start and `radio2 <preset>` is refused:

```text
Err - radio2 conflicts with extra.sf; clear extra SF first
```

Clear the extra SF settings with the core command before enabling radio2.

### Temporary radio (`set radio` / `tempradio`)

A temporary primary preset pauses radio2. It retunes the radio to primary,
applies the temporary settings, and revalidates the secondary pair when the
temporary settings revert. If the pair is no longer usable, radio2 stays off
and reports the reason.

Changing radio2's preset or turning it off is refused while a temporary radio
change is pending.

### Changing the primary preset (`set radio`)

`set radio` takes effect on reboot. At boot, radio2 validates the saved
secondary preset against the new primary. If they overlap or the pair is not
scannable, radio2 stays off and reports the conflict in status. The primary is
never modified to accommodate radio2.

### Packet filter

See [Filters](#filters). The filter’s rate limiter also applies to both
presets.

### Battery gate and fleet

The battery gate and fleet behaviour are unchanged:

- a policy `drop` is a deliberate routing decision, checked before the battery
  gate and the forwarding filter check, and counted as a policy drop. Decrypted
  group content still passes through the filter’s content rules (the same as
  stock), and fleet jobs still run;
- fleet-channel group traffic still relays while the battery gate is suspended;
- fleet acknowledgements use the preset the script arrived on, even after a
  delay or a scheduled `!at` run;
- battery and fleet alerts originate on primary.

### Noise floor

There is one shared noise floor, as before. There is no per-preset floor. While
the radio is visiting both presets, `int.thresh` (if enabled) sees the shared
value and may be approximate. `radio2 status` shows the current value.

### MQTT observer

The observer captures traffic heard on both presets, including packets the node
will not forward. The current observer message format does not label which
preset a packet arrived on.

## Persistence and restarts

- radio2 saves its settings in its own file, `/radio2_cfg`.
- The preset, policy, advert setting and enabled flag survive reboots.
- Saves are staged with a backup; a torn write falls back to the last good copy.
- `radio2 off` keeps the preset and policy.
- The RF change is deferred by about two seconds and waits for the outbound
  queue to drain, so a remote reply can leave before the retune.
- If the config file is missing or invalid, radio2 starts off.
- Flashing stock MeshCore later ignores the file.

## Defaults

| Setting | Default |
|---|---|
| radio2 | off |
| Policy, primary origin | `same` |
| Policy, secondary origin | `same` |
| Adverts | `primary` |
| Replies | origin preset |
| Filters | both presets equally |
| RXPS | must be off |
| Powersaving | allowed (nRF52/ESP32) |
| Active hold | 30 seconds |
| Retune estimate | 3 ms |
| Deferred apply | about 2 seconds |
| Overlap guard | 6 kHz |

## Limits and tradeoffs

- **One transceiver.** The node cannot receive both presets at once. The
  secondary is a bonus and may miss traffic.
- **Primary protection can cost a primary packet.** A secondary packet that has
  started is allowed to finish, up to one maximum-length packet, so the primary
  can be blind for that long. The scheduler still prefers the primary between
  packets.
- **Idle hold lets secondary traffic finish.** In idle mode a long secondary
  packet is allowed to finish too, which can extend the primary's blind time
  after a long primary silence.
- **Only broadcast flood crosses.** Direct messages, ACKs, path returns,
  traces and unicast flood DMs stay on their origin preset.
- **No per-preset filters or noise floor** in this version.
- **`both` duplicates traffic.** Use one bridge or `other` where possible.
- **`powersaving` saves nothing on non-nRF52/ESP32 boards** when radio2 is
  scanning.
- **The wio-e5 repeater no longer fits its flash budget.** Its 224 KB STM32
  part was already at ~98.5 % before radio2; this feature takes it over the
  limit even with the release build's library LTO, so the release has no asset
  for that target. Use a board with more flash.
- **Retune time is a conservative estimate.** Hardware may be faster or slower;
  the 3 ms default is deliberately safe.

## Troubleshooting

| Symptom | Likely cause | What to do |
|---|---|---|
| `radio2` says `off` | Not enabled | Set a preset with `radio2 <freq>,<bw>,<sf>,<cr>`. |
| `off; conflict=radio.rxps` | RXPS is enabled | `set radio.rxps off`, then re-enable radio2 or reboot. |
| `off; conflict=extra.sf` | Side detectors configured | Clear `extra.sf`, then re-enable or reboot. |
| `off; conflict=preset` | Overlap or bad preset after a primary change | Choose a new secondary preset, or fix the primary. |
| `off; conflict=timing` | Pair cannot be scanned safely | Use closer/slower presets. |
| `off; conflict=switch` | Repeated retune failures | Re-check the preset/antenna; change the config to retry. |
| `off; paused` | Temporary radio change in progress | Wait for it to revert. |
| `apply=pending` | Deferred apply waiting | Wait about two seconds and re-check. |
| Nothing crosses | Policy is `same`, or the packet is not broadcast flood | Check the policy and the packet class. |
| Replies arrive on the “wrong” preset | The reply should follow origin | Check that the request was heard on the preset you expect. |
| Secondary traffic is often missed | Active mode, or presets very different | Keep primary traffic lower, or accept that the secondary is best-effort. |
| `radio2 timing` says unusable | Pair too fast or too different | Choose a slower/closer pair. |

## Safety and operational notes

- radio2 bridges **broadcast flood** traffic between two radio presets. Group
  messages and adverts from one network can become visible in the other. Make
  sure you are authorised to carry that traffic.
- The feature does **not** bridge direct messages, ACKs, path returns or traces,
  so private conversations stay on their origin preset.
- Use the packet filter to restrict what may cross, and prefer `other` over
  `both` when one bridge is enough.
- Holding the CLI (serial or remote admin) is still the authority; radio2 does
  not add or remove any administrator credential.
