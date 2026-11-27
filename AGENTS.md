# Agent Guide: jhuebert/MeshCore (personal fork)

## Project & ownership

This is my personal fork of `meshcore-dev/MeshCore`. The fork-specific features
are the repeater packet filter (the `filter` CLI, drop/forward rules, advert rate
limiter), the battery gate, mesh clock sync, fleet management, and an optional
MQTT observer firmware. They are carried on the `repeater-filter*` branches.
Firmware release channels are cut by `.github/workflows/filter-release.yml` (tag
and release object; firmware assets are built locally).

Remotes: `origin` is upstream `meshcore-dev/MeshCore`, never push there; `fork`
is `jhuebert/MeshCore`, push here only when explicitly asked. Upstream has no
AGENTS.md, so this file is fork-owned and does not conflict with upstream syncs.

## Agent conduct

These rules apply to every change, including documentation-only changes.

- **Ask before codifying a pattern.** When a way of working recurs (a convention
  applied more than once, a preference stated twice, a pattern from a finished
  change), propose wording plus a one-line rationale and ask before adding it
  here. Never follow an unwritten rule silently, and never edit this file on
  your own initiative.
- **Commit only when asked.** Commit when Jason asks or clearly wants one. Never
  bundle unrelated cleanup with an implementation commit.
- **Ask before publishing.** Do not create a GitHub issue, PR, release, tag, or
  published artifact, push to a remote, or trigger `Filter Release`,
  `promote-stable`, or `sync-upstream` without Jason's explicit request.
- **No em dashes anywhere.** In this guide, fork docs, code comments, commit
  messages, CLI help and replies, and any other user-facing text. Use a short
  sentence, a colon or a comma instead. Replace one on any line you touch.
- **Minimum viable change.** The smallest diff that fully fixes the problem: no
  drive-by refactors, no renames, no reformatting of untouched lines, no
  dependency upgrades folded into unrelated work. Re-read the final diff and
  confirm every touched line is required and nothing else changed.
- **Keep the upstream diff small.** Prefer fork-owned files. Where a hook in an
  upstream file is unavoidable, keep it to a handful of lines so syncs stay
  cheap and the fork remains easy to rebase. See *Fork-owned vs upstream code*.
- **DRY.** Shared logic gets one authoritative home, and nothing re-implements
  it. Precedents: the CLI tokenizer and reply writer in `CliUtil.h`, and the
  match gates plus hit/airtime commit in `FilterRules::decideMatch()`. If you
  find yourself copying a block from a sibling function, factor it instead.
- **Deslop as you go.** Code you touch ends up cleaner than you found it where
  slop is in reach: duplicated logic factored, unbounded writes bounded, dead
  state removed, twin branches merged. Remove unimplemented stubs and dead code
  before calling the change done. Do not deslop code you are not otherwise
  touching; that is churn, not cleanliness.
- **Simplicity and maintainability.** The simplest correct code; obvious beats
  clever; write what the project's maintainers would have written. Comments
  explain why (invariants, protocol quirks, on-air format constraints), not
  what. Choose solutions that stay workable so later features are easy to add.
- **Minimize flash and RAM.** Targets have tight flash budgets and limited RAM.
  Share code instead of duplicating it, reuse existing helpers, avoid heavy
  dependencies for small gains, bound buffers and queues, keep per-feature state
  small, and avoid large stack frames and heap churn. Stack size is a
  boot-breaking constraint the host suites cannot see; see *Verification*.
- **Guide durability.** This guide holds durable rules and pointers, not
  volatile state: no test counts, current coverage figures, version or date
  stamps, churn statistics, growing suite or target inventories, or planned
  work. Point to the file that owns the current value. Keep a numeric constant
  only when it encodes a frozen contract, and name its authoritative source.
- **Honesty about validation.** Do not claim hardware or on-air validation
  unless it was actually run. State what was bench-verified and what remains
  empirical.

## Start here: docs & code map

Everything needed to begin work is here or in the files named below; an agent
should not need to explore beyond them before making a change.

**User guides (authoritative for behavior; keep in sync with code):**

- `FILTER.md`: the packet filter's user manual: rule model, the predicate table,
  full command reference, common setups and limits; ends with a brief pointer to
  fleet management. First stop for any CLI-semantics or least-surprise question.
  Deep detail lives in two linked appendices: `FILTER_PATTERNS.md` (pattern
  syntax, quoting, quirks, troubleshooting) and `FILTER_CONFIG_FORMAT.md`
  (binary record layout and migration, for tooling authors).
- `FLEET.md`: fleet management user manual: the `fleet chan set` and
  `fleet tag add` setup, script format (the `!tags`/`!ack`/`!at` directive
  block, inline `!delay`), tag targeting (exact equality), acknowledgements and
  the reply-jitter window, the sync to verify to schedule recipe for `!at`,
  idempotency, two-phase cutovers, recipes, troubleshooting and limits. First
  stop for anything about the fleet feature.
- `BATTERY.md`: battery gate user manual: behavior, what suspension does not
  touch, persistence, CLI reference.
- `MESHCLOCK.md`: mesh clock sync behavior, CLI, defaults and caveats.
- `POWER.md`: power profile user manual: `powersaving` (upstream light sleep),
  `radio.rxps` (the receive duty cycle ported from EasySkyMesh, the level scale
  and its preamble guard, presets/raw/level forms), `cpu.freq`, persistence,
  the measured profile, attribution and what the profile deliberately does not
  do. First stop for any power question.
- `MQTT_OBSERVER.md`: optional MQTT observer firmware user manual: build
  targets, the `mqtt*`/`wifi*` CLI, broker setup (JWT vs password, TLS), the
  capture and delivery model, recipes, troubleshooting and limits. The
  status/packet/raw JSON reference lives in `MQTT_SCHEMA.md`.
- `RADIO2.md`: second radio preset user manual: shared transceiver model,
  preset validation (overlap and scannability), the policy matrix
  (`same`/`other`/`both`/`drop`, broadcast-flood-only crossing), reply-origin
  rules, the active/idle visit scheduler, the full CLI reference, feature
  interactions (RXPS, `extra.sf`, tempradio, filter, battery/fleet,
  powersaving) and tradeoffs.
- `HAM.md`: ham mode user manual: the Part 97 / Part 15 power split, the
  public/private classification table, the callsign gate and the 10-minute
  station-ID timer, the `ham` CLI, persistence in `/ham_cfg`, the deliberate
  decoupling from the filter, credit to NebraskaMesh/MeshCore-Ham, and the
  operator's responsibility for power levels and identification. First stop for
  any ham-mode question.
- `OTA.md`: short operator overview of LoRa OTA for repeaters (branch `lora-ota`).
  The full user manual, Android/Linux app and hardware bench live in
  `MeshCoreRepeaterManager`; `OtaUpdate.h` is the authoritative wire spec. The
  partition-change notice and target lists live here. The frozen contract is in
  *Compatibility & frozen contracts*.
- `ONLINE_OTA.md`: online OTA over WiFi for repeater observer builds: the
  `ota fetch ver`/`ota fetch sha`/`ota fetch start` commands, `ota net`
  progress, the frozen `ota status` line, digest syntax, errors and limits,
  fleet recipes. Observer-only: setting ver/sha touches no WiFi, and
  non-observer builds compile no adapter.
- `BOT.md`: opt-in chat bot on one keyed channel: reply triggers, CLI, rate
  limit, persistence and interactions.

**User guide style.** Guides are task-focused reference, not prose. Each starts
with `# <Feature>: User Guide`, then `## What it does` (one short paragraph),
`## Quick start`, `## Command reference`, `## Common setups` or `## Recipes`,
`## Interactions`, `## Limits`. Use "you" and active voice; no first person,
jokes, design justification, or "honest limit" asides. One term per concept:
"repeater", "stock firmware", "radio2", "behaviour". Exact CLI replies, error
strings, safety/legal notes, attribution and frozen contracts are not
paraphrased or condensed. Deep tooling detail goes in a linked appendix page,
not the main guide. No em dashes. Fork-wide parity is stated once in
`README.md`: every feature is opt-in, and off means stock behaviour.

**New top-level features need two docs in the same commit:** a paragraph in
`README.md` describing the feature, and a dedicated user-focused guide covering
setup, behavior, the CLI and limits. When behavior changes, the matching guide
section is updated in the same commit. The guides are user-facing API
documentation, not optional docs.

**Code (`examples/simple_repeater/`):**

- `PacketFilter.h/.cpp`: rule model, evaluation (`checkPacket` for packet-level
  rules, `checkContent` for decrypted-content in one pass, verdict stash),
  binary persistence (load/save, stepwise version migration),
  `filterCLI`.
- `FleetManager.h/.cpp`: one PSK-backed channel, the repeater's tag set,
  `fleetCLI`, the receive-side hook (`onGroupData`), the relay-side battery
  exemption (`checkForward`), jittered acknowledgements with one byte-identical
  retry until a repeat is heard, sent on the captured channel, unsolicited
  alerts on the configured channel, scheduled `!at` jobs (RAM-only), and
  `/fleet_cfg` persistence with a whole-payload CRC-16. Owns a `CliScriptRunner`.
- `CliScript.h/.cpp`: `!id` parse, the `!tags`/`!ack`/`!at` directive block,
  inline `!delay` with resumable run, the two-slot pending queue, the seen-key
  hash ring, and the run callback MyMesh drives.
- `AdvertRateLimiter.h/.cpp`: per-origin advert repeat window: cache, counters,
  check/commit split.
- `PersistUtil.h`: lazy dirty-save pattern, per-platform file opens, shared
  staged-save transaction, CRC-16 and config-integrity helpers.
- `PacketFilterConfig.h` and `FleetConfig.h`: capacity tunables (`FILTER_*`,
  `FLEET_*`, including the renamed `FLEET_CLI_*` runner capacities and the
  `!tags`/`!delay`/`!at` grammar limits). Override via build flags; layout
  caveats noted inline.
- `TinyRegex.h/.cpp`: vendored kokke/tiny-regex-c plus a step budget. Treat as
  upstream; see *Compatibility & frozen contracts*.
- `PatternMatch.h/.cpp`: the user-facing pattern language: top-level `|`
  alternation, structural validation (empty alternative, alternative cap), a
  thin wrapper over the vendored engine.
- `CliUtil.h`: shared CLI helpers (`nextToken`, `CliBuf`, `cliPrepare`,
  `cliCutReply`, `cliIsErrorReply`, `CLI_REPLY_MAX`) used by the filter,
  battery, fleet and clock CLIs: bounded reply writer, command entry point,
  marked cut, and the one place that knows what a failing reply looks like.
- `BatteryGate.h/.cpp`: low-battery forward suspension, `battery` CLI, RAM-only
  one-shot suspend/resume readout (`takeStateChange`).
- `MeshClockSync.h/.cpp`, `MeshClockSyncConfig.h`, `BuildEpoch.h`: advert sample
  collection, median/quorum decision, enabled-flag persistence and CLI.
- `Radio2.h/.cpp`, `Radio2Config.h`: second radio preset: config record and
  staged persistence (`/radio2_cfg`), preset/timing validation against the
  primary, policy matrix, active/idle visit scheduler, deferred apply,
  `radio2CLI`. `Radio2Device` is its only radio surface, so the native suite
  drives a fake; MyMesh supplies the adapter over `radio_driver`.
- `HamMode.h/.cpp`, `HamModeConfig.h`: per-packet public/private TX power
  selection, the 10-minute station-ID advert timer, the `ham` CLI and the
  `/ham_cfg` record (magic `HAMC`, CRC-16). Uses `CliUtil.h` and
  `PersistUtil.h`; the one upstream hook is `Dispatcher::onBeforeTransmit()`.
  Fully decoupled from the filter.
- `OtaUpdate.h/.cpp`: LoRa OTA engine: the `ota` CLI (`stage`, `status`, `apply`,
  `abort`, `rollback`), the `/ota_pend` boot-health marker and attempt counter,
  the boot-time verdict (`earlyBootCheck`) that rolls back a freshly applied
  image after bad boots, ESP32 partition glue (`Esp32SlotIo`), and the
  all-state `OP_INFO` handshake. Ops ride inside admin REQ/RESPONSE datagrams
  (`REQ_TYPE_OTA`, dispatched by `handleRequest`); the boot-health probe is the
  one zero-hop RAW_CUSTOM `OP_NOP`. Also exposes the local staging API
  (`beginLocalStage`/`appendLocalStage`/`finishLocalStage`/`cancelLocalStage`)
  used by the online download and the appended `0x40` feature bit. Frozen
  replies and report layouts are unchanged.
- `OnlineOtaPolicy.h`: pure, Arduino-free version normalization, SHA-256 token
  parsing and release URL construction. The release tag prefix macro is here.
- `OnlineOtaCore.h/.cpp`: RAM-only async download state machine (wifi, clock,
  asset, verify, ready, error) driven one bounded step per `loop()`. Three CLI
  steps (ver, sha, start) store inputs before any network work. No persistence.
- `OnlineOtaNet.h/.cpp`: narrow network interface and ESP32 observer adapter:
  station request, HTTPS GET with the framework CA bundle, bounded redirects,
  Content-Length via `contentLength()` and one reused small chunk. Compiled
  only under `ESP32 && WITH_MQTT_OBSERVER`.
- `MyMesh.cpp/.h`: upstream files carrying the fork's hook lines
  (`allowPacketForward`, `onGroupDataRecv`, `logRx` (profile stamp and ack
  repeat detector), `logRxRaw`/`logRx`/`logTx` observer capture,
  `searchChannelsByHash`, CLI dispatch, lazy-save loop, `execCliLine`
  script-execution trampoline, fleet device-service injections, the
  `batteryAlerts` transition drain, radio2 scheduler/profile-control hooks and
  `getSleepMs`).
- `main.cpp`: upstream's serial CLI entry plus the fork's radio2 sleep deadline
  (`getSleepMs()`/`sleepMs()`) on the powersaving path.

**Power (`src/helpers/` and the upstream radio layer):**

- `PowerProfile.h/.cpp`: fork power glue kept out of upstream files: persisted
  CPU cap (`cpu.freq` parsing/apply, `0`/default restore), repeater power
  defaults (RXPS off, balanced level carried), and the hook pushing stored RXPS
  config into the radio. `loadPrefs()`, `handleSetCmd`, `handleGetCmd` and
  `MyMesh` carry only the call lines.
- `radiolib/RXPowerSaving.h/.cpp`: duty-cycle policy: level ladders per preamble
  profile, datasheet timer guard, period calculation, config normalization and
  arm-retry state. Ported from EasySkyMesh (`IoTThinks/MeshCore`), the
  `PowerSaving-v18` branch, plus the SX1262 post-preamble-timer fix from
  `Dev-PS18` (PR by Dom4n) ported unchanged; see `SX1262RxPowerSaving.h`. The
  LR1110 arm path carries this fork's LF-clock fix (BUSY release must not wait
  on a 32 kHz Xtal that never starts on the T1000-E).
- `radiolib/SX1262RxPowerSaving.h`: SX1262-only effective RX-period computation
  at arm time (Dom4n's measured post-preamble timer model; `R` chosen so the
  timer covers a P32 sender's header).
- `radiolib/RXPowerSavingCLI.h/.cpp`: `get radio.rxps` / `set radio.rxps`
  (presets, `level N`, `preamble 16|32`, raw periods) and status report.
  `preamble=0` means the profile follows the SF (SF<=8 maps to P32).
- The radio integration lives in upstream files and is the fork's largest
  upstream diff: `radiolib/RadioLibWrappers.*` (duty-cycle arming, re-arm around
  TX/receive, noise-floor calibration), `CustomSX1262*` and
  `CustomLR1110*`/`CustomLR2021` (per-family arming and TCXO delay), and
  `CommonRadioPrefs.*` (CLI hook and apply). Read the merge playbook before
  resolving a conflict in them.

**Code (`examples/mqtt_observer/`, compiled only under `WITH_MQTT_OBSERVER`):**

- `MQTTObserver.h/.cpp`: ESP32-only observer: Wi-Fi/NTP, up to two broker slots,
  Ed25519 JWT or user/pass auth, receive/TX capture hooks, bounded RAM event
  queue, heap-headroom connect gate, `mqtt*`/`wifi*` CLI.
- `MQTTObserverConfig.h`: config model and codec (`/mqtt_prefs` payload v5;
  `MQTT_OBSERVER_MAX_BROKERS`, queue capacity).
- `MQTTObserverRecord.h`: record framing (magic `MQO1`, CRC-16 over the
  payload) and header, version and checksum checks.
- `MQTTObserverPolicy.h`: pure host-tested policies: command/slot parsing, JWT
  renewal, queue/retry rules, connect gates (heap admission, attempt outcome),
  liveness probe, raw frame stager.
- `MQTTObserverCli.h/.cpp`: command handling separated from the radio, Wi-Fi
  stack and MQTT client: the observer supplies device facts (Wi-Fi, clock,
  heap, queue, slot states) and the unit owns config and replies. Host-tested.
- `MQTTObserverFormat.h/.cpp`: pure host-tested JSON builders for status,
  packet and raw messages (the reference observer schema).

`observer_targets.ini` (repo root) defines the optional
`*_repeater_observer` PlatformIO environments. It is pulled in through
`platformio.ini`'s `extra_configs`, so those environments are part of the
resolved config. What marks an environment as an observer is the
`_repeater_observer` name suffix, not membership in that file.

**Tests:** behavior-level native googletest suites, one per feature, each
reached through the same entry points a user or the firmware would:
`test/test_packet_filter/` (matching, content rules, limiter, persistence
upgrades, CLI surface, TinyRegex, PatternMatch), `test/test_fleet/` (runner
grammar, tag targeting, acknowledgements, scheduling, persistence, CLI, battery
exemption contract), `test/test_battery_gate/` (suspension, persistence, CLI),
`test/test_ham_mode/` (classification, callsign gate, station-ID timing, CLI,
persistence), `test/test_mesh_clock/` (sample collection, quorum decision,
persistence, CLI), `test/test_rx_power_saving/` (duty-cycle ladders and preamble
guard, timer-condition validation, config normalization, presets/raw parsing,
CLI report), `test/test_radio2/` (profile metadata, outbound peek, policy
matrix, timing bounds, scheduler states, deferred apply, persistence, CLI) and
`test/test_mqtt_observer/` (config/record framing, URI/topic builders, CLI and
slot parsing, JWT renewal, connect gates, queue/retry, liveness probe, JSON
schema), `test/test_online_ota/` (version and digest parsing, URL construction,
every core phase and error with a fake network, CLI replies and local staging).
Test-only shims: `NativeShim.h`, `NativeTestStubs.cpp`, `RegionMapStub.cpp`,
`FilterTestHelpers.h`, `CliReplyHelper.h`.

## Current state & next milestone

The integration branch is `repeater-filter` (dev); `repeater-filter-stable` is
the stable channel. LoRa OTA is carried on `lora-ota` and is not merged into
`repeater-filter` yet; `OtaUpdate.h` is the authoritative spec and the app side
lives in `MeshCoreRepeaterManager`. Current build and release state lives in
`RELEASE_NOTES.md` and the workflows; do not duplicate it here.

Some repeater targets fail on the upstream tree too and are deliberately left
failing. They are upstream's to fix; do not patch them as part of a fork fix and
do not add them to the fork's release targets. The current target set lives in
the build script, not in this guide.

## Parity & intentional differences

### Default-off parity with upstream

Every fork feature is opt-in, and **off must mean off**. With every feature
disabled (the fresh-install default), the node must behave like the official
(upstream `meshcore-dev/MeshCore`) firmware for that role, within reason: same
on-air behaviour, same forwarding decisions, same RF activity, same sleep/wake
behaviour and the same default power state. Flashing this fork over an official
node must be undetectable until the operator enables a feature.

- **Do not override the board's compiled configuration.** "No cap" or "off"
  means the board's own value (for example `ESP32_CPU_FREQ`), not a convenient
  default such as 240 MHz.
- **Official persisted config loads unchanged.** New keys ride with defaults;
  an upgrade must never switch a feature on or change a clock behind the
  operator's back.
- **When off, take the upstream path.** Hot-path hooks are skipped or branch to
  the original code, not computed and discarded; no feature may install a
  radio/profile/timer/sleep hook while off.
- **"Within reason"** allows additive CLI/status output, new config keys and
  boot-time setup that does not change a running node's RF, forwarding or
  sleep/wake behaviour.
- **Verify the off path explicitly**: default value, RF command trace,
  sleep/wake path, forwarding decision and CLI surface. "It compiles" is not
  evidence.

The exception is an overlap between fork features that must work in tandem
(for example the fleet channel's battery-gate exemption, the filter's channel
store and the fleet PSK, radio2's RXPS mutual exclusion). Those interactions may
change behaviour for the participating features, are documented where they are
defined, and must not affect an otherwise-off node.

The fork-wide convention is a gate at the top of the feature's receive/loop-path
hook (or at the call site): a disabled feature may not compute a value nothing
downstream reads. RXPS and radio2 take the upstream path when off
(`needsService()`, no profile controller installed), `filter off` branches
every gate to upstream behaviour and `checkContent` skips an empty rule store,
the battery gate samples the ADC only while enabled, and the fleet and mesh
clock sync early-return before any crypto or RTC read (the advert hook is gated
at the call site). The boot path makes no clock-changing call when the requested
CPU frequency equals the running one.

### Known intentional differences

These are accepted residuals, recorded so nobody re-flags them:

- Fresh nodes pre-provision the built-in Public channel. The core MAC-verifies
  Public group traffic, and the documented `filter chan list` contract pins it
  in the CLI tests.
- The fleet's relay-side auth (the just-decrypted fleet packet's content hash)
  is not cleared after the receive bracket, so a later same-tag group packet
  pays one SHA-256 in the battery-gate exemption.
- Observer builds always create the worker task (1 Hz idle, Wi-Fi off with no
  broker configured).
- The fresh first boot writes `/filter_cfg` once (the Public provision).
- The OTA engine is compiled into every repeater build, where its loop-path hook
  is a flag test and its per-TX hook compares one payload length. A node with no
  pending apply and no traffic on the wire does no OTA work.

## Compatibility & frozen contracts

### Backwards compatibility

- **Persistence is paramount.** A config written by any firmware version still
  in the field must load with settings intact. A change is either directly
  backwards compatible (new fields ride tail padding; the old record is a
  byte-identical prefix of the new one, enforced by the `static_assert`s in
  `PacketFilter.h`) or it ships migration code in `load()` plus a version bump.
  Within reason: layouts older than the versions whose record code is kept in
  `load()` were already discarded by design and need no migration; the current
  floor lives in `PacketFilter.cpp`. Every version bump must keep older configs
  loading, with roundtrip and old-version fixtures in the persistence suite.
- **CLI surface is compatibility too.** Never break existing command syntax,
  reply formats or index semantics (0-based, shifting on delete).
- **Behavioral invariants are compatibility.** First-match-wins ordering,
  prob-before-throttle and stats semantics (hits travel with the rule on
  move/delete; a stats reset never grants a throttle free pass) are observable
  behaviour. So is the fleet model: a `!id` job key runs at most once per boot
  (marked at admission, RAM-only seen ring), targeting is by exact tag equality
  via `!tags` (absent means broadcast), the fleet channel is a single unnamed
  PSK that supersedes the battery gate (scripts run, fleet traffic relays, acks
  send while suspended), armed `!at` jobs and pending acks are RAM-only, battery
  alerts are emitted on the fleet channel only when it is enabled and
  configured, and script execution is deferred out of the receive path.
  Changing them is a breaking decision, not a refactor side effect.
- The observer's observable surface is compatibility too: capture happens
  before forwarding policy (a packet the node drops is still observed), the
  feed is best-effort (no flash replay, no delivery guarantee), TX uplinks this
  node's own adverts by default, the topic template and the status/packet/raw
  schemas follow the reference observer, and `/mqtt_prefs` (magic `MQO1`) is the
  fork's own format, deliberately not migrated from the other observers.
- **New pattern syntax must not silently change the meaning of an
  already-storable pattern.** If it would, either ship an escape (`\|`) or
  reject the ambiguous form with an error. Deliberate historical exception:
  `|` became OR through alternation; a stored pattern containing an unescaped
  `|` previously matched a literal pipe.
- **Config integrity is checked per record where the format allows it.** A
  truncated save is detected on load; a torn write of exactly the right length
  is caught only by a record checksum. Since the CRC bump, each rule record
  carries a CRC-16 over its payload (`crc16_ccitt` in `PersistUtil.h`); older
  records cannot, so they stay length-checked. A checksum rides a version bump,
  never a patch. Channel records and the battery config keep the length-only
  story until their own layout change adds one. No sidecar file: a second file
  is a second thing to go missing. This stops a later "improvement" from
  quietly weakening compatibility.
- **Old-version migration is temporary.** Older record loading is kept for now
  and is expected to be dropped once the field has migrated. The migration
  chain is one named step function per version pair in `PacketFilter.cpp`,
  kept contiguous; never skip a middle step in code. Removing a version later
  is one excision: delete its step function, raise the floor in
  `rule_bytes_for()`, delete its fixtures.

### Hard invariants

1. **Config persistence layout.** See *Backwards compatibility* above; the
   `static_assert`s in `PacketFilter.h` are the enforced form of that rule.
2. **Vendored TinyRegex.** Only the documented modifications (step budget,
   `re_budget_exhausted()`, unsigned-char literal compare and the `matchone()`
   operand fix). A bug fix is the only reason to touch vendored source, and
   only after asking first: not every bug is worth a vendored edit. Weigh a
   one-line engine fix against reimplementing the check in the wrapper, and
   propose the trade-off (diff size, hot-path cost, what else it would pin
   down) rather than deciding unilaterally. Features, syntax and tuning are
   never a reason.
3. **CLI reply bound: 157 bytes in a 160-byte buffer.** All writes bounded:
   one-liners via `snprintf(reply, CLI_REPLY_MAX, ...)`, staged output via
   `CliBuf`; never unbounded `sprintf` into `reply`. `CLI_REPLY_MAX` is 3 bytes
   under the upstream buffers (`main.cpp`'s serial/ethernet `char reply[160]`
   and the mesh remote-CLI's `&temp[5]` of `temp[166]`) because the core's
   companion-radio `xx|command` form puts that prefix in front of the reply the
   handler writes into the same buffer. Those buffers are upstream's; the fork
   keeps its replies short enough not to overrun them (`static_assert` in
   `CliUtil.h`). A fork-owned buffer the prefix is written into is
   `CLI_REPLY_BUFFER_MAX` (the fleet's script runner). Shared CLI helpers live
   in `CliUtil.h`; do not duplicate them.
4. **First match wins.** Rule evaluation order (listed order, packet-level in
   `checkPacket()`, whole-list single pass in `checkContent()`, stash
   consumption) is load-bearing; the prob roll runs before the throttle gate.
5. **Pattern syntax lives in the wrapper, not in the vendored engine.**
   `PatternMatch.*` owns every user-visible syntax decision (alternation
   splitting, structural validation, alternative cap). `TinyRegex.cpp` keeps
   only the documented modifications and `TinyRegex.h`'s only role is the
   engine API. A change that adds syntax by editing the engine violates this.
   Fixing an engine bug does not, but it must not grow into a second dialect in
   two places.

### LoRa OTA wire contract

`OtaUpdate.h` is the authoritative spec. The app side lives in
`MeshCoreRepeaterManager`; the two halves must stay in sync.

1. Ops ride inside ordinary encrypted REQ/RESPONSE datagrams (`REQ_TYPE_OTA`),
   never a new payload type.
2. The op stream is inert unless armed by an operator (`ota stage` plus token),
   and a staged image boots only after a SHA-256 gate.
3. `OP_INFO` is the capability handshake, answered in every state. Its report
   carries target id, slot size, max literal, feature bits, running firmware
   version and the running image's SHA-256, taken over exactly the bytes an
   update wrote (image length from the ESP image header, appended digest
   included), so it equals the hash of the app's own `.bin` and is the
   delta-base identity. All zeros means "cannot tell".
4. The wire contract is append-only and frozen: new features add new op codes
   or report bits, never change existing ones.
5. `REQ_TYPE_OTA` is fork-private `0xA0`, outside upstream's `0x01..0x09` REQ
   range (`0x08` is upstream's `subscribe`); the app's `reqTypeOta` constant
   changes with it.

### Frozen terminology

The OTA transport is the format, not a draft awaiting a successor. Do not
introduce "wire v2" or similar naming in docs or comments, and do not rename
the existing op stream. Nodes in the field predate any such release.

## Principle of least surprise

CLI and filter semantics must work the way a user would guess without reading
`FILTER.md`:

- `filter get <idx>` is an **inspection** command: its reply shows the rule's
  predicates plus counters, is truncated to the CLI reply bound (see hard
  invariant 3), and is *not* re-addable input (`add` rejects the `hits=`,
  `air=`, `pass=` metadata it prints).
- `filter chan list [<start-idx>]` exists because a full store does not fit one
  reply and `chan del` takes a name; a truncated listing ends with `next=N`.
- Errors say what is wrong and what is accepted, for example `Err - chanhash
  must be 2 hex chars`; usage lines enumerate the command surface.
- Syntax reads like the concept: `[a,b]`-style intervals, comma lists, `*` as
  wildcard, first match wins like firewall rules, `prob=` and `throttle=` gates
  behave exactly as `FILTER.md` describes them.
- Replies stay short and stable. Treat existing reply strings as user-facing
  API: humans parse them, and companion tools may too.

## Domain rules

### Branch model

- `repeater-filter-stable`: stable release channel; tracks upstream `main`,
  synced by the manually dispatched `sync-upstream.yml` (channel=stable).
- `repeater-filter`: dev release channel, based on `repeater-filter-stable`,
  the integration branch for all fork work. Default branch of the fork.
- Changes reach stable by fast-forward via `promote-stable.yml` (gate included),
  never by a merge commit on stable; dev absorbs stable via `sync-upstream.yml`
  (channel=dev) after every sync or promotion that moves
  `repeater-filter-stable`. Keep the invariant that stable is an ancestor of
  dev: every commit on stable is reachable from dev. Never commit directly to
  `repeater-filter-stable`; never merge dev into stable with a merge commit.
- Work branches: `repeater-filter-<topic>` cut from `repeater-filter`, merged
  back with `--no-ff` to keep feature commits traceable.
- Do not hand-merge upstream `main` locally; use `sync-upstream.yml` and resolve
  conflicts in the branch if the run fails.
- A conflict in a sync opens or updates an issue through
  `.github/actions/open-conflict-issue/` and fails the run. `sync-upstream.yml`
  and `promote-stable.yml` are manual-dispatch only, never on a schedule, and
  neither cuts a release; use Filter Release for that. If stable is not an
  ancestor of dev, promotion fails and tells you to sync channel=dev first.
- Upstream's own docs live in `docs/` (MeshCore protocol and CLI generally),
  not fork-filter documentation.

### Fork-owned vs upstream code

Fork-owned files (free to edit):

- `examples/simple_repeater/PacketFilter.h/.cpp`, `PacketFilterConfig.h`
- `examples/simple_repeater/TinyRegex.h/.cpp`, `CliUtil.h`, `BatteryGate.h/.cpp`
- `examples/simple_repeater/PatternMatch.h/.cpp`, `CliScript.h/.cpp`
- `examples/simple_repeater/AdvertRateLimiter.h/.cpp`, `PersistUtil.h`
- `examples/simple_repeater/MeshClockSync.h/.cpp`, `MeshClockSyncConfig.h`,
  `BuildEpoch.h`
- `examples/simple_repeater/Radio2.h/.cpp`, `Radio2Config.h`
- `examples/simple_repeater/OnlineOtaPolicy.h`, `OnlineOtaCore.h/.cpp`,
  `OnlineOtaNet.h/.cpp`
- `src/helpers/PowerProfile.h/.cpp`,
  `src/helpers/radiolib/RXPowerSaving.h/.cpp`, `RXPowerSavingCLI.h/.cpp`,
  `src/helpers/radiolib/SX1262RxPowerSaving.h`,
  `src/helpers/radiolib/RadioProfileControl.h`
- `test/test_packet_filter/`, `test/test_fleet/`, `test/test_battery_gate/`,
  `test/test_mesh_clock/`, `test/test_rx_power_saving/`, `test/test_radio2/`,
  `test/test_mqtt_observer/`, `test/test_online_ota/`
- `examples/mqtt_observer/`, `observer_targets.ini`
- `FILTER.md`, `FLEET.md`, `BATTERY.md`, `MESHCLOCK.md`, `POWER.md`,
  `RADIO2.md`, `MQTT_OBSERVER.md`, `ONLINE_OTA.md`
- `scripts/local-release-build.sh`, `scripts/rename-ota-assets.py`,
  `scripts/tests/`
- `.github/workflows/filter-release.yml`, `sync-upstream.yml`,
  `.github/workflows/promote-stable.yml`
- `.github/actions/open-conflict-issue/` (`setup-build-environment/` next to
  it is upstream's)

Fork lines inside upstream files stay **minimal**: a reviewer should see a
handful of lines, not a fork interleaved into upstream code. Those files are
`examples/simple_repeater/MyMesh.h/.cpp` (the hooks the features call),
`build.sh` (observer commands and `build_observer_firmwares`), `platformio.ini`
(the fork's native and sanitizer environments and the `observer_targets.ini`
include), small patches in `examples/companion_radio/` and
`test/mocks/SHA256.h`, plus `README.md`, which is the fork's own. Everything
else is upstream; apply the change discipline even more strictly there and
match the existing style (`.clang-format`: 2-space indent, `camelCase`
functions, `UpperCamelCase` classes, lines under about 100 chars).

### Merge playbook: the RXPS layer

The power feature is the fork's largest edit inside upstream files, so it is
where an upstream sync is most likely to conflict. Resolutions are mechanical:

| File | Fork surface | On conflict |
|---|---|---|
| `RadioLibWrappers.h` | `RxPowerSavingControl` base, `_rx_ps_*`/`_nf_*` members, declarations, ctor init | keep upstream's shape, re-add the fork block |
| `RadioLibWrappers.cpp` | `loop()` and `startRecv()` replaced; hooks in `begin`/`idle`/`powerOff`/`resetAGC`/`recvRaw`/`startSendRaw`/`onSendFinished`; additive functions in one fork section at the end | take upstream's function, re-apply the fork's 1 to 6 lines; the end section is additive |
| `CustomSX1262.h`, `CustomSX1262Wrapper.h`, `CustomLR1110*`, `CustomLR2021.h` | `begin()` and TCXO helper, per-family arm/stop, wrapper overrides | keep both; upstream has not touched these in months |
| `CommonRadioPrefs.h/.cpp` | include, `_active_sf`/`_active_bw`, virtual + apply, two CLI branches | keep both sides |
| `CommonCLI.h` | `cpu_freq_mhz`, `rxps`, `def(...)` keys | add the keys to upstream's structure |
| `MyMesh.cpp` | `powerApplyDefaults`, `powerApplyRadioProfile` call lines | one line each |

1. Take upstream's version of a conflicting *function* first, then re-apply the
   fork's lines. Never hand-merge the two bodies.
2. A conflict in the additive functions (`startReceiveMode`,
   `noiseFloorCalibCheck`, ...) means upstream added a function at the same
   spot: keep both.
3. After any conflict in the radio layer the check is behavioural, not just a
   build: flash, confirm `get radio.rxps` reports `effective=armed`, then send a
   direct CLI command and see it ACKed. A wrong delegation makes the node deaf
   while it still transmits.
4. `test/test_rx_power_saving` covers the policy, not the integration: only the
   on-air check catches a broken delegation.

## Verification

- PlatformIO's project checksum is derived from global build flags, so
  `build.sh` (release `-D` flags) and plain `pio test` produce different
  signatures, and each flip wipes `.pio/build` before rebuilding. Alternate
  between them as little as possible. A firmware build that dies with a
  `.sconsign314.dblite` error right after a native run was almost certainly a
  checksum flip mid-run; rerun it alone.
- Every behavior change ships with tests covering its new or changed code paths,
  in the same commit. The suites are behavior-level (native googletest) and
  reach the same CLI/`checkPacket`/`checkContent` entry points a user or the
  firmware would. Every bug fix gets a test that reproduces it. Pure refactors
  keep the suites green **unchanged**: the suites are the safety net that proves
  no behavior slipped.
- The fleet's no-ACK variant compiles its test bodies in `#else` branches no
  other environment sees, so it cannot be inferred from the main fleet suite.
- **Stack is a boot-breaking constraint the suites cannot see.** `setup()` and
  `loop()` run in a hard-capped small FreeRTOS task on nRF52 and a small
  `loopTask` on ESP32; a large scratch array in boot- or loop-path code
  crash-loops every repeater flashed with the release. Measure frames with
  `PLATFORMIO_BUILD_FLAGS=-fstack-usage pio run -e <env>` before committing
  large local arrays. The stack gate in `scripts/local-release-build.sh` fails
  any release target whose `src/`/`examples/` code holds a frame at or above
  its threshold; keep new frames well under it.
- Run the feature suites, then the full host-env run, then re-read the diff and
  confirm every touched line is required. The exact commands and environment
  names live in `build.sh`; do not duplicate them here.
- Touching firmware-hook or persistence code additionally warrants compile
  checks of every filesystem family before calling the work done. The current
  target set lives in `build.sh`; host-native tests cannot catch a
  platform-specific API difference, and one did (the nRF52 file type has no
  default constructor). The smallest STM32 env that still links stands in for
  the STM32WLE5 repeaters, which no longer fit their flash budget (same
  `InternalFS` path).
- PlatformIO does **not** track the force-included `test/test_packet_filter/NativeShim.h`
  as a dependency: after changing a struct it defines, clean the native envs
  (`pio run -e native_packet_filter -t clean`) or stale objects will silently
  disagree with the new layout.
- Firmware is built locally with `bash build.sh build-firmware <target>`
  (`bash build.sh list`; `build.sh` uses bash arrays, so `sh` is not a
  contract). CI cuts tag and release object only; the local release script
  builds and uploads assets with `gh`. It prints the tail of a failed target's
  log, prunes a failed target's build tree and retries the failed set once with
  two workers after the main pass, so runner-resource flakes do not leave
  assets missing; a deterministic failure simply fails again.
- `RELEASE_NOTES.md` holds pending release notes and `filter-release.yml` uses
  it verbatim as the body of every dev release. A stable release collects the
  bodies of the dev releases since the previous stable one, so it needs no
  separate notes file; `RELEASE_NOTES_STABLE.md` on the stable branch overrides
  that collection when curated stable notes are wanted. Update the pending file
  as changes land, since the gate cuts a release only when code, not docs,
  changed. Release tags use the `v` prefix; branch names and the workflow file
  name keep `filter` for now. `OnlineOtaPolicy.h` holds the single tag-prefix
  macro. `scripts/rename-ota-assets.py` and `scripts/rename-ota-tags.py` renamed
  existing assets and releases in place through the GitHub API, keeping old tag
  refs as aliases.

## Change & git discipline

- Commit style: conventional prefixes as used in the fork history: `feat:`,
  `fix:`, `ci:`, `docs:`, `refactor:` (no behavior change). Descriptive, one
  concern per commit.
- Work stays on the **fork**. Opening a PR against `meshcore-dev/MeshCore`
  requires explicit human approval of the diff first.
- Upstream `CONTRIBUTING.md`: an agent-opened PR with `🤖🤖` in the title opts
  into fast-tracked merging.
