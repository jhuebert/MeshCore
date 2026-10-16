# Mesh Clock Sync — User Guide

Give a repeater a plausible UTC from the adverts it already hears, so features
that need a clock — most importantly fleet `!at` scheduling — work on a node with
no GPS, no NTP and no battery-backed RTC.

**Off by default.** Enabling it changes nothing else; disabling it changes
nothing else either.

---

## Why

A repeater with no clock source starts its RTC at a fallback date. Anything that
compares times is then wrong, and the fleet feature refuses `!at` scripts because
it cannot trust the node's clock (`FLEET_AT_CLOCK_FLOOR`). If you already have
GPS, NTP, or set the clock with the `time` command, you do not need this.

## Behaviour

Every advert carries a signed Unix-epoch timestamp from its sender. An advert
is eligible only when its timestamp is **at or after this firmware's build
epoch** (`__DATE__`/`__TIME__`, interpreted as UTC). There is no grace period:
an older timestamp is rejected and never enters the batch. The comparison is
wrap-safe. The compiler supplies local build time, so sites whose build host uses
a non-UTC timezone may conservatively reject some otherwise-current adverts.

When mesh clock sync is **on**, each distinct source contributes **one vote** per batch.
When the batch fills, the repeater takes the **median** of the sources' predicted
"now" and counts how many sources agree with it:

- **too few agree** — nothing happens; the batch is dropped and a new one starts.
- **the median is close to the node's own clock** (within 60 s) — nothing happens;
  the node is already in sync.
- **enough sources agree and the clock differs** — the RTC is stepped **once** to
  the median, and a new batch starts.

The median is deliberately robust: a minority of sources with a wrong clock
cannot drag the result. Agreement (an inlier count) answers "is there a cluster?"
directly; a standard deviation would be inflated by any single extreme value.

A sample's age does not matter. Because the node's clock advances at roughly real
rate even while wrong, every sample predicts the same "now", so a batch may be
collected over hours and its samples still cluster within seconds.

## When not to use it

- You have a real clock source (`time`, GPS, NTP). Mesh clock sync does not
  detect or defer to one; leave it off.
- **Public adverts are not a root of trust.** N distinct fingerprints are N keys,
  not N independent operators. The quorum filters broken clocks, nothing more. On
  a mesh where anyone can inject keys, this is not a security boundary; treat the
  result as "plausible", not "authoritative".
- A dense, long-lived mesh may hold stale signed adverts dated on or after this
  firmware's build epoch that happen to agree and look like consensus. The strict
  build-time floor rejects pre-build timestamps, but advert signatures prove
  authorship, not freshness; do not enable this where a wrong clock has serious
  consequences.

## CLI reference

| Command | Effect |
|---|---|
| `clock mesh` | Status: `off`, or `on samples=<n>/<cap> last=<result>` |
| `clock mesh on` | Enable |
| `clock mesh off` | Disable |

The status `last=` field is one of:

| Value | Meaning |
|---|---|
| `idle` | no batch has decided yet |
| `set(+N s)` / `set(-N s)` | clock stepped by the median |
| `in-sync(N s)` | median was close to the node clock; nothing changed |
| `no-consensus` | not enough sources agreed; batch dropped |

`last=` is status only — it is **not** persisted, so a reboot reports `idle`.

### Examples

```
> clock mesh
off

> clock mesh on
OK - mesh clock sync on

> clock mesh
on samples=12/33 last=idle

# later, once a batch of 33 distinct sources has agreed
> clock mesh
on samples=0/33 last=set(+31536000s)
```

## Persistence

Only the enabled flag persists, in **`/meshclock`** (4 bytes, CRC-checked). The
batch, the last result and its delta are RAM-only: a reboot always starts a fresh,
empty batch. A `clock mesh` command is written to flash about **3 seconds** later,
staged the same way as the other configs (scratch file, read back and checked,
previous config kept as a backup, then swapped in).

## Defaults

| Setting | Default | Meaning |
|---|---|---|
| `MESHCLOCK_SLOTS` | 33 | distinct sources held per batch (= the quorum size) |
| `MESHCLOCK_MIN_AGREE` | 22 | agreeing sources required to step the clock |
| `MESHCLOCK_TOLERANCE_SECS` | 60 | how close a source must be to the median to agree |
| `MESHCLOCK_MIN_DIFF_SECS` | 60 | below this difference the clock is left alone |

Sparse meshes may take a long time to hear 33 distinct sources; the feature then
simply never acts, which is the safe failure mode. Sites can override
`MESHCLOCK_SLOTS` and `MESHCLOCK_MIN_AGREE` at build time to shrink the batch and
quorum. The estimator uses the batch's middle value, so an odd capacity is
recommended.

## Limits

- The design does not detect an external clock step mid-batch. If something walks
  the clock while a batch is filling and enough stale samples are already held,
  the batch can commit a clock that is off by the step. Not a concern if mesh
  clock sync is the node's only clock source.
- The build epoch comes from the compiler's local `__DATE__`/`__TIME__`,
  interpreted as UTC. A non-UTC build host can make the strict floor conservative
  by its timezone offset.
- A 4-byte source fingerprint can collide, dropping the second key's vote for one
  batch. Rare; accepted to keep RAM tiny.
- Modular (32-bit) arithmetic keeps timestamps correct across 2038/2106; the only
  limit is the same ±68-year ambiguity the RTC already has.
