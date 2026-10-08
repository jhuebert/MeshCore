# Repeater Battery Gate — User Guide

A configurable low-battery cut-off for the `simple_repeater` firmware. When the
battery drops below a configurable voltage the repeater stops forwarding
packets; when the battery recovers it starts again automatically — no rebuild,
physical access, or manual intervention required.

---

## Purpose

A repeater otherwise repeats until the battery dies, leaving the node dark with
no warning. The battery gate flips that failure mode: repeating stops while
there is still charge to spare, so the node itself stays **reachable** — CLI
replies and status responses are originated by the node, never forwarded, and
the node's own adverts continue.

One honest qualification: *this* node answers for itself, but reaching it over
the mesh still needs the path to work. Remote admin and discovery replies depend
on intermediate repeaters actually relaying; a repeater on that path that is
itself suspended or filtered will stop the reply getting through. Suspension
protects the battery; it does not guarantee end-to-end reachability.

## Behaviour

- **Sampling:** the battery is measured every 30 s. The first sample happens
  immediately, but one reading is not enough to suspend — so after a reboot a
  genuinely flat battery takes roughly **30 seconds** before the gate closes.
  That debounce is deliberate, not a delay to be improved on.
- **Debounce:** **2 consecutive readings** below the suspend threshold are
  required before suspending — rejects transient sags (e.g. the voltage dip
  right after a transmission). Resuming needs only **one** reading back above
  the resume threshold.
- **Hysteresis:** suspending takes the voltage below the *suspend* threshold;
  resuming requires the voltage to rise to the *resume* threshold. The gap
  between the two prevents the gate from flapping around a single threshold.
  If no resume value is given, it defaults to suspend + 200 mV. Above 9800 mV
  that default would fall outside the accepted range, so the command is refused
  and you must pass the resume value explicitly (`Err - default resume exceeds
  10000mV; specify resume`). Nothing is clamped silently.
- **Drop counter:** a RAM-only counter of packets gated while suspended
  (resets on reboot, like the filter stats).

Suspension is checked before the content rules too, so a decryptable group
message does not get evaluated (and counted as a filter hit) while the repeater
is refusing to relay it — it is accounted as a battery drop at forwarding
admission instead, once, the same as any other packet. Traffic already queued for
transmission is not cancelled by suspending.

**Exception — the fleet channel.** Fleet-channel traffic, scripts and
acknowledgements are exempt from suspension: fleet-channel group packets relay
(they are never counted as battery drops — the exemption is the one packet the
repeater has just MAC-verified under the fleet key, never a channel-hash match),
queued fleet scripts run while
suspended, and `!ack` replies are still sent. A node in trouble is exactly the
node you need to reach; whether to spend battery on a response is the
operator's `!ack` choice, not the gate's. See [FLEET.md](./FLEET.md#battery-supersession).
Every other packet is gated exactly as described here.

While suspended, `allowPacketForward()` rejects every packet — flood, direct,
and group content alike. Everything the node originates on its own behalf
(CLI replies, status responses, adverts) is unaffected.

### What suspension does not touch

- **`set off` (`disable_fwd`)**: the manual off switch is orthogonal. It stays
  off regardless of battery state, and the battery gate never toggles it. The
  status reply's "is disabled" flag reports only the manual state.
- **Packet filter**: independent. `filter on/off` governs rule matching; the
  battery gate governs forwarding as a whole. Both may be active.
- **Fleet channel**: exempt by design — see the exception above.
- **CLI access**: unaffected — replies are originated, not forwarded. You can
  always log in and look.

## Persistence

Thresholds (and the enabled flag) persist in **`/batt_cfg`**. The suspended
flag itself is **never persisted** — it is recomputed from the live voltage on
every boot, so a reboot mid-suspend can never leave the repeater off after the
battery recovers. The consequence, stated plainly: a reboot also restarts the
two-reading debounce, so it costs you the ~30 s grace period again.

A `battery` command is written to flash about **3 seconds** later; a power cut
inside that window loses that change, not the whole file. The write itself is
staged — scratch copy, read back and checked, previous config kept as a backup,
and only then swapped in — so a crash mid-write cannot leave you without a
usable configuration. If `/batt_cfg` ever fails to load, the backup is used and
repaired on the next save.

## CLI reference

| Command | Effect |
|---|---|
| `battery` | Status: current mV reading, gate state, thresholds, drop counter |
| `battery off` | Disable the gate (no sampling, never suspends) |
| `battery <suspend-mV> [resume-mV]` | Set thresholds and enable the gate |

Thresholds are millivolts only (1..10000); the resume value must be strictly
greater than the suspend value. A rejected command always leaves the previous
settings untouched — an `OK` reply means the values were actually applied.

### Examples

```
> battery
batt 3950mV; gate on; forwarding; suspend <3400mV; resume >=3600mV; drops 0

> battery 3300 3500
OK - gate on; suspend <3300mV; resume >=3500mV

> battery
batt 3250mV; gate on; SUSPENDED; suspend <3300mV; resume >=3500mV; drops 42

> battery off
OK - battery gate off
```
