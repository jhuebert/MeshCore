# Agent Guide — jhuebert/MeshCore (personal fork)

This is my personal fork of `meshcore-dev/MeshCore`. The fork-specific feature
is the **repeater packet filter** (`filter ...` CLI, drop/forward rules, advert
rate limiter) carried on the `repeater-filter*` branches, plus firmware release
channels built by `.github/workflows/filter-build.yml`.

Remotes: `origin` = upstream `meshcore-dev/MeshCore` (never push there),
`fork` = `jhuebert/MeshCore` (push here). Upstream has **no** AGENTS.md, so this
file is fork-owned and does not conflict with upstream syncs.

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
- `test/test_packet_filter/`, `test/test_battery_gate/`
- `FILTER.md`, `.github/workflows/filter-build.yml`, `sync-upstream.yml`

Hook lines in upstream files (`MyMesh.h/.cpp`, `main.cpp`) must stay **minimal**
— a reviewer should see a handful of lines, not a fork interleaved into
upstream code. Everything else is upstream: **minimum viable change**, match the
existing style (`.clang-format`: 2-space indent, `camelCase` functions,
`UpperCamelCase` classes, lines < ~100 chars), never reformat or refactor
untouched code, and never upgrade dependencies while fixing something else.

## Hard invariants (do not break)

1. **Config persistence layout.** `FilterRule` records must keep the v4/v5
   record a byte-identical prefix of the current one (guarded by
   `static_assert` in `PacketFilter.h`). Growing the record requires a
   `FILTER_CFG_VERSION` bump plus an upgrade branch in `load()`.
2. **Vendored TinyRegex.** Only the documented modifications (step budget,
   `re_budget_exhausted()`, unsigned-char literal compare) — no other edits, or
   the vendoring diff against kokke/tiny-regex-c becomes unmaintainable.
3. **CLI reply buffer is 160 bytes.** All writes bounded: one-liners via
   `snprintf(reply, CLI_REPLY_MAX, ...)`, staged output via `radd()`; never
   unbounded `sprintf` into `reply`. Shared CLI helpers (`nextToken`, `radd`)
   live in `CliUtil.h` — don't duplicate them.
4. **First match wins.** Rule evaluation order (listed order, packet-level in
   `checkPacket()`, whole-list single pass in `checkContent()`, stash
   consumption) is load-bearing; the prob roll runs before the throttle gate.

## Verification before handing off

```
pio test -e native_packet_filter    # filter + TinyRegex + CLI + persistence
pio test -e native_battery_gate     # battery gate
```

All tests green, and `git diff` re-read: every touched line required, nothing
else changed. Touching firmware-hook or persistence code additionally warrants
a compile check of the real target (e.g. `pio run -e xiao_s3_wio`).

## Commit / PR discipline

- Commit style: conventional prefixes as used in the fork history — `feat:`,
  `fix:`, `ci:`, `docs:`, `refactor:` (no behavior change). Descriptive, one
  concern per commit.
- Work stays on the **fork**. Opening a PR against `meshcore-dev/MeshCore`
  requires explicit human approval of the diff first.
- Upstream CONTRIBUTING.md: an agent-opened PR with `🤖🤖` in the title opts
  into fast-tracked merging.
