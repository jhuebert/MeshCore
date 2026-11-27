# Fleet Management: User Guide

Fleet management turns one PSK-backed channel into a remote admin bus for
`simple_repeater`. A group text on that channel runs as a small CLI script,
deferred and at most once per job, on every repeater whose **tags** it targets.
One broadcast configures the whole fleet or a tagged subset, with no serial
cable and no per-node logins.

- Independent of the packet filter: works with `filter off`, and filter changes cannot break script delivery.
- Works while the battery gate has suspended forwarding; see [Battery supersession](#battery-supersession).

## What it does

Every repeater with `fleet on` and a channel set is a fleet member. A message
like this is relayed like any group text (flood carries it to every repeater in
range of the sender) and, on each member it targets, its lines run through the
same command dispatcher as the serial console:

```text
!id 2026-06-12-preset2-cfg
set txdelay 2
set direct.txdelay 2
set rxdelay 3
```

That includes `filter …`, `set …`, `battery …`, `reboot`, everything an
authenticated admin can type. Each executed line and its reply are logged to the
serial console (`cli[<key>] <line>`), so an operator at the console sees exactly
what ran.

**Note:** Possession of the channel PSK is full admin access. Anyone who can
encrypt to the channel can run any CLI command on every fleet member, equivalent
to serial-console access. The trust boundary is the key, nothing else: the keyed
channel's MAC must verify before the repeater even looks at the message, and
that is the whole defense. See [Security model](#security-model).

## Quick start

On each fleet repeater, over serial or remote admin (see
[FILTER.md](./FILTER.md#managing-the-repeater-remotely)):

```text
fleet chan set 00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff
fleet tag add xiao
fleet tag add siteA
fleet on
```

Then, from any companion device that holds the PSK-backed channel, send a group
text on that channel:

```text
!id first-job
set path.hash.mode 1
```

One job = one message: a job's commands are the lines of a single group text, so
only the first line is checked for `!id`, a bare command sent on its own is
ordinary chat and does nothing, and two separate things need their own `!id`
keys to become independent jobs. Check it landed:

```text
fleet seen first-job             # → OK - key first-job seen
```

That is the entire mechanism. Everything below is working out what to put in
the jobs and when.

## Setting up

- `fleet chan set <psk>` takes exactly **16 or 32 bytes of hex**. The reply
  reports the derived on-air channel hash (`OK - fleet channel set h=XX (PSK
  holder = admin)`), never the PSK itself. The same PSK must be configured on
  every companion you drive the fleet from.
- `fleet chan clear` disables receipt and cancels every queued, sleeping and
  armed script (seen keys and counters survive). See [Kill switches](#kill-switches).
- Tags name this repeater for targeting. A repeater with **no tags** still runs
  broadcast scripts.
- `fleet on` arms the hooks; `fleet off` idles them, cancels queued, sleeping
  and armed scripts, and preserves all config.
- Config persists in `/fleet_cfg` across reboots. Pending acknowledgements,
  armed `!at` scripts and the seen-key table are RAM-only: a reboot forgets
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

Match rule is **exact set membership**: the script runs iff its tag list names
one of the repeater's tags. No contains, no prefix, no wildcards, no negation: a
tag either is set on the repeater or it is not. A literal `*` is just an invalid
tag character.

- **Absent `!tags` = broadcast**: the script runs on every fleet member.
- Tag lists are exact: no whitespace around tags (`xiao, siteB` rejects).
- `fleet tag add <tag>` / `del` / `list` / `clear` manage the set; duplicates
  no-op.

Groups are formed by shared tags rather than syntax; the canary and cohort
pattern is in the [Recipes](#recipes) table.

## Script format

The script is the **text of an ordinary group text message**: companions that
auto-prepend `name: ` work unchanged, since the script is everything after the
first `": "`. Only **plain** group text (txt_type 0) can be a script; any other
text type on the channel is ignored.

```text
!id <key>              # line 1, always
[!tags t1,t2,...]      # the directive block: !tags, !ack and/or !at,
[!ack [err]]           #   each at most once, in either order
[!at <unix-ts>]        #   (lines 2..N; everything before the first command)
<CLI command lines>    # may include "!delay <ms>" anywhere, any number of times
```

- **Line 1 must be `!id <key>`**: the marker doubles as the job identity, so no
  message can ever run un-deduplicated by accident. `<key>` is 1–32 characters
  of letters, digits, `.`, `_`, `-` (e.g. `2026-06-12-preset2-cfg`). The key is
  a human-readable job name: it appears in the serial log, in `fleet seen <key>`
  and in `fleet forget <key>`.
- Directive block rules: `!tags`, `!ack` and `!at` are the known block
  directives, each **at most once**, in either order. Any other `!` line in the
  block refuses the **whole** script (fail-safe, not fail-open).
- After the block, `!`-lines are only valid as `!delay <ms>`; any other unknown
  `!` line still refuses the whole script.
- The block ends at the first command line, so a `!delay` cannot open a script:
  a script whose first line after the directives is `!delay` is refused. Put a
  command first.
- Blank lines and lines starting with `#` are skipped (the same comment
  convention FILTER.md uses).
- A line longer than the serial CLI's own command buffer is skipped and counted
  (`badline` in `fleet`); the rest of the script still runs.
- **Note:** one LoRa packet carries ~175 bytes of script, about 3–4
  short commands including the `!id` line. Bigger jobs are several messages with
  **distinct `!id` keys**, and their parts must be order-independent (flood
  arrival order is not guaranteed).

## Acknowledgements (`!ack`)

A script that carries `!ack` (or `!ack err`) in its directive block makes every
repeater that runs it reply on the **same fleet channel** the script arrived on,
as a plain group text with the **repeater's own name** as the sender: companions
and other fleet members see an ordinary `<repeater>: <text>` message.

- `!ack`: reply always. The query case: `filter stats`, `battery`, version
  audits. Whoever replies is alive, so this doubles as a liveness roster.
- `!ack err`: reply **only on error**; silence means success. The settings-job
  case: the cutover workflow stays quiet, failures speak up. This is the
  airtime-polite choice for large fleets.
- No `!ack`: no reply.

"Error" means the command's reply is a failure: the fork's own `Err - ...` and
the core's `Error`, `Error, ...`, `ERROR: ...`, `ERR: ...`, `error` and
`Unknown command`. A typo'd core command (`set txdelayy 2`) replies
`Unknown command`, and that is an error: the job did not do what it said.

Reply content is always prefixed with the job key, so replies stay correlatable
when several jobs are in flight. On air the body is `<key>`, a newline, then the
response; companions show both lines separately.

| Job | Reply |
|---|---|
| One command | The command's actual reply, cut to fit one packet (e.g. `2026-06-12-stats rx:12 fwd:34 arp:1 arp_fwd:0`) |
| Several commands | `<key> ran 5 ok`, or `<key> ran 5; err: <first failing line's reply>` |

The first error is the only useful detail that fits one packet. Replies are never split across packets.

| Marker | Meaning |
|---|---|
| `...` at the end of a reply | The reply was cut here. |
| `...` on its own line in `filter list` | The filter's page marker: more rules follow. |
| `..` at the end of a line | That rule was not shown whole. |

Both markers have the same next step: `filter get <idx>` shows the rule whole,
then `filter list <idx+1>` pages on. Re-reading the same line only shows the
same `..` again.

**Reply budget.** The node name and job key take their share of the packet,
leaving 97 bytes in the worst case (31-character name, 32-character key), about
93 bytes of text after the 3-byte marker, and more whenever either is shorter.
A reply that does not fit is cut where it stops fitting: text before the cut is
kept exactly, a single `...` marks the stop, and a reply that fits is copied
byte for byte, so the filter's own `...` survives untouched.

**Protocol guarantees:**

1. Replies are `txt_type = 0x00` plain group texts, visible in the companion's channel view.
2. Replies never parse as scripts: they start with the job key (charset `[A-Za-z0-9._-]`, never `!`), so they cannot match the `!id ` marker. Keep future reply formats key-prefixed for the same reason.
3. Flood is the only reply route. Group texts carry no pubkey, so a repeater cannot direct-reply to "alice" (sender-controlled text, not an identity). Every reply floods and is relayed by the fleet, using the repeater's path-hash size and its default region scope when one is set.

**Storm control.** A reply is scheduled at a uniform-random instant in a jitter
window measured from when the script finishes. The default is 15 s, set per node
with `fleet reply <secs>` (1-600 s). A storm is N near-simultaneous floods, so
widen the window to spread them: about **5 s per repeater** (3 nodes: default,
10 nodes: ~50 s, 20 nodes: ~100 s). Widening costs only latency; a narrow window
costs collisions. Set it fleet-wide from a script with `fleet reply 50`.
Pending replies (2 slots) are RAM-only: a reboot wipes them, so a re-sent `!ack`
job after a reboot re-runs and re-replies. A full reply store drops and counts
the reply (`refused`); there is no reply cooldown in v1, because per-key dedupe
already bounds each script to one reply.

**Retry.** After a reply floods, the repeater listens for a neighbour repeating
it. With none heard within ~5 s it re-sends once, byte-identically (same frozen
timestamp, node name and cut body), then stops. Nodes that already relayed
suppress the copy at the mesh layer, so it never appears as a duplicate message.
The name and room are frozen with the first attempt, so a later `set name`
cannot turn the copy into a different packet.

| Retry fact | Detail |
|---|---|
| Heard repeat | Proves a repeater relayed the reply, not that the companion received it. Flood group text has no end-to-end ack; each miss increments `rtx`. |
| Refused send | Retried after the configured backoff, with `rtx` incremented only when the retry is queued. Given up after a few attempts and counted as `refused`, so an unsendable ack cannot hold the reply slot forever and block later acknowledgements until reboot. |
| Build flags | `FLEET_ACK_RETRY`, `FLEET_ACK_REPEAT_WINDOW_MS`, `FLEET_ACK_RETRY_BACKOFF_MS`, `FLEET_ACK_SEND_MAX_ATTEMPTS`. Never persisted. `FLEET_ACK_RETRY=0` restores the single fire-and-forget send. |

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
  behaviour is exactly the classic one: one whole script per loop pass.
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
a single job every node runs **at the same instant** (no mesh-split risk) and
enables off-hours maintenance windows.

- **`<unix-ts>` is unix epoch seconds (seconds since 1970-01-01 UTC)**, as all
  unix timestamps are. Unix time carries no timezone; repeater logs display
  UTC (the `U` in log timestamps). Compute it with `date -u +%s`.
- **Stale is refused**: a timestamp more than 300 s in the past is refused
  (counted, key not marked): a typo'd stale date must never run immediately,
  since the damage it can do (a surprise `reset`) is exactly what scheduling is
  meant to control. Within the 300 s grace (clock skew, mesh transit delay) it
  executes immediately. A job whose key was already seen is a **duplicate**
  (counted as `dup`), whatever its timestamp says: the job ran or was armed
  once, and a re-sent copy is a duplicate, not a stale refusal.
- **Clock unset → refuse.** While the node's RTC is before 2025-01-01, `!at`
  scripts are refused and counted: executing at an unknown time is worse than
  not executing. The sync recipe below fixes this first.
- Armed jobs live in a dedicated RAM-only store (2 entries; a full store
  refuses new ones, counted, key not marked). **A reboot wipes them: re-send
  the job.** Ordinary scripts keep flowing while a cutover is armed.
- When the RTC passes the timestamp the script runs as normal: same deferral,
  same `!ack` behaviour (the reply fires after execution, so a scheduled job's
  replies are themselves jittered and confirm *when* each node actually ran
  it).

### The sync → verify → schedule recipe

Repeater RTCs drift (no GPS = minutes to hours off), and scheduled cutovers
executed by nodes with disagreeing clocks are *worse* than the two-phase pattern
they replace: nodes would bounce at different moments, exactly the mesh split
scheduling exists to avoid. Sync first, using existing commands only:

1. **Sync**: a fleet script running **`time <unix-ts>`**: re-send until every
   node has it (idempotent; `time` refuses to go backwards).

   **`clock sync` does not work inside fleet scripts**: it syncs from the
   message's sender timestamp, and scripts run with `sender_timestamp = 0`,
   so scripts must use `time` with an explicit epoch.
2. **Verify**: a `!ack` query job running **`clock`**: every node replies with
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
| `fleet` | Status: `on, chan h=XX, tags 2/8; scripts ran:3 dup:1 noid:0 refused:0; pending 0/2; sched 0/2; seen 2/32; ackq 0/2 await 0 rtx 0; badline 0` |
| `fleet on` / `fleet off` | Arm / idle the hooks (config preserved either way) |
| `fleet chan set <psk-hex>` | Set/replace the one fleet channel (16 or 32 bytes hex): `OK - fleet channel set h=XX (PSK holder = admin)` |
| `fleet chan clear` | `OK - fleet channel cleared`: receipt stops, queued and armed scripts are cancelled, seen keys and counters kept |
| `fleet tag add <tag>` | `OK - tag xiao added (2/8)`; duplicates no-op `OK` |
| `fleet tag del <tag>` | `OK - tag xiao deleted` / `Err - tag xiao not set` |
| `fleet tag list` | `tags: xiao siteA` or `tags: (none) - broadcast scripts only` |
| `fleet tag clear` | `OK - tags cleared` |
| `fleet reply [<secs>]` | Query or set the `!ack` jitter window (1–600 s; default 15) |
| `fleet seen` | How many job keys are remembered: `seen 2/32` |
| `fleet seen <key>` | `OK - key <key> seen` / `Err - key <key> not seen`: did this job run here? |
| `fleet forget <key>` | `OK - key forgotten`: re-arm one job for a deliberate re-run |
| `fleet forget all` | `OK - seen table cleared`: re-arm everything |

Counter meanings:

| Counter | Meaning |
|---|---|
| `ran` | Scripts executed since boot |
| `dup` | Messages refused because their key was already seen (the normal "re-send" case) |
| `noid` | Messages ignored because line 1 was not `!id …` (ordinary chat on the channel) |
| `refused` | Scripts refused: queue full, a bad `!id` key, a malformed or unknown `!` directive, a stale/unset-clock/full-store `!at` admission, or an acknowledgement the packet pool kept refusing |
| `badline` | Command lines skipped at run time for being over-long |
| `sched` | Armed `!at` scripts (`0/2`) |
| `ackq` | Acknowledgements queued but not sent yet (`0/2`) |
| `await` | Sent acknowledgements still awaiting a heard repeat (`0/2`) |
| `rtx` | Replies re-sent because the first attempt was not heard repeated |
| `crowded` | Packets on the fleet channel that core never offered the fleet key for (the filter's channel store filled core's four candidate slots for that hash) |

`ackq` and `await` share one two-slot store: a queued reply and a sent one
waiting for its repeat both hold a slot, so the store is full when the two
counts add up to 2 (`ackq 1/2 await 1` = full, no room for another
acknowledgement).

The status line is one line, and it is cut to the CLI's reply bound like every
other reply (a few bytes under 160). The counters are 32-bit, so a node that
has run for a very long time can push the line past that; the cut is
**marked**, never silent: the line ends with `...` and the operator knows the
status is incomplete.

## Two-phase fleet changes (radio presets and other cutovers)

A job that reboots a repeater on first receipt splits the mesh **before**
stragglers got the new settings: repeaters still on the old frequency would be
orphaned, and with them possibly your path back in. So cut over in two phases:

```text
# phase 1 (saturate): re-send this over a period of time (each send catches
# more of the fleet; it no-ops on repeaters that already ran it, and
# re-running it after a reboot is harmless because it only sets values)
alice: !id 2026-06-12-preset2-cfg
set radio 869.650,62.5,9,5

# phase 2 (commit): one-line job, sent once, only after phase 1 has reached
# every node (check with `fleet seen` over a normal admin session)
alice: !id 2026-06-12-preset2-go
reboot
```

`set radio` itself replies `OK - reboot to apply`, which is why the pattern
works so cleanly: the settings job is idempotent and repeatable, and the commit
job is a single reboot that only fires once everyone is ready.

Rollback is just a new key, and it runs everywhere, including repeaters that
already ran the first job:

```text
alice: !id 2026-06-12-preset2-fix
set radio 869.525,62.5,8,5
```

followed by its own `reboot` job once saturated.

When `!at` replaces this: once the fleet's clocks are synced (the
[sync → verify → schedule recipe](#the-sync--verify--schedule-recipe)), a
cutover can be a single armed job, settings applied and `reboot` in one script,
every node executing at the same coordinated instant:

```text
alice: !id 2026-06-12-preset2-at
!at 1781268000
set radio 869.650,62.5,9,5
reboot
```

Rollback after an armed cutover is the same shape on the old settings. Re-send
the armed job after any node rebooted in between (armed jobs are RAM-only).

## Recipes

Each recipe is one or more jobs on the fleet channel. `#` lines are comments:
do not send them.

| Goal | Job commands | Notes |
|---|---|---|
| Radio preset switch | See the [two-phase pattern](#two-phase-fleet-changes-radio-presets-and-other-cutovers) or its [armed variant](#scheduled-execution-at-unix-ts) | Keep your preset table (freq/bw/sf/cr values) in your own notes or a companion snippet: the repeater stores one active setting, so the job is just the value plus a commit. |
| Tame advert storms | `!id 2026-06-12-adv24`<br>`filter ratelimit advert 24` | Re-sendable forever; nodes that already ran it no-op. |
| Fleet filter update | `!id 2026-06-13-rule-add`<br>`filter add chan=#wardriving hops=[2,*] action=forward`<br>`!id 2026-06-13-rule-del`<br>`filter del 2` | Add the rule (re-sendable), then remove the old one in a **separate job** because indices shift. If you are not sure of the index, run `filter list` with `!ack` and read the replies, or use a normal admin session per node, then send del jobs to the cohorts whose layout you know. A mis-addressed `del` deletes the wrong rule: prefer `filter disable <idx>` first, watch the mesh for a day, then delete. |
| Rename nodes and refresh location | `!id 2026-06-14-names`<br>`!tags xiao`<br>`set name RIDGE-NORTH`<br>`set lat 47.6205`<br>`set lon -122.3493` | All set-state commands: safe to re-send, safe to re-run after a reboot. |
| Rotate the channel after a suspected key leak | `fleet chan set <new-psk>` as the last line of a job sent over the *old* channel | `fleet chan set` is a one-channel config, so rotation is a single step, not an add-then-retire. Set a fresh channel and verify the new path works before retiring the old one. Roll the new key out by direct admin per node, or by that script, keeping the old channel configured in the companion until every node has rotated; `fleet seen` over the old channel tells you which nodes already rotated, since they stop seeing jobs there. The old key still opens the channel, so speed matters; in the meantime `fleet off` over normal admin, per node, closes the door. |
| Emergency quiet mode | `!id 2026-06-16-quiet`<br>`filter off` | Stops the fleet repeating without bricking your reach-back: each node still answers its own admin. Recover with `filter on`, in the same shape or per node. For a harder stop, `set repeat off` in its own job; recovery needs an explicit `set repeat on` job, which is itself re-sendable. |
| Canary rollout and cohorts | `!tags canary` on the first jobs<br>`fleet tag add canary` to widen | Tag a few repeaters `canary` and send `!tags canary` jobs first; widen the tag after verification (the add is itself a broadcast script). Cohorts by board/site/role work the same way. Hierarchies are shared tags: give every node a common `all` plus specific ones (`xiao`, `siteA`, `roof`), send `!tags all` for everyone or `!tags xiao` for that cohort; more tags (location, firmware channel, role) form further overlapping groups. |
| Remote fleet re-configuration | `!id 2026-06-17-rooftags`<br>`!tags all`<br>`fleet tag add roof` | A script line that runs `fleet …` is fine (the drain is outside the receive path). **Note:** The obvious footgun is a script that runs `fleet chan clear` on the whole fleet, the same class as deleting the filter rule that delivered it. |
| Online firmware download (observer builds) | `!id 2026-11-24-online-fetch`<br>`!tags canary`<br>`!ack err`<br>`ota fetch ver 1.18.0-dev.11`<br>`ota fetch sha sha256:39483a72f358ea976125928a45394115d452ec29b4dbe4bf35c9e2ddf3f2d826`<br>`ota fetch start`<br>`!id 2026-11-24-online-apply`<br>`!tags canary`<br>`ota apply` | Target only observer nodes, since a plain repeater replies `Err - unsupported`. The `start` reply is immediate, so a script never waits for the download, and image bytes never travel over LoRa. Once `ota status` shows `ready`, apply with the second job, which `!at` can schedule. Each node fetches independently over its own WiFi; the fleet PSK remains full admin access. See [ONLINE_OTA.md](./ONLINE_OTA.md) for digest syntax, errors and limits. |

## Status, logs and troubleshooting

`fleet` is the feature's dashboard; `fleet seen <key>` over a normal admin
session answers "did node X get job Y?" The serial console carries the full
transcript:

```text
cli[2026-06-12-preset2-cfg] set radio 869.650,62.5,9,5
  -> OK - reboot to apply
```

| Symptom | Check | Usual cause |
|---|---|---|
| Job never ran anywhere | `fleet seen <key>` per node; `noid` counter | Message sent without the `!id` first line, or on the wrong channel |
| Commands did nothing one at a time | `noid` counter climbing | Not a job: send the lines together with `!id` first (see Quick start) |
| Some nodes ran it, some didn't | `refused` counter on the stragglers | Their pending queue was full (a third job arrived before the first drained); re-send the job |
| Script targets nobody | `fleet tag list` on the nodes | Tag typo (exact matching: `xiao ` ≠ `xiao`); or all nodes lack the tag and `!tags` was present |
| No `!ack` reply arrived | `ackq` on the node; window too long? | Reply still jittering (up to `reply window` s); store was full (`refused`); or the channel changed between receipt and send |
| `rtx` climbing | Neighbouring repeaters in range? | Replies are not being heard repeated: likely no neighbouring repeater, or heavy loss; each miss is one extra byte-identical resend (never a duplicate, since nodes that already relayed suppress the copy) |
| `dup` climbing, nothing changed | - | Normal: re-sends and duplicate flood deliveries |
| Job ran twice | seen-ring wrap | 33+ distinct jobs this boot evicted the key; re-send deliberately via `forget` semantics |
| Lines missing from a run | `badline` counter | Lines over the CLI's length limit were skipped: split the job |
| Reply cut (`...` or `..`) | - | See Acknowledgements: `...` is a cut reply or filter page marker, `..` is an abbreviated rule; page on with `filter get <idx>` / `filter list <idx+1>` |
| Whole script refused, `refused` up | - | Unknown `!` directive, bad key characters, or a malformed `!tags`/`!ack`/`!at`/`!delay`; check the exact text |
| `!at` job never fired | `sched` count; `clock` reply per node | Clock not synced (below the 2025 floor → refused), or the RTC is past it and it fired already; or a reboot wiped the armed job: re-send |
| `!at` refused, `refused` up | - | Timestamp > 300 s in the past (stale), RTC unset, or the scheduled store was full |
| Nothing on the fleet channel is ever decrypted | `crowded` counter on the node | Core tries at most **4** candidate keys per on-air channel hash, and the packet filter's channel store is offered first. If the filter's store holds 4 *different* keys on the fleet channel's hash, the fleet key is never tried: the fleet channel goes deaf. The `crowded` counter counts the packets that were never offered to it. Fix it by removing a filter channel (`filter chan del <name>`) that shares the hash, or by using a different fleet PSK |

## Interactions

### Kill switches

`fleet off` and `fleet chan clear` stop the fleet at once: receipt stops, and
every queued, sleeping and armed script is cancelled (its key stays consumed,
see [Security model](#security-model)), as are queued and awaiting
acknowledgements. A script that switches the fleet off runs its remaining lines
up to its next `!delay`, and nothing after that. `fleet forget <key>` re-arms one
job. `filter off` does not stop fleet; the
[battery gate does not stop fleet either](#battery-supersession).

### Battery supersession

**Note:** Fleet management works when the battery is low: a node in trouble
is exactly the node you need to reach. All three of the battery gate's reach are
exempt for the fleet channel:

- **Execution**: fleet scripts enqueue and run while forwarding is suspended.
- **Relaying**: fleet-channel traffic relays while suspended, and is never
  counted as a battery drop. (The exemption is exactly the packet the repeater
  has just MAC-verified under the fleet key. The one-byte channel hash only
  narrows the search, so other channels that share it stay gated.)
- **Acknowledgements**: replies are sent regardless of gate state: the
  operator chose `!ack`, and it is the operator's call whether to spend
  battery on a response. A battery report from a *low* node is the most
  valuable one.

Fleet members also **emit battery alerts** on the channel: a suspend or resume
transition is announced as an ordinary `<name>: BATT SUSPEND|RESUME <mV>` group
text, and these relay while suspended like any other fleet traffic. See
[BATTERY.md](./BATTERY.md#alerts).

Normal user traffic stays gated exactly as BATTERY.md describes. Decryption is
unaffected by the gate in the first place; a script executed while suspended
runs with full serial privilege, and what its commands *send* is governed by
the commands themselves. The two-phase cutover gets **simpler**, not riskier: a
suspended node still receives, runs, and retransmits fleet traffic, so it can't
be orphaned from a fleet cutover by its own battery state.

## Security model

The channel key is the credential. Its MAC gate is the only thing between the
world and your scripts. Key holders are equals: there is no per-sender
distinction, and tags are not authentication.

| Topic | How it works |
|---|---|
| Per boot | Executed job keys live in RAM only and start empty on every boot. A key runs at most once per boot and is marked at admission, when the script is queued. Duplicates and deliberate re-sends no-op but are still relayed, at no cost. |
| Seen ring | 32 entries. A 33rd distinct job evicts the oldest key and re-arms it. |
| Inspect and re-arm | `fleet seen <key>` answers "did this job run here?" over a normal admin session, one per repeater. `fleet forget <key>` re-arms one key and `fleet forget all` re-arms everything. A job still queued, sleeping or armed is never doubled: a re-send stays refused until it has run or been cancelled, even after a `forget`. |
| Re-send | Re-send freely while the fleet is up to catch nodes that were offline or out of range. Repeats are free: they no-op on the seen table and are still relayed. |

**Note:** After a reboot, keys are forgotten and a re-sent job runs again. Write
scripts to be re-runnable:

- State-setting commands (`set …`, `filter add …`, `filter ratelimit …`, `battery …`) are safe to repeat forever.
- Index-based mutations (`filter del 3`, indices shift) and one-shot effects (`reboot`, `start ota`) belong in their own jobs, sent deliberately and once.
- A captured message replayed after a reboot re-runs. Cross-reboot replay immunity is not a property of this feature; if that matters, keep one-shot jobs off the channel and do them over direct admin.

**Leak response.** Treat every fleet member as compromised, rotate the PSK
([channel rotation](#recipes)), and use `fleet off` per node over normal admin
in the meantime.

**Bounded resources.** Payload-sized scripts, a 2-slot run queue, 2 scheduled
entries, 2 reply slots, a 32-entry seen ring, one script per packet. No
amplification: a member relays the message once and runs its own copy locally,
and `!ack` replies are jittered against storms.

## Limits

| Limit | Value |
|---|---|
| Script per message | ~175 bytes of script (`!id` line included) |
| Pending scripts | 2: a third before the queue drains is refused, but **not** marked seen; a re-send reaches it |
| Armed `!at` scripts | 2 (RAM-only; a reboot wipes them) |
| Pending acknowledgements | 2 (RAM-only; a full store drops and counts) |
| `!ack` reply window | 1–600 s; default 15 s (~5 s per repeater) |
| `!ack` repeat confirmation | one byte-identical resend after ~5 s with no repeat heard; RAM-only, build-time tunable (`FLEET_ACK_*`) |
| Seen job keys | last 32 per boot (RAM-only; a 33rd evicts the oldest and re-arms it) |
| `!id` key | 1–32 chars of `[A-Za-z0-9._-]` |
| Tags | up to 8 on a repeater; up to 8 per `!tags` list; 1–16 chars each |
| `!delay` | 1–300000 ms per pause; a sleeping script holds its queue slot |
| Command lines | up to the serial CLI's buffer (~160 bytes); longer lines are skipped |
| Reply content | up to 97 bytes of room per acknowledgement in the worst case (a 31-character node name and a 32-character job key); a shorter name or key buys more; about 93 bytes of reply text after the 3-byte cut marker. Cuts are marked: see Acknowledgements |

- Execution is **deferred**: a queued script runs in the main loop, outside the
  packet handling that queued it, so scripts may safely contain `filter …` and
  `fleet …` commands that mutate config.
- All script state is RAM-only. Job keys, queue, armed jobs and counters reset
  on reboot: the channel, tags and reply window persist in `/fleet_cfg`; the
  script machinery does not.
- `filter stats reset` does not touch the fleet counters; they reset only on
  reboot.
