# Agent Guide — jhuebert/MeshCore (personal fork)

This is my personal fork of `meshcore-dev/MeshCore`. The fork-specific feature
is the **repeater packet filter** (`filter ...` CLI, drop/forward rules, advert
rate limiter) carried on the `repeater-filter*` branches, plus firmware release
channels built by `.github/workflows/filter-build.yml`.

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
  notes, troubleshooting. First stop for any CLI-semantics or
  least-surprise question.
- `BATTERY.md` — battery gate user manual: behavior, what suspension does
  not touch, persistence, CLI reference.

**When behavior changes, the matching guide section is updated in the same
commit** — the guides are user-facing API documentation, not optional docs.

**Code (`examples/simple_repeater/`):**
- `PacketFilter.h/.cpp` — rule model, evaluation (`checkPacket` packet-level,
  `checkContent` decrypted-content single pass, verdict stash), binary
  persistence (`load`/`save`, v3..v7 stepwise migration), `filterCLI`.
- `AdvertRateLimiter.h/.cpp` — the per-origin advert repeat window: cache,
  counters, and the check/commit split (see the fork's rule-model note).
- `PersistUtil.h` — the lazy dirty-save pattern, the per-platform file opens, and
  the shared staged-save transaction; also the config-integrity note.
- `PacketFilterConfig.h` — all capacity tunables (`FILTER_MAX_RULES`, ...),
  override via build flags; persistence-layout caveats noted inline.
- `TinyRegex.h/.cpp` — vendored kokke/tiny-regex-c + step budget (see Hard
  invariants; treat as upstream).
- `PatternMatch.h/.cpp` — the user-facing pattern language: top-level `|`
  alternation, structural validation (empty alternative, alternative cap); thin
  wrapper over the vendored engine.
- `CliUtil.h` — shared CLI helpers (`nextToken`, `radd`, `CLI_REPLY_MAX`)
  used by filter and battery CLIs alike.
- `BatteryGate.h/.cpp` — low-battery forward suspension, `battery` CLI.
- `MyMesh.cpp/.h` — upstream files carrying the fork's hook lines
  (`allowPacketForward`, `onGroupDataRecv`, `searchChannelsByHash`, CLI
  dispatch, lazy-save loop).
- `main.cpp` — serial CLI entry (upstream + fork's buffer-hardening lines).

**Tests:** `test/test_packet_filter/` (295 behavior-level cases: matching,
content rules, limiter, persistence upgrades, CLI surface, TinyRegex,
PatternMatch) and `test/test_battery_gate/`. Test-only shims: `NativeShim.h`,
`NativeTestStubs.cpp`, `RegionMapStub.cpp`, `FilterTestHelpers.h`.

**Build & CI:** `pio test -e native_packet_filter` / `-e
native_battery_gate` (host-native, no hardware), plus `-e
native_packet_filter_san` / `-e native_battery_gate_san` for ASan+UBSan;
firmware via `bash build.sh build-firmware <target>` (`bash build.sh list` —
`build.sh` uses bash arrays, so `sh` is not a contract), the filter fork's flash
target being `Xiao_S3_WIO_repeater`. **Compile-check every filesystem
family** before calling persistence work done: `Xiao_S3_WIO_repeater`,
`RAK_4631_repeater` (nRF52), `PicoW_repeater`, `wio-e5_repeater` — host-native
tests cannot catch a platform-specific API difference, and one did (the nRF52
file type has no default constructor).
`.github/workflows/filter-build.yml` builds release channels (dev →
`repeater-filter`, stable → `repeater-filter-stable`); `sync-upstream.yml` merges
upstream weekly and promotes stable only on explicit dispatch. Upstream's own
docs live in `docs/` (MeshCore protocol/CLI generally) — not fork-filter
documentation.

## Branch model

- `repeater-filter` — dev release channel; the integration branch for all
  filter work. Default branch of the fork.
- `repeater-filter-stable` — stable release channel; updated from
  `repeater-filter` deliberately, not automatically.
- Work branches: `repeater-filter-<topic>` cut from `repeater-filter`, merged
  back with `--no-ff` (keep feature commits traceable).
- Upstream is tracked via the weekly `sync-upstream.yml` workflow; do not
  hand-merge upstream `dev` locally unless fixing a sync failure.

## Fork-owned vs upstream code

Fork-owned file set (free to edit):
- `examples/simple_repeater/PacketFilter.h/.cpp`, `PacketFilterConfig.h`
- `examples/simple_repeater/TinyRegex.h/.cpp`, `CliUtil.h`, `BatteryGate.h/.cpp`
- `examples/simple_repeater/PatternMatch.h/.cpp`
- `examples/simple_repeater/AdvertRateLimiter.h/.cpp`, `PersistUtil.h`
- `test/test_packet_filter/`, `test/test_battery_gate/`
- `FILTER.md`, `.github/workflows/filter-build.yml`, `sync-upstream.yml`

Hook lines in upstream files (`MyMesh.h/.cpp`, `main.cpp`) must stay **minimal**
— a reviewer should see a handful of lines, not a fork interleaved into
upstream code. Everything else is upstream: apply the change discipline below
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
  predicates plus counters, is truncated to the 160-byte transport, and is *not*
  re-addable input (`add` rejects the `hits=`/`air=`/`pass=` metadata it prints).
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
  behavior. Changing them is a breaking decision, not a refactor side effect.
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
3. **CLI reply buffer is 160 bytes.** All writes bounded: one-liners via
   `snprintf(reply, CLI_REPLY_MAX, ...)`, staged output via `radd()`; never
   unbounded `sprintf` into `reply`. Shared CLI helpers (`nextToken`, `radd`)
   live in `CliUtil.h` — don't duplicate them.
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
- Pure refactors keep both suites green **unchanged** — the suites (295 filter,
  44 battery) are the safety net that proves no behavior slipped.
- Run both suites, then re-read the diff:

```
pio test -e native_packet_filter    # filter + PatternMatch + TinyRegex + CLI + persistence
pio test -e native_battery_gate     # battery gate
```

All green, and every touched line required — together with the full four-env
run, since the generic envs are where a broken selection shows up:

```
pio test -e native -e native_kiss_modem -e native_packet_filter -e native_battery_gate
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
