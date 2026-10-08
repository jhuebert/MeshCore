# Repeater Fleet Management — User Guide

Fleet management for the `simple_repeater` firmware. One PSK-backed channel,
set once per repeater, turns into a remote administration bus: a group text
message on that channel is executed as a small CLI script — deferred, and at
most once per job — on every repeater whose **tags** the script targets. One
broadcast message therefore configures a whole fleet (or any tagged subset of
it), with no serial cable and no per-node logins.

The feature is a top-level `fleet` command, independent of the packet filter:
it works with `filter off`, it works while the battery gate has suspended
forwarding (see [Battery supersession](#battery-supersession)), and filter
rule changes can never break fleet script delivery.

---

## What it does

Every repeater that has `fleet on` and a channel set is a fleet member. A
message like

```text
!id 2026-06-12-preset2-cfg
set txdelay 2
set direct.txdelay 2
set rxdelay 3
```

is relayed like any group text (flood carries it to every repeater in range of
the sender) and, on each member it targets, its lines run through the same
command dispatcher as the serial console — `filter …`, `set …`, `battery …`,
`reboot`, everything an authenticated admin can type. Each executed line and
its reply are logged to the serial console (`cli[<key>] <line>`), so an
operator at the console sees exactly what ran.

**Possession of the channel PSK is full admin access.** Anyone who can encrypt
to the channel can run any CLI command on every fleet member — equivalent to
serial-console access. The trust boundary is the key, nothing else: the keyed
channel's MAC must verify before the repeater even looks at the message, and
that is the whole defense. See [Security model](#security-model).

## Quick start

On each fleet repeater (over serial, or remote admin — see
[FILTER.md](./FILTER.md#managing-the-repeater-remotely)):

```text
fleet chan set 00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff
fleet tag add xiao
fleet tag add siteA
fleet on
```

Then, from any companion device that holds the PSK-backed channel, send a
group text on that channel:

```text
!id first-job
set path.hash.mode 1
```

Check it landed:

```text
fleet seen first-job             # → OK - key first-job seen
```

That is the entire mechanism. Everything below is working out what to put in
the jobs and when.

## Setting up

- `fleet chan set <psk>` takes exactly **16 or 32 bytes of hex**. The reply
  reports the derived on-air channel hash (`OK - fleet channel set h=XX (PSK
  holder = admin)`) — never the PSK itself. The same PSK must be configured on
  every companion you drive the fleet from.
- `fleet chan clear` disables receipt and cancels every queued, sleeping and
  armed script (seen keys and counters survive). See [Kill switches](#kill-switches).
- Tags (below) name this repeater for targeting. A repeater with **no tags**
  still runs broadcast scripts.
- `fleet on` arms the hooks; `fleet off` idles them, cancels queued, sleeping
  and armed scripts, and preserves all config.
- Config persists in `/fleet_cfg` across reboots. Pending acknowledgements,
  armed `!at` scripts and the seen-key table are RAM-only — a reboot forgets
  them.

### Tags: targeting by exact membership

Each repeater carries its own tag set (up to 8 tags, 1–16 characters of
`[A-Za-z0-9._-]` each). A script names the tags it targets with a `!tags`
directive:

```text
!id 2026-06-12-xiao-job
!tags xiao,siteA
set name RIDGE-1
```

Match rule — **exact set membership**: the script runs iff its tag list names
one of the repeater's tags. No contains, no prefix, no wildcards, no
negation: a tag either is set on the repeater or it is not. A literal `*` is
just an invalid tag character.

- **Absent `!tags` = broadcast**: the script runs on every fleet member.
- Tag lists are exact — no whitespace around tags (`xiao, siteB` rejects).
- `fleet tag add <tag>` / `del` / `list` / `clear` manage the set; duplicates
  no-op.

Hierarchies are done by shared tags, not syntax: give every node a common tag
(`all`) plus specific ones (`xiao`, `siteA`, `roof`). Send `!tags all` for
everyone or `!tags xiao` for that cohort. More tags (location, firmware
channel, role) form further overlapping groups. Canary rollouts work the same
way: `!tags canary` first (only canary-tagged repeaters have the tag), widen
the tag after verification.

## Script format

The script is the **text of an ordinary group text message** — companions that
auto-prepend `name: ` work unchanged, since the script is everything after the
first `": "`. Only **plain** group text (txt_type 0) can be a script; any other
text type on the channel is ignored.

```text
!id <key>              # line 1, always
[!tags t1,t2,...]      # the directive block — !tags, !ack and/or !at,
[!ack [err]]           #   each at most once, in either order
[!at <unix-ts>]        #   (lines 2..N; everything before the first command)
<CLI command lines>    # may include "!delay <ms>" anywhere, any number of times
```

- **Line 1 must be `!id <key>`** — the marker doubles as the job identity, so
  no message can ever run un-deduplicated by accident. `<key>` is 1–32
  characters of letters, digits, `.`, `_`, `-` (e.g. `2026-06-12-preset2-cfg`).
  The key is a human-readable job name: it appears in the serial log, in
  `fleet seen <key>`, and in `fleet forget <key>`.
- Directive block rules: `!tags`, `!ack` and `!at` are the known block
  directives — each **at most once**, in either order. Any other `!` line in
  the block refuses the **whole** script (fail-safe, not fail-open).
- After the block, `!`-lines are only valid as `!delay <ms>`; any other
  unknown `!` line still refuses the whole script.
- The block ends at the first command line, so a `!delay` cannot open a script:
  a script whose first line after the directives is `!delay` is refused. Put a
  command first.
- Blank lines and lines starting with `#` are skipped (the same comment
  convention FILTER.md uses).
- A line longer than the serial CLI's own command buffer is skipped and
  counted (`badline` in `fleet`); the rest of the script still runs.
- **Size reality:** one LoRa packet carries ~175 bytes of script — about 3–4
  short commands including the `!id` line. Bigger jobs are several messages
  with **distinct `!id` keys**, and their parts must be order-independent
  (flood arrival order is not guaranteed).

## Idempotency: at most once per key, per boot

Each repeater remembers the job keys it has executed **in RAM only** — the
memory starts empty on every boot, like the advert cache. Within one boot a
key executes at most once; a duplicate flood delivery or a deliberate re-send
no-ops (but the message is still relayed). The key is marked the moment the
script is queued, so duplicate deliveries of one broadcast cost nothing.

What this means in practice:

- **Re-send freely while the fleet is up.** That is how you catch nodes that
  were offline or out of range; repeats are free.
- `fleet seen <key>` answers "did this job run here?" — over a normal admin
  session, one per repeater.
- `fleet forget <key>` re-arms one key for a deliberate re-run; `fleet forget
  all` re-arms everything. A job that is still **queued, sleeping or armed** is
  never doubled: a re-send is refused as a duplicate until it has run or been
  cancelled, even after a `forget`.
- `fleet off` and `fleet chan clear` cancel queued, sleeping and armed jobs.
  Their keys stay consumed: send the job again under a new key, or `fleet
  forget <key>` first.
- **After a reboot, keys are forgotten and a re-sent job runs again.** Scripts
  are therefore *re-runnable by convention*: commands that **set** state
  (`set …`, `filter add …`, `filter ratelimit …`, `battery …`) are safe to
  repeat forever; index-based mutations (`filter del 3` — indices shift!) and
  one-shot effects (`reboot`, `start ota`) belong in their own jobs, sent
  deliberately and only once.
- A captured message replayed after a reboot re-runs. That residual exposure
  is bounded by the re-runnable-scripts convention above; cross-reboot replay
  immunity is not a property of this feature. See [Security model](#security-model).

## Acknowledgements (`!ack`)

A script that carries `!ack` (or `!ack err`) in its directive block makes every
repeater that runs it reply on the **same fleet channel** the script arrived on,
as a plain group text with the **repeater's own name** as the sender —
companions and other fleet members see an ordinary `<repeater>: <text>`
message.

- `!ack` — reply always. The query case: `filter stats`, `battery`, version
  audits. Whoever replies is alive, so this doubles as a liveness roster.
- `!ack err` — reply **only on error**; silence means success. The
  settings-job case: the cutover workflow stays quiet, failures speak up.
  This is the airtime-polite choice for large fleets.
- No `!ack` — no reply.

Reply content, always prefixed with the job key so replies are correlatable
when several jobs are in flight:

- Single-command script → the command's **actual reply** (e.g.
  `2026-06-12-stats rx:12 fwd:34 arp:1 arp_fwd:0`), truncated to fit one packet.
- Multi-command script → `<key> ran 5 ok`, or
  `<key> ran 5; err: <first failing line's reply>` — the first error is the
  only useful detail in one packet. Replies are never split across packets.

Protocol guarantees, by construction:

1. Replies are `txt_type = 0x00` plain group texts — visible in the
   companion's channel view.
2. **Replies never parse as scripts**: reply text starts with the job key
   (charset `[A-Za-z0-9._-]`, never `!`), so it can never match the `!id `
   marker. Keep future reply formats key-prefixed for the same reason.
3. Flood is the only reply route — group texts carry no pubkey, so a repeater
   cannot direct-reply to "alice" (sender-controlled text, not an identity).
   Every reply floods and is relayed by the fleet.

**Storm control:** the reply is scheduled at a uniform-random instant in a
jitter window (default 60 s, node-tunable via `fleet reply <secs>`, 1–600 s)
measured from when the script *finishes running*. With N nodes in window W and
~1 s packets, W = 60 s keeps a 10-node fleet essentially collision-free;
**widen W for denser fleets**. Pending replies (2 slots) are RAM-only: a
reboot wipes them, so a re-sent `!ack` job after a reboot re-runs *and*
re-replies. A full reply store drops and counts the reply (`refused`); no
reply cooldown in v1 — per-key dedupe already bounds each script to one reply.

## Inline pauses (`!delay <ms>`)

```text
!id 2026-06-12-cutover
set radio 869.650,62.5,9,5
!delay 5000
reset
```

`!delay <ms>` (1–300000 ms, i.e. up to 5 minutes) is valid **anywhere in the
command body**, any number of times, and pauses that script's execution for
`<ms>` milliseconds before the next line runs:

- A bad, missing or out-of-range value refuses the whole script (fail-safe).
- The runner is resumable: the script's slot keeps its position across main-loop
  passes and execution resumes once the deadline passes. With no delays the
  behavior is exactly the classic one: one whole script per loop pass.
- **A sleeping script occupies its queue slot.** Two concurrently-sleeping
  scripts fill the 2-slot queue and new scripts are refused (counted, key NOT
  marked) until one resumes. Scripts needing long delays should be their own
  jobs anyway.
- `!ack` interaction: the reply summary is computed when the script *finishes*
  (after its last `!delay`), so a delayed script replies once, late, complete.

## Scheduled execution (`!at <unix-ts>`)

```text
!id 2026-06-12-cutover
!at 1781268000
reset
```

A `!at` block directive schedules the whole script to execute when the node's
RTC reaches the given unix epoch timestamp instead of immediately. This turns
the fleet-wide `reset` cutover from a two-phase, wait-for-saturation dance into
a single job every node runs **at the same instant** — no mesh-split risk — and
enables off-hours maintenance windows.

- **`<unix-ts>` is unix epoch seconds — seconds since 1970-01-01 UTC**, as all
  unix timestamps are. Unix time carries no timezone; repeater logs display
  UTC (the `U` in log timestamps). Compute it with `date -u +%s` — "is this
  UTC?" is the first question every user asks.
- **Stale is refused**: a timestamp more than 300 s in the past is refused
  (counted, key not marked) — a typo'd stale date must never run immediately,
  since the damage it can do (a surprise `reset`) is exactly what scheduling is
  meant to control. Within the 300 s grace (clock skew, mesh transit delay) it
  executes immediately.
- **Clock unset → refuse.** While the node's RTC is before 2025-01-01, `!at`
  scripts are refused and counted: executing at an unknown time is worse than
  not executing. The sync recipe below fixes this first.
- Armed jobs live in a dedicated RAM-only store (2 entries; a full store
  refuses new ones, counted, key not marked). **A reboot wipes them — re-send
  the job.** Ordinary scripts keep flowing while a cutover is armed.
- When the RTC passes the timestamp the script runs as normal — same
  deferral, same `!ack` behavior (the reply fires after execution, so a
  scheduled job's replies are themselves jittered and confirm *when* each node
  actually ran it).

### The sync → verify → schedule recipe

Repeater RTCs drift (no GPS = minutes to hours off), and scheduled cutovers
executed by nodes with disagreeing clocks are *worse* than the two-phase
pattern they replace — nodes would bounce at different moments, exactly the
mesh split scheduling exists to avoid. Sync first, using existing commands
only:

1. **Sync**: a fleet script running **`time <unix-ts>`** — re-send until every
   node has it (idempotent; `time` refuses to go backwards).

   **`clock sync` does not work inside fleet scripts** — it syncs from the
   message's sender timestamp, and scripts run with `sender_timestamp = 0` —
   so scripts must use `time` with an explicit epoch.
2. **Verify**: a `!ack` query job running **`clock`** — every node replies with
   its current time (minute resolution, UTC), so you can confirm agreement
   before trusting a scheduled job. Minute resolution cannot see a node that is
   a minute or so behind: an `!at` job fires when the node's own RTC reaches the
   timestamp, so a node running 70 s slow fires 70 s late. Sync right before you
   schedule, and keep the cutover timestamp well clear of the verify step.
3. **Schedule**: only then arm the `!at` job.

## Command reference

All commands begin with `fleet`.

| Command | Effect |
|---|---|
| `fleet` | Status: `on, chan h=XX, tags 2/8; scripts ran:3 dup:1 noid:0 refused:0; pending 0/2; sched 0/2; seen 2/32; ackq 0/2; badline 0` |
| `fleet on` / `fleet off` | Arm / idle the hooks (config preserved either way) |
| `fleet chan set <psk-hex>` | Set/replace the one fleet channel (16 or 32 bytes hex) — `OK - fleet channel set h=XX (PSK holder = admin)` |
| `fleet chan clear` | `OK - fleet channel cleared` — receipt stops, queued and armed scripts are cancelled, seen keys and counters kept |
| `fleet tag add <tag>` | `OK - tag xiao added (2/8)`; duplicates no-op `OK` |
| `fleet tag del <tag>` | `OK - tag xiao deleted` / `Err - tag xiao not set` |
| `fleet tag list` | `tags: xiao siteA` or `tags: (none) - broadcast scripts only` |
| `fleet tag clear` | `OK - tags cleared` |
| `fleet reply [<secs>]` | Query or set the `!ack` jitter window (1–600 s; default 60) |
| `fleet seen` | How many job keys are remembered: `seen 2/32` |
| `fleet seen <key>` | `OK - key <key> seen` / `Err - key <key> not seen` — did this job run here? |
| `fleet forget <key>` | `OK - key forgotten` — re-arm one job for a deliberate re-run |
| `fleet forget all` | `OK - seen table cleared` — re-arm everything |

Counter meanings:

| Counter | Meaning |
|---|---|
| `ran` | Scripts executed since boot |
| `dup` | Messages refused because their key was already seen (the normal "re-send" case) |
| `noid` | Messages ignored because line 1 was not `!id …` (ordinary chat on the channel) |
| `refused` | Scripts refused: queue full, a bad `!id` key, a malformed or unknown `!` directive, a stale/unset-clock/full-store `!at` admission, or a dropped acknowledgement |
| `badline` | Command lines skipped at run time for being over-long |
| `sched` | Armed `!at` scripts (`0/2`) |
| `ackq` | Pending acknowledgements (`0/2`) |

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
# every node (check with `fleet seen` over a normal admin session)
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

**When `!at` replaces this:** once the fleet's clocks are synced (the
[sync → verify → schedule recipe](#the-sync--verify--schedule-recipe)), a
cutover can be a single armed job — settings applied and `reboot` in one
script, every node executing at the same coordinated instant:

```text
alice: !id 2026-06-12-preset2-at
!at 1781268000
set radio 869.650,62.5,9,5
reboot
```

Rollback after an armed cutover is the same shape on the old settings. Re-send
the armed job after any node rebooted in between (armed jobs are RAM-only).

## Recipes

Each recipe is one or more jobs on the fleet channel. `#` lines are comments —
don't send them.

### Radio preset switch

The [two-phase pattern](#two-phase-fleet-changes-radio-presets-and-other-cutovers)
above, or its [armed variant](#scheduled-execution-at-unix-ts) once clocks are
synced. Keep your preset table (freq/bw/sf/cr values) in your own notes or a
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

If you are not sure of the index, don't guess: run `filter list` with `!ack`
and read the replies, or over a normal admin session per node, then send del
jobs addressed to the cohorts whose layout you know. A mis-addressed `del`
deletes the wrong rule — prefer `filter disable <idx>` first, watch the mesh
for a day, then delete.

### Node renaming and location refresh

```text
alice: !id 2026-06-14-names
!tags xiao
set name RIDGE-NORTH
set lat 47.6205
set lon -122.3493
```

All set-state commands: safe to re-send, safe to re-run after a reboot.

### Channel rotation (PSK response)

Suspect the fleet key leaked? Set a fresh channel and verify the new path
works, then retire the old one — but note the asymmetry with the filter's
channel store: **`fleet chan set` is a one-channel config**, so the rotation
is a single step, not an add-then-retire. Roll the new key out by direct admin
per node (or a script sent over the *old* channel that ends with
`fleet chan set <new-psk>` — it must be the last line of its job, and the
operator should keep the old channel configured in the companion until every
node has rotated). `fleet seen` over the old channel tells you which nodes
already rotated: they stop seeing jobs there.

While you work, the old key still opens the channel — speed matters. In the
meantime `fleet off` (over a normal admin session, per node) closes the door.

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

Tag a few repeaters `canary` and send `!tags canary` jobs first; widen the tag
(`fleet tag add canary` — which is itself a broadcast script) after
verification. Cohorts by board/site/role work the same way; `!tags all` with
an `all` tag on every node is the convention for fleet-wide jobs.

### Remote fleet re-configuration

A script line that runs `fleet …` is fine (the drain is outside the receive
path); the tag model makes remote reconfiguration a normal recipe:

```text
alice: !id 2026-06-17-rooftags
!tags all
fleet tag add roof
```

The obvious footgun — a script that runs `fleet chan clear` on the whole
fleet — is the same class as deleting the filter rule that delivered it:
documented here, not special-cased in code. Don't.

## Status, logs and troubleshooting

`fleet` is the feature's dashboard; `fleet seen <key>` over a normal admin
session answers "did node X get job Y?". The serial console carries the full
transcript:

```text
cli[2026-06-12-preset2-cfg] set radio 869.650,62.5,9,5
  -> OK - reboot to apply
```

| Symptom | Check | Usual cause |
|---|---|---|
| Job never ran anywhere | `fleet seen <key>` per node; `noid` counter | Message sent without the `!id` first line, or on the wrong channel |
| Some nodes ran it, some didn't | `refused` counter on the stragglers | Their pending queue was full (a third job arrived before the first drained); re-send the job |
| Script targets nobody | `fleet tag list` on the nodes | Tag typo (exact matching: `xiao ` ≠ `xiao`); or all nodes lack the tag and `!tags` was present |
| No `!ack` reply arrived | `ackq` on the node; window too long? | Reply still jittering (up to `reply window` s); store was full (`refused`); or the channel changed between receipt and send |
| `dup` climbing, nothing changed | — | Normal: re-sends and duplicate flood deliveries |
| Job ran twice | seen-ring wrap | 33+ distinct jobs this boot evicted the key; re-send deliberately via `forget` semantics |
| Lines missing from a run | `badline` counter | Lines over the CLI's length limit were skipped — split the job |
| Whole script refused, `refused` up | — | Unknown `!` directive, bad key characters, or a malformed `!tags`/`!ack`/`!at`/`!delay`; check the exact text |
| `!at` job never fired | `sched` count; `clock` reply per node | Clock not synced (below the 2025 floor → refused), or the RTC is past it and it fired already; or a reboot wiped the armed job — re-send |
| `!at` refused, `refused` up | — | Timestamp > 300 s in the past (stale), RTC unset, or the scheduled store was full |

### Kill switches

`fleet off` and `fleet chan clear` stop the fleet at once: receipt stops, and
every queued, sleeping and armed script is cancelled (its key stays consumed,
see [Idempotency](#idempotency-at-most-once-per-key-per-boot)). A script that
switches the fleet off runs its remaining lines up to its next `!delay`, and
nothing after that. `fleet forget <key>` re-arms one job. `filter off` does not stop fleet; the
[battery gate does not stop fleet either](#battery-supersession).

## Battery supersession

**Fleet management works when the battery is low** — a node in trouble is
exactly the node you need to reach. All three of the battery gate's reach are
exempt for the fleet channel:

- **Execution**: fleet scripts enqueue and run while forwarding is suspended.
- **Relaying**: fleet-channel traffic relays while suspended, and is never
  counted as a battery drop. (The exemption is exactly the packet the repeater
  has just MAC-verified under the fleet key. The one-byte channel hash only
  narrows the search, so other channels that share it stay gated.)
- **Acknowledgements**: replies are sent regardless of gate state — the
  operator chose `!ack`, and it is the operator's call whether to spend
  battery on a response. A battery report from a *low* node is the most
  valuable one.

Normal user traffic stays gated exactly as BATTERY.md describes. Decryption is
unaffected by the gate in the first place; a script executed while suspended
runs with full serial privilege, and what its commands *send* is governed by
the commands themselves. The two-phase cutover recipe gets **simpler**, not
riskier: a suspended node still receives, runs, and retransmits fleet traffic,
so it can't be orphaned from a fleet cutover by its own battery state.

## Security model

- **PSK possession = admin.** The channel key is the credential; its MAC gate
  is the only thing between the world and your scripts. Holders of the key are
  equals — there is no per-sender distinction, and tags are not authentication.
- **Replay:** a captured message re-sent while the repeater is up no-ops
  (seen table) and is still relayed. After a reboot the table is empty and the
  replay re-runs — bounded by the re-runnable-scripts convention. If
  cross-reboot replay protection matters to you, keep one-shot jobs out of the
  channel and do them over direct admin.
- **Leak response:** treat every fleet member as compromised, rotate the PSK
  ([channel rotation](#channel-rotation-psk-response)), and `fleet off` (per
  node, over normal admin) in the meantime.
- **Bounded resources:** payload-sized scripts, a 2-slot run queue, a
  2-entry scheduled store, a 2-entry reply store, a 32-entry seen ring, one
  script per packet. No amplification: a fleet member relays the message once
  and runs its own copy locally; `!ack` replies are jittered against storms.

## Limits and good-to-knows

| Limit | Value |
|---|---|
| Script per message | ~175 bytes of script (`!id` line included) |
| Pending scripts | 2 — a third before the queue drains is refused, but **not** marked seen; a re-send reaches it |
| Armed `!at` scripts | 2 (RAM-only; a reboot wipes them) |
| Pending acknowledgements | 2 (RAM-only; a full store drops and counts) |
| Seen job keys | last 32 per boot (RAM-only; a 33rd evicts the oldest and re-arms it) |
| `!id` key | 1–32 chars of `[A-Za-z0-9._-]` |
| Tags | up to 8 on a repeater; up to 8 per `!tags` list; 1–16 chars each |
| `!delay` | 1–300000 ms per pause; a sleeping script holds its queue slot |
| Command lines | up to the serial CLI's buffer (~160 bytes); longer lines are skipped |

- Execution is **deferred**: a queued script runs in the main loop, outside the
  packet handling that queued it, so scripts may safely contain `filter …` and
  `fleet …` commands that mutate config.
- All script state is RAM-only. Job keys, queue, armed jobs and counters reset
  on reboot — the channel, tags and reply window persist in `/fleet_cfg`; the
  script machinery does not.
- `filter stats reset` does not touch the fleet counters; they reset only on
  reboot.
- Companions that prepend the sender name (`name: `) work unchanged: the
  script is everything after the first `": "`.