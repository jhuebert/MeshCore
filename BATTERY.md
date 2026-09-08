# Repeater Battery Gate — User Guide

A configurable low-battery cut-off for the `simple_repeater` firmware. When the
battery drops below a configurable voltage the repeater stops forwarding
packets; when the battery recovers it starts again automatically — no rebuild,
physical access, or manual intervention required.

---

## 1. Purpose

A repeater otherwise repeats until the battery dies, leaving the node dark with
no warning. The battery gate flips that failure mode: repeating stops while
there is still charge to spare, so the node stays **reachable** — CLI replies
and status responses are originated by the node itself, never forwarded, and
the node's own adverts continue. An admin can log in over the mesh at any time,
check the voltage, and adjust or disable the gate.

## 2. Behaviour

- **Sampling:** the battery is measured every 30 s (first sample immediately
  after boot, so the gate is correct right away).
- **Debounce:** **2 consecutive readings** below the suspend threshold are
  required before suspending — rejects transient sags (e.g. the voltage dip
  right after a transmission).
- **Hysteresis:** suspending takes the voltage below the *suspend* threshold;
  resuming requires the voltage to rise to the *resume* threshold. The gap
  between the two prevents the gate from flapping around a single threshold.
  If no resume value is given, it defaults to suspend + 200 mV.
- **Drop counter:** a RAM-only counter of packets gated while suspended
  (resets on reboot, like the filter stats).

While suspended, `allowPacketForward()` rejects every packet — flood, direct,
and group content alike. Everything the node originates on its own behalf
(CLI replies, status responses, adverts) is unaffected.

### What suspension does not touch

- **`set off` (`disable_fwd`)**: the manual off switch is orthogonal. It stays
  off regardless of battery state, and the battery gate never toggles it. The
  status reply's "is disabled" flag reports only the manual state.
- **Packet filter**: independent. `filter on/off` governs rule matching; the
  battery gate governs forwarding as a whole. Both may be active.
- **CLI access**: unaffected — replies are originated, not forwarded. You can
  always log in and look.

## 3. Persistence

Thresholds (and the enabled flag) persist in **`/batt_cfg`**. The suspended
flag itself is **never persisted** — it is recomputed from the live voltage on
every boot, so a reboot mid-suspend can never leave the repeater off after the
battery recovers.

## 4. CLI reference

| Command | Effect |
|---|---|
| `battery` | Status: current mV reading, gate state, thresholds, drop counter |
| `battery off` | Disable the gate (no sampling, never suspends) |
| `battery <suspend-mV> [resume-mV]` | Set thresholds and enable the gate |

Thresholds are millivolts only (1..10000); the resume value must be strictly
greater than the suspend value.

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
