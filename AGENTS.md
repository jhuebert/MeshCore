# Agent Guide — jhuebert/MeshCore (personal fork)

This is my personal fork of `meshcore-dev/MeshCore`. The fork-specific features
are the **repeater packet filter** (`filter ...` CLI, drop/forward rules, advert
rate limiter), the battery gate, mesh clock sync, fleet management, and an
optional **MQTT observer** firmware — all carried on the `repeater-filter*`
branches — plus firmware release channels released by
`.github/workflows/filter-release.yml` (tag + release object; firmware assets are
built locally).

Remotes: `origin` = upstream `meshcore-dev/MeshCore` (never push there),
`fork` = `jhuebert/MeshCore` (push here). Upstream has **no** AGENTS.md, so this
file is fork-owned and does not conflict with upstream syncs.

## Patterns become rules — ask first

When a way of working turns out to persist — a convention the user applies
repeatedly, a preference stated more than once, a pattern that emerged from a
finished change and will likely recur — **ask the user** whether to codify it
in this file before relying on it again. Never silently start following an
unwritten rule, and never edit this file on your own initiative. Proposed
wording plus a one-line rationale; the user decides.

## Repo docs & code map — read this, then start

Everything needed to begin work is here or in the two files below; an agent
should not need to explore beyond them before making a change.

**User guides (authoritative for behavior; keep in sync with code):**
- `FILTER.md` — the packet filter's user manual: rule model, every predicate
  (`type`, `route`, `hops`, `len`, `snr`, `path`, `hsize`, `chan`, `chanhash`,
  `region`, `sender`, `text`, `prob`, `throttle`, `action`), full command
  reference, TinyRegex syntax + limits, real-world setups, quoting/sending
  notes, troubleshooting; ends with a brief pointer to the fleet feature.
  First stop for any CLI-semantics or least-surprise question.
- `FLEET.md` — the fleet management user manual: the `fleet chan set` +
  `fleet tag add` setup, script format (the `!tags`/`!ack`/`!at` directive
  block, inline `!delay`), tag targeting (exact equality), acknowledgements
  and the reply-jitter window, the sync → verify → schedule recipe for `!at`,
  idempotency, two-phase cutovers, recipes, troubleshooting and limits. First
  stop for anything about the fleet feature.
- `BATTERY.md` — battery gate user manual: behavior, what suspension does
  not touch, persistence, CLI reference.
- `MESHCLOCK.md` — mesh clock sync behavior, CLI, defaults and caveats.
- `MQTT_OBSERVER.md` — the optional MQTT observer firmware's user manual:
  build targets, the `mqtt*`/`wifi*` CLI, broker setup (JWT vs password, TLS),
  topic templates and the status/packet/raw schemas, the capture and delivery
  model, recipes, troubleshooting and limits.

**When behavior changes, the matching guide section is updated in the same
commit** — the guides are user-facing API documentation, not optional docs.

**Code (`examples/simple_repeater/`):**
- `PacketFilter.h/.cpp` — rule model, evaluation (`checkPacket` packet-level,
  `checkContent` decrypted-content single pass, verdict stash), binary
  persistence (`load`/`save`, v3..v7 stepwise migration), `filterCLI`.
- `FleetManager.h/.cpp` — the fleet feature: one PSK-backed channel, the
  repeater's tag set, `fleetCLI`, the receive-side hook (`onGroupData`), the
  relay-side battery exemption (`checkForward`), acknowledgements (jittered,
  one byte-identical retry until a repeat is heard, sent on the captured
  channel), unsolicited alerts (`sendAlert`, on the
  configured channel), scheduled `!at` jobs (RAM-only store), and the
  `/fleet_cfg` persistence (v1 with a whole-payload CRC-16). Owns a
  `CliScriptRunner`.
- `CliScript.h/.cpp` — the fleet script runner: `!id` parse, the
  `!tags`/`!ack`/`!at` directive block, inline `!delay` with resumable run,
  the 2-slot pending queue, the seen-key hash ring, and the run callback
  MyMesh drives (see the behavioral-invariants note).
- `AdvertRateLimiter.h/.cpp` — the per-origin advert repeat window: cache,
  counters, and the check/commit split (see the fork's rule-model note).
- `PersistUtil.h` — the lazy dirty-save pattern, the per-platform file opens, and
  the shared staged-save transaction; also the config-integrity note.
- `PacketFilterConfig.h` — the filter's capacity tunables (`FILTER_MAX_RULES`,
  ...), override via build flags; persistence-layout caveats noted inline.
  `FleetConfig.h` holds the fleet's (`FLEET_*`, including the renamed
  `FLEET_CLI_*` runner capacities and the `!tags`/`!delay`/`!at` grammar
  limits).
- `TinyRegex.h/.cpp` — vendored kokke/tiny-regex-c + step budget (see Hard
  invariants; treat as upstream).
- `PatternMatch.h/.cpp` — the user-facing pattern language: top-level `|`
  alternation, structural validation (empty alternative, alternative cap); thin
  wrapper over the vendored engine.
- `CliUtil.h` — shared CLI helpers (`nextToken`, `CliBuf`, `cliPrepare`,
  `cliCutReply`, `cliIsErrorReply`, `CLI_REPLY_MAX`) used by the filter,
  battery, fleet and clock CLIs alike: the bounded reply writer, the command
  entry point, the marked cut, and the one place that knows what a failing
  reply looks like (the fork's `Err - ...` and the core's own vocabulary).
- `BatteryGate.h/.cpp` — low-battery forward suspension, `battery` CLI, and a
  RAM-only one-shot suspend/resume transition readout (`takeStateChange`).
- `MeshClockSync.h/.cpp`, `MeshClockSyncConfig.h`, and `BuildEpoch.h` — advert
  sample collection, median/quorum decision, enabled-flag persistence and CLI.
- `MyMesh.cpp/.h` — upstream files carrying the fork's hook lines
  (`allowPacketForward`, `onGroupDataRecv`, `logRx` (ack repeat detector),
  `logRxRaw`/`logRx`/`logTx` observer capture, `searchChannelsByHash`, CLI
  dispatch, lazy-save loop, the `execCliLine` script-execution trampoline,
  the fleet device-service injections and the `batteryAlerts` transition
  drain).
- `main.cpp` — upstream's serial CLI entry (no fork lines).

**Code (`examples/mqtt_observer/`, compiled only under `WITH_MQTT_OBSERVER`):**
- `MQTTObserver.h/.cpp` — the ESP32-only observer: Wi-Fi/NTP, up to two broker
  slots, Ed25519 JWT or user/pass auth, the receive/TX capture hooks, a bounded
  RAM event queue, the heap-headroom connect gate, and the `mqtt*`/`wifi*` CLI.
- `MQTTObserverConfig.h` — the config model + `MQTTObserverConfigCodec`
  (`/mqtt_prefs` payload v5; `MQTT_OBSERVER_MAX_BROKERS`, queue capacity).
- `MQTTObserverRecord.h` — the `/mqtt_prefs` record framing (magic `MQO1`,
  CRC-16 over the payload) and its header/version/checksum checks.
- `MQTTObserverPolicy.h` — pure host-tested policies: command/slot parsing, JWT
  renewal, the queue/retry rules, the connect gates (heap admission, attempt
  outcome), the liveness probe, and the raw frame stager.
- `MQTTObserverCli.h/.cpp` — the `mqtt*`/`wifi*` command handling, separated
  from the radio, the Wi-Fi stack and the MQTT client: the observer fills in
  the device facts (Wi-Fi, clock, heap, queue, slot states) and the unit owns
  the config and the replies. Host-tested, like the filter's and the fleet's
  CLIs.
- `MQTTObserverFormat.h/.cpp` — pure host-tested JSON builders for the status,
  packet and raw messages (the reference observer schema).
`observer_targets.ini` (repo root) defines the optional `*_repeater_observer`
PlatformIO environments. It is pulled in through `platformio.ini`'s
`extra_configs`, so the environments it adds are part of the resolved config
(`build.sh list` enumerates them); what marks an environment as an observer is
the `_repeater_observer` name suffix, not membership in that file.

**Tests:** the behavior-level (native googletest) suites, one per feature, each
reached through the same entry points a user or the firmware would:
`test/test_packet_filter/` (matching, content rules, limiter, persistence
upgrades, CLI surface, TinyRegex, PatternMatch), `test/test_fleet/` (runner
grammar, tag targeting, acknowledgements, scheduling, persistence, CLI surface,
the battery exemption contract), `test/test_battery_gate/` (suspension
behavior, persistence, CLI), `test/test_mesh_clock/` (sample collection, quorum
decision, persistence, CLI) and `test/test_mqtt_observer/` (config codec +
record framing, URI/topic builders, CLI and slot parsing, JWT renewal, the
connect gates, queue/retry policy, the liveness probe, and the
status/packet/raw JSON schema). Test-only shims:
`NativeShim.h`, `NativeTestStubs.cpp`, `RegionMapStub.cpp`,
`FilterTestHelpers.h`, `CliReplyHelper.h`.

**Build & CI:** `pio test -e native_packet_filter` / `-e
native_battery_gate` / `-e native_fleet` / `-e native_fleet_noack` (the fleet
suite with `FLEET_ACK_RETRY=0`, whose test bodies live in `#else` branches no
other env compiles) / `-e native_mesh_clock` / `-e native_mqtt_observer`
(host-native, no hardware), plus `-e
native_packet_filter_san` / `-e native_battery_gate_san` / `-e native_fleet_san` /
`-e native_mesh_clock_san` / `-e native_mqtt_observer_san` for ASan+UBSan;
firmware via `bash build.sh build-firmware <target>` (`bash build.sh list` —
`build.sh` uses bash arrays, so `sh` is not a contract), the filter fork's flash
target being `Xiao_S3_WIO_repeater` (observer builds add
`Xiao_S3_WIO_repeater_observer`). **Compile-check every filesystem
family** before calling persistence work done: `Xiao_S3_WIO_repeater`,
`RAK_4631_repeater` (nRF52), `PicoW_repeater`, `wio-e5_repeater` — host-native
tests cannot catch a platform-specific API difference, and one did (the nRF52
file type has no default constructor).
`.github/workflows/filter-release.yml` cuts release channels (dev →
`repeater-filter`, stable → `repeater-filter-stable`) — tag + release object
only; the firmware assets are built locally with
`scripts/local-release-build.sh` (the default target set includes the observer
targets, so they ship in the same release — no separate observer channel) and
uploaded to the release with `gh`
(CI no longer builds firmware — the wide runner fan-out got Actions
shut down). `sync-upstream.yml` merges upstream into the chosen channel branch
and `promote-stable.yml` cherry-picks the filter commits from `repeater-filter`
onto `repeater-filter-stable`; both are manual-dispatch only (never on a
schedule) and neither cuts a release — use Filter Release for that. A conflict
in either one opens (or updates) an issue through
`.github/actions/open-conflict-issue/` and fails the run; the promotion also
skips a pick that conflicts when the source branch later rewrote that commit
(squash) and only publishes if the branch then matches the source for every
file the source changed since its upstream merge base. The promotion runs a
quality gate (the native suites plus the build-tooling tests in
`scripts/tests/`) before it publishes. Upstream's own docs live in `docs/`
(MeshCore protocol/CLI generally) — not fork-filter documentation.

## Branch model

- `repeater-filter` — dev release channel; the integration branch for all
  filter work. Default branch of the fork.
- `repeater-filter-stable` — stable release channel; updated from
  `repeater-filter` deliberately via `promote-stable.yml`, not automatically.
- Work branches: `repeater-filter-<topic>` cut from `repeater-filter`, merged
  back with `--no-ff` (keep feature commits traceable).
- Upstream is tracked via the manually-dispatched `sync-upstream.yml`
  workflow; do not hand-merge upstream `dev` locally unless fixing a sync
  failure.

## Fork-owned vs upstream code

Fork-owned file set (free to edit):
- `examples/simple_repeater/PacketFilter.h/.cpp`, `PacketFilterConfig.h`
- `examples/simple_repeater/TinyRegex.h/.cpp`, `CliUtil.h`, `BatteryGate.h/.cpp`
- `examples/simple_repeater/PatternMatch.h/.cpp`
- `examples/simple_repeater/CliScript.h/.cpp`
- `examples/simple_repeater/AdvertRateLimiter.h/.cpp`, `PersistUtil.h`
- `examples/simple_repeater/MeshClockSync.h/.cpp`, `MeshClockSyncConfig.h`,
  `BuildEpoch.h`
- `test/test_packet_filter/`, `test/test_fleet/`, `test/test_battery_gate/`,
  `test/test_mesh_clock/`, `test/test_mqtt_observer/`
- `examples/mqtt_observer/`, `observer_targets.ini`
- `FILTER.md`, `FLEET.md`, `BATTERY.md`, `MESHCLOCK.md`, `MQTT_OBSERVER.md`
- `scripts/local-release-build.sh`, `scripts/tests/`
- `.github/workflows/filter-release.yml`,
  `sync-upstream.yml`, `.github/workflows/promote-stable.yml`
- `.github/actions/open-conflict-issue/` (the conflict issue the two sync
  workflows open; `setup-build-environment/` next to it is upstream's)

Fork lines inside upstream files stay **minimal** — a reviewer should see a
handful of lines, not a fork interleaved into upstream code. Those files are
`examples/simple_repeater/MyMesh.h/.cpp` (the hooks the features call),
`build.sh` (the observer commands and `build_observer_firmwares`),
`platformio.ini` (the fork's native and sanitizer environments, and the
`observer_targets.ini` include), plus small patches in
`examples/companion_radio/` and `test/mocks/SHA256.h`; `README.md` is the
fork's own. Everything else is upstream: apply the change discipline below
*even more strictly* there, and match the existing style (`.clang-format`:
2-space indent, `camelCase` functions, `UpperCamelCase` classes, lines < ~100
chars).

## Change discipline — every change (fork or upstream)

- **Minimum viable change.** The smallest diff that fully fixes the problem:
  no drive-by refactors, no renames, no reformatting of untouched lines, no
  dependency upgrades folded into an unrelated change. Re-read the final diff
  and confirm every touched line is required and nothing else changed.
- **DRY.** Shared logic gets one authoritative home, and nothing re-implements
  it. Precedents: the CLI tokenizer and reply writer in `CliUtil.h`; the match
  gates + hit/airtime commit in `FilterRules::decideMatch()`. If you find
  yourself copying a block from a sibling function, factor it instead.
- **Deslop as you go.** Code you touch must end up cleaner than you found it
  where slop is in reach: duplicated logic factored, unbounded writes bounded,
  dead state removed, twin branches merged. But don't deslop code you aren't
  otherwise touching — that's churn, not cleanliness.
- **Simplicity and maintainability.** The simplest code that is correct;
  obvious beats clever; write what the project's maintainers would have
  written, not what an agent would. Comments explain *why* (invariants,
  protocol quirks, on-air format constraints), not what.

## Behavior — principle of least surprise

CLI and filter semantics must work the way a user would guess without reading
FILTER.md:

- `filter get <idx>` is an **inspection** command: its reply shows the rule's
  predicates plus counters, is truncated to the CLI reply bound (see hard
  invariant 3), and is *not* re-addable input (`add` rejects the
  `hits=`/`air=`/`pass=` metadata it prints).
- `filter chan list [<start-idx>]` exists because a full store does not fit one
  reply and `chan del` takes a name; a truncated listing ends with `next=N`.
- Errors say what's wrong and what's accepted (e.g. `Err - chanhash must be 2
  hex chars`); usage lines enumerate the command surface.
- Syntax reads like the concept: `[a,b]`-style intervals, comma lists, `*` =
  wildcard, first match wins like firewall rules, `prob=`/`throttle=` gates
  behave exactly as FILTER.md describes them.
- Replies stay short and stable — treat existing reply strings as user-facing
  API. Humans parse them, and companion tools may too.

## Backwards compatibility

- **Persistence is paramount.** A config written by any firmware version still
  in the field must load with settings intact. A change is either directly
  backwards compatible (new fields ride tail padding; the old record is a
  byte-identical prefix of the new one — the `static_assert` rule below) or it
  ships migration code in `load()` plus a `FILTER_CFG_VERSION` bump. Within
  reason: layouts older than the versions whose record code is kept in
  `load()` (currently v3..v6, migrated stepwise to v7) were already discarded
  by design and need no migration. Every version bump must keep older configs
  loading — add tests for the upgrade path (roundtrip + old-version fixtures,
  as the persistence tests already do).
- **CLI surface is compatibility too.** Never break existing command syntax,
  reply formats, or index semantics (0-based, shifting on delete).
- **Behavioral invariants are compatibility.** First-match-wins ordering,
  prob-before-throttle, stats semantics (hits travel with the rule on
  move/del; a stats reset never grants a throttle free pass) are observable
  behavior. So is the fleet model: a `!id` job key runs at most once per
  boot (marked at admission, RAM-only seen ring), targeting is by exact tag
  equality via `!tags` (absent = broadcast), the fleet channel is a single
  unnamed PSK that supersedes the battery gate (scripts run, fleet traffic
  relays, acks send while suspended), armed `!at` jobs and pending acks are
  RAM-only, battery alerts are emitted on the fleet channel only when it is
  enabled and configured, and script execution is deferred out of the receive
  path. Changing them is a breaking decision, not a refactor side effect.
  The observer's observable surface is compatibility too: capture happens before
  forwarding policy (a packet the node drops is still observed), the feed is
  best-effort (no flash replay, no delivery guarantee), TX uplinks this node's
  own adverts by default, the topic template and the status/packet/raw schemas
  follow the reference observer, and `/mqtt_prefs` (v5, magic `MQO1`) is the
  fork's own format, deliberately not migrated from the other observers.
- **New pattern syntax must not silently change the meaning of an
  already-storable pattern.** If it would, either ship an escape (`\|`) or
  reject the ambiguous form with an error. Deliberate exception: `|` became OR
  in 2026-10 (alternation) — a stored pattern containing an unescaped `|`
  previously matched a literal pipe.
- **Config integrity is checked per record where the format allows it.** A
  truncated save is detected on load; a torn write of exactly the right length
  is caught only by a record checksum. Since the 2026-10 v7 bump, each rule
  record carries a CRC-16 over its payload (crc16_ccitt in PersistUtil.h);
  older records cannot, so they stay length-checked — a checksum rides a
  version bump, never a patch. Channel records and the battery config keep
  the length-only story until their own layout change adds one. No sidecar
  file (a second file is a second thing to go missing). Rationale: stops a
  later "improvement" from quietly weakening config compatibility.
- **Old-version migration is temporary.** v3..v6 loading is kept for now and
  is expected to be dropped once the field has migrated. The migration chain
  is one named step function per version pair (`migrateV3ToV4` …
  `migrateV6ToV7` in PacketFilter.cpp), kept contiguous — never skip a middle
  step in code. Removing a version later is one excision: delete its step
  function, raise the floor in `rule_bytes_for()`, delete its fixtures.

## Hard invariants (do not break)

1. **Config persistence layout.** See *Backwards compatibility* above; the
   `static_assert`s in `PacketFilter.h` are the enforced form of that rule.
2. **Vendored TinyRegex.** Only the documented modifications (step budget,
   `re_budget_exhausted()`, unsigned-char literal compare, and the `matchone()`
   operand fix) — no other edits, or the vendoring diff against
   kokke/tiny-regex-c becomes unmaintainable. **A bug fix is the only reason to
   touch vendored source, and only after asking first:** not every bug is worth
   a vendored edit — weigh a one-line engine fix against reimplementing the
   check in the wrapper, and propose the trade-off (diff size, hot-path cost,
   what else it would pin down) rather than deciding unilaterally. Features,
   syntax and tuning are never a reason.
3. **CLI reply bound: 157 bytes, in a 160-byte buffer.** All writes bounded:
   one-liners via `snprintf(reply, CLI_REPLY_MAX, ...)`, staged output via
   `CliBuf`; never unbounded `sprintf` into `reply`. `CLI_REPLY_MAX` is 3 bytes
   under the upstream buffers (`main.cpp`'s serial/ethernet `char reply[160]`,
   the mesh remote-CLI's `&temp[5]` of `temp[166]`) because the core's
   companion-radio `xx|command` form puts that 3-byte prefix in front of the
   reply the handler writes *into the same buffer* — those buffers are
   upstream's, the fork only keeps its replies short enough not to overrun them
   (`static_assert` in `CliUtil.h`). A fork-owned buffer the prefix is written
   into is `CLI_REPLY_BUFFER_MAX` (the fleet's script runner). Shared CLI
   helpers (`nextToken`, `CliBuf`, `cliPrepare`) live in `CliUtil.h` — don't
   duplicate them.
4. **First match wins.** Rule evaluation order (listed order, packet-level in
   `checkPacket()`, whole-list single pass in `checkContent()`, stash
   consumption) is load-bearing; the prob roll runs before the throttle gate.
5. **Pattern syntax lives in the wrapper, not in the vendored engine.**
   `examples/simple_repeater/PatternMatch.*` owns every user-visible syntax
   decision (alternation splitting, structural validation, the alternative
   cap). `TinyRegex.cpp` keeps only the documented modifications and
   `TinyRegex.h`'s only role is the engine API. A change that adds syntax by
   editing the engine violates this. Fixing an engine *bug* does not (see 2),
   but it must not grow into a second dialect living in two places.

## Tests & verification — all code paths covered

- Every behavior change ships with tests covering its new or changed code
  paths, added in the same commit. The suites are behavior-level (native
  googletest); reach them via the same CLI/`checkPacket`/`checkContent`
  entry points a user or the firmware would.
- **Stack is a boot-breaking constraint the suites cannot see.** `setup()`/
  `loop()` run in a hard-capped 4 KB FreeRTOS task on nRF52 and an 8 KB
  `loopTask` on ESP32, and a big scratch array in boot- or loop-path code
  crash-loops every repeater flashed with the release (2026-10: ~8.5 KB of
  scratch in `FilterRules::load()` did exactly that). Measure frames with
  `PLATFORMIO_BUILD_FLAGS=-fstack-usage pio run -e <env>` before committing
  large local arrays; the stack gate in `scripts/local-release-build.sh`
  fails any release target whose `src/`/`examples/` code holds a frame
  >= 2 KB (formerly a filter-release.yml CI check) — keep new frames well
  under it.
- Pure refactors keep the suites green **unchanged** — the suites are the safety
  net that proves no behavior slipped.
- Run the feature suites, then re-read the diff:

```
pio test -e native_packet_filter    # filter + PatternMatch + TinyRegex + CLI + persistence
pio test -e native_battery_gate     # battery gate
pio test -e native_fleet            # fleet manager + runner + CLI + persistence
pio test -e native_fleet_noack      # the same, with FLEET_ACK_RETRY=0 (the off switch)
pio test -e native_mesh_clock       # mesh clock sync
pio test -e native_mqtt_observer    # observer config/format/policy
```

All green, and every touched line required — together with the full host-env
run, since the generic envs are where a broken selection shows up:

```
pio test -e native -e native_kiss_modem -e native_packet_filter -e native_battery_gate -e native_fleet -e native_fleet_noack -e native_mesh_clock -e native_mqtt_observer
```

Touching firmware-hook or persistence code additionally warrants compile checks
of `Xiao_S3_WIO_repeater`, `RAK_4631_repeater`, `PicoW_repeater` and
`wio-e5_repeater`. Note that PlatformIO does **not** track the
force-included `test/test_packet_filter/NativeShim.h` as a dependency: after
changing a struct it defines, clean the native envs (`pio run -e
native_packet_filter -t clean`) or the stale objects will silently disagree
with the new layout.

## Commit / PR discipline

- Commit style: conventional prefixes as used in the fork history — `feat:`,
  `fix:`, `ci:`, `docs:`, `refactor:` (no behavior change). Descriptive, one
  concern per commit.
- Work stays on the **fork**. Opening a PR against `meshcore-dev/MeshCore`
  requires explicit human approval of the diff first.
- Upstream CONTRIBUTING.md: an agent-opened PR with `🤖🤖` in the title opts
  into fast-tracked merging.
