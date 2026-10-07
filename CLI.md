# Repeater Remote CLI Scripts — User Guide

A fleet management channel for the `simple_repeater` firmware. A filter rule
with `action=cli` turns a private keyed channel into a remote administration
bus: a group text message on that channel is relayed like any other traffic
**and** its text is executed as a small CLI script — deferred, and at most once
per job, on every repeater carrying the rule. One broadcast message therefore
configures a whole fleet, with no serial cable and no per-node logins.

---

## What it does

Every repeater that stores a `cli` rule becomes a fleet member. A message like

```text
alice: !id 2026-06-12-preset2-cfg
set radio 869.650,62.5,9,5
```

is **forwarded** (flood carries it to every repeater in range of the sender)
and, on each member, its lines run through the same command dispatcher as the
serial console — `filter …`, `set …`, `battery …`, `reboot`, everything an
authenticated admin can type. Each executed line and its reply are logged to
the serial console (`cli[<key>] <line>`), so an operator at the console sees
exactly what ran.

**Possession of the channel PSK is full admin access.** Anyone who can encrypt
to the channel can run any CLI command on every fleet member — equivalent to
serial-console access. The trust boundary is the key, nothing else: the keyed
channel's MAC must verify before the repeater even looks at the message, and
that is the whole defense. See [Security model](#security-model).

## Quick start

On each fleet repeater (over serial, or remote admin — see
[FILTER.md](./FILTER.md#managing-the-repeater-remotely)):

```text
filter chan add ops 00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff
filter add chan=ops action=cli
```

Then, from any companion device that holds the `ops` key, send a group text on
that channel:

```text
!id first-job
set name FLEET-7
```

Check it landed:

```text
filter cli seen first-job        # → OK - key first-job seen
```

That is the entire mechanism. Everything below is working out what to put in
the jobs and when.

## Setting up the channel

- The rule's `chan=` must name at least one **PSK-backed** channel — a channel
  stored with an explicit key. `'#'`-named channels derive their key from the
  public channel name, so they authenticate nobody, and `cli` rules refuse them
  (even one holding an explicit PSK is refused — name the channel without the
  `#`).
- The add reply carries the standing warning: `OK - rule 0 added (cli: PSK
  holder = admin)`.
- The rule is forced to group text only (`type=txt`), and refuses `prob=` and
  `throttle=` — a gated rule would make different repeaters decide differently,
  which is exactly what fleet management must not do.
- A `cli` rule is terminal like `forward` (first match wins), so it normally
  belongs at the top of the list for its channel.
- Every rule predicate still works as a *condition*: `sender=`, `text=`,
  `hops=`, `region=`, … narrow which messages run. They are fleet selection,
  not authentication.

## Script format

The script is the **text of an ordinary group text message** — exactly the
field `text=` matches, so companions that auto-prepend `name: ` work unchanged.

```text
alice: !id <key>
<command line 1>
<command line 2>
```

- **Line 1 must be `!id <key>`** — the marker doubles as the job identity, so
  no message can ever run un-deduplicated by accident. `<key>` is 1–32
  characters of letters, digits, `.`, `_`, `-` (e.g. `2026-06-12-preset2-cfg`).
  The key is a human-readable job name: it appears in the serial log, in
  `filter cli seen <key>`, and in `filter cli forget <key>`.
- Remaining lines are CLI commands, one per line. Blank lines and lines
  starting with `#` are skipped (the same comment convention FILTER.md uses).
- Any other line starting with `!` is reserved for future directives; an
  unknown `!` directive refuses the **whole** script (fail-safe, not
  fail-open).
- A line longer than the serial CLI's own command buffer is skipped and counted
  (`badline` in `filter cli`); the rest of the script still runs.
- **Size reality:** one LoRa packet carries ~175 bytes of script — about 3–4
  short commands including the `!id` line. Bigger jobs are several messages
  with **distinct `!id` keys**, and their parts must be order-independent
  (flood arrival order is not guaranteed).

## Idempotency: at most once per key, per boot

Each repeater remembers the job keys it has executed **in RAM only** — the
memory starts empty on every boot, like the advert cache. Within one boot a
key executes at most once; a duplicate flood delivery or a deliberate re-send
no-ops (but the message is still forwarded). The key is marked the moment the
script is queued, so duplicate deliveries of one broadcast cost nothing.

What this means in practice:

- **Re-send freely while the fleet is up.** That is how you catch nodes that
  were offline or out of range; repeats are free.
- `filter cli seen <key>` answers "did this job run here?" — over a normal
  admin session, one per repeater.
- `filter cli forget <key>` re-arms one key for a deliberate re-run;
  `filter cli forget all` re-arms everything.
- **After a reboot, keys are forgotten and a re-sent job runs again.** Scripts
  are therefore *re-runnable by convention*: commands that **set** state
  (`set …`, `filter add …`, `filter ratelimit …`, `battery …`) are safe to
  repeat forever; index-based mutations (`filter del 3` — indices shift!) and
  one-shot effects (`reboot`, `start ota`) belong in their own jobs, sent
  deliberately and only once.
- A captured message replayed after a reboot re-runs. That residual exposure
  is bounded by the re-runnable-scripts convention above; cross-reboot replay
  immunity is not a property of this feature. See [Security model](#security-model).

## Command reference

All commands begin with `filter` (see [FILTER.md](./FILTER.md#command-reference)
for the rest of the namespace).

| Command | Effect |
|---|---|
| `filter cli` | Counters and state, e.g. `scripts ran:3 dup:1 noid:0 refused:0 badline:0; pending 0/2; seen 2/32` |
| `filter cli seen` | How many job keys are remembered: `seen 2/32` |
| `filter cli seen <key>` | `OK - key <key> seen` / `Err - key <key> not seen` — did this job run here? |
| `filter cli forget <key>` | `OK - key forgotten` — re-arm one job for a deliberate re-run |
| `filter cli forget all` | `OK - seen table cleared` — re-arm everything |

Counter meanings:

| Counter | Meaning |
|---|---|
| `ran` | Scripts executed since boot |
| `dup` | Messages refused because their key was already seen (the normal "re-send" case) |
| `noid` | Messages ignored because line 1 was not `!id …` (ordinary chat on the channel) |
| `refused` | Scripts refused: queue full, a bad `!id` key, or an unknown `!` directive |
| `badline` | Command lines skipped at run time for being over-long |

## Two-phase fleet changes (radio presets and other cutovers)

A job that reboots a repeater on first receipt splits the mesh **before**
stragglers got the new settings — repeaters still on the old frequency would
be orphaned, and with them possibly your path back in. So cut over in two
phases:

```text
# phase 1 — saturate: re-send this over a period of time (each send catches
# more of the fleet; it no-ops on repeaters that already ran it, and
# re-running it after a reboot is harmless because it only sets values)
alice: !id 2026-06-12-preset2-cfg
set radio 869.650,62.5,9,5

# phase 2 — commit: one-line job, sent once, only after phase 1 has reached
# every node (check with `filter cli seen` over a normal admin session)
alice: !id 2026-06-12-preset2-go
reboot
```

`set radio` itself replies `OK - reboot to apply`, which is why the pattern
works so cleanly: the settings job is idempotent and repeatable, and the
commit job is a single reboot that only fires once everyone is ready.

Rollback is just a new key — it runs everywhere, including repeaters that
already ran the first job:

```text
alice: !id 2026-06-12-preset2-fix
set radio 869.525,62.5,8,5
```

followed by its own `reboot` job once saturated.

## Recipes

Each recipe is one or more jobs on the fleet channel. `#` lines are comments —
don't send them.

### Radio preset switch

The [two-phase pattern](#two-phase-fleet-changes-radio-presets-and-other-cutovers)
above. Keep your preset table (freq/bw/sf/cr values) in your own notes or a
companion snippet — the repeater stores one active setting, and the job is
just the value plus a commit.

### Advert hygiene

Tame advert storms fleet-wide without touching any node by hand:

```text
alice: !id 2026-06-12-adv24
filter ratelimit advert 24
```

Re-sendable forever; nodes that already ran it no-op.

### Fleet filter update

Push a new policy to every repeater at once. First add the rule (re-sendable),
then remove an old one by index — but only in a **separate job**, because
indices shift after a delete:

```text
alice: !id 2026-06-13-rule-add
filter add chan=#wardriving hops=[2,*] action=forward

alice: !id 2026-06-13-rule-del
filter del 2
```

If you are not sure of the index, don't guess: run `filter list` over a
normal admin session per node (a script's replies are only logged to the
repeater's serial console — scripts do not answer over the mesh — so reads
belong in an admin session), then send del jobs addressed to the fleets whose
layout you know. A mis-addressed `del` deletes the wrong rule — prefer
`filter disable <idx>` first, watch the mesh for a day, then delete.

### Node renaming and location refresh

```text
alice: !id 2026-06-14-names
set name RIDGE-NORTH
set lat 47.6205
set lon -122.3493
```

All set-state commands: safe to re-send, safe to re-run after a reboot.

### Channel rotation (PSK response)

Suspect the fleet key leaked? Add a fresh channel and rule, verify the new
path works, then retire the old one — each phase its own re-sendable job:

```text
alice: !id 2026-06-15-rotate-1
filter chan add ops2 <new-64-hex-psk>
filter add chan=ops2 action=cli
filter move 0 1   # put the new rule where you want it in the list

alice: !id 2026-06-15-rotate-2
filter del <old-cli-rule-idx>
filter chan del ops
```

Do phase 2 only from the new channel, and only after `filter cli seen
2026-06-15-rotate-1` confirmed the new rule runs everywhere. While you work,
the old key still opens the old rule — speed matters.

### Emergency quiet mode

An incident response that stops the fleet repeating without bricking your
reach-back (each node still answers its own admin):

```text
alice: !id 2026-06-16-quiet
filter off
```

Recovery is the same shape (`filter on`) — or per-node over normal admin.
For a harder stop, `set repeat off` in its own job; recovering needs an
explicit `set repeat on` job, which is itself re-sendable.

### Canary rollout

Try a change on a subset before the fleet:

- Provision a canary channel (`canary`) with a `cli` rule on a few repeaters
  only, and run jobs there first; or
- Use [target groups](#targeting-groups-of-repeaters) with a canary tag and
  send `-canary-` jobs first, then the `-all-` job.

`prob=` is deliberately unavailable on cli rules — dosing a fleet change
per-packet would split the fleet arbitrarily. Distinct keys and channels give
you deterministic groups instead.

## Targeting groups of repeaters

`text=` selectors on the `!id` key address nested groups while keeping one
keyed channel and one script mechanism — fleet selection by tag, not
authentication. Provision non-XIAO repeaters with a global selector and XIAO
repeaters with a selector that accepts both tags:

```text
# other repeaters:
filter add chan=ops action=cli text="^!id .*-all-.*"

# XIAO repeaters:
filter add chan=ops action=cli text="^!id .*-all-.*|^!id .*-xiao-.*"
```

A job `!id 26-all-radio` then reaches every repeater, `!id 26-xiao-radio` only
the XIAOs. Put the group marker in the key and nowhere else — `text=` examines
the whole message, so repeating tags in command lines is redundant.

More tags (location, firmware channel, role) form further overlapping groups:

```text
filter add chan=ops action=cli text="^!id .*-all-.*|^!id .*-west-.*"
```

subject to the usual pattern limits (63 characters, 8 `|`-alternatives — see
[Writing sender/text patterns](./FILTER.md#writing-sendertext-patterns)).
Job keys like `!id 26-west-radio` or `!id 26-all-adv24` keep the convention:
date, group tag, purpose.

## Status, logs and troubleshooting

`filter cli` is the feature's dashboard; `filter cli seen <key>` over a normal
admin session answers "did node X get job Y?". The serial console carries the
full transcript:

```text
cli[2026-06-12-preset2-cfg] set radio 869.650,62.5,9,5
  -> OK - reboot to apply
```

| Symptom | Check | Usual cause |
|---|---|---|
| Job never ran anywhere | `filter cli seen <key>` per node; `noid` counter | Message sent without the `!id` first line, or on the wrong channel |
| Some nodes ran it, some didn't | `refused` counter on the stragglers | Their pending queue was full (a third job arrived before the first drained); re-send the job |
| `dup` climbing, nothing changed | — | Normal: re-sends and duplicate flood deliveries |
| Job ran twice | seen-ring wrap | 33+ distinct jobs this boot evicted the key; re-send deliberately via `forget` semantics |
| Lines missing from a run | `badline` counter | Lines over the CLI's length limit were skipped — split the job |
| Whole script refused, `refused` up | — | Unknown `!` directive or bad key characters; check the exact text |

Kill switches, all existing behavior: `filter off` (nothing is scanned or
queued), disabling or deleting the rule, battery-gate suspension, or deleting
the channel (the rule then goes inert — a `cli` rule never falls back to
running on a `'#'`-derived channel).

## Security model

- **PSK possession = admin.** The channel key is the credential; its MAC gate
  is the only thing between the world and your scripts. Holders of the key are
  equals — there is no per-sender distinction.
- **`sender=` and `text=` are not authentication.** The sender name is
  sender-chosen text; selectors narrow which messages run, they never prove
  who wrote them.
- **Replay:** a captured message re-sent while the repeater is up no-ops
  (seen table) and is still forwarded. After a reboot the table is empty and
  the replay re-runs — bounded by the re-runnable-scripts convention. If
  cross-reboot replay protection matters to you, keep one-shot jobs out of the
  channel and do them over direct admin.
- **Leak response:** treat every fleet member as compromised, rotate the PSK
  ([channel rotation](#channel-rotation-psk-response)), and `filter off` (or
  delete the rule) in the meantime.
- **Bounded resources:** payload-sized scripts, a 2-slot queue, a 32-entry
  seen ring, one script per packet. No amplification: a fleet member forwards
  the message once and runs its own copy locally.

## Limits and good-to-knows

| Limit | Value |
|---|---|
| Script per message | ~175 bytes of script (`!id` line included) |
| Pending scripts | 2 — a third before the queue drains is refused, but **not** marked seen; a re-send reaches it |
| Seen job keys | last 32 per boot (RAM-only; a 33rd evicts the oldest and re-arms it) |
| `!id` key | 1–32 chars of `[A-Za-z0-9._-]` |
| Command lines | up to the serial CLI's buffer (~160 bytes); longer lines are skipped |

- Execution is **deferred**: a queued script runs in the main loop, outside the
  packet handling that queued it, so scripts may safely contain `filter …`
  commands that mutate the rule list.
- All script state is RAM-only. Job keys, queue and counters reset on reboot —
  rules, channels and settings survive; the script machinery does not.
- `filter stats reset` does not touch the script counters; they reset only on
  reboot.
- The script machinery shares nothing with the advert rate limiter or the
  battery gate; disabling either does not affect scripts (and vice versa —
  except that battery suspension stops the content scan, so nothing is queued
  while suspended).
- Companions that prepend the sender name (`name: `) work unchanged: the
  script is everything after the first `": "`.