# radio2 on-air tests

Behavior-level tests for the repeater's second radio preset, run against real
hardware. They are the on-air complement to `test/test_radio2` (native).

## Bench

| Role | Endpoint / identity |
|---|---|
| companion `nc3` (secondary network, moves radio) | `192.168.1.165:5000` |
| companion `nc4` (primary network, also the CLI bridge) | `192.168.1.154:5000` |
| repeater | prefix `3d3bc06e`, admin password `password` (env override) |
| contact-list source (read-only) | `192.168.1.55:9508` |
| live primary preset | `910.525 MHz, BW 62.5, SF7, CR 4/8` |

All group traffic uses the private `r2lab` channel (a pinned PSK), never
Public, `#test` or any other shared channel. The companions may be moved to the
secondary preset; the script puts them back on the live preset when it finishes.

> **Airtime warning.** The `primary`, `secondary` and `active` sections send
> encrypted private-channel flood traffic on the live primary. Other repeaters
> relay flood traffic they cannot decrypt, so these sections still cost live
> airtime. Use the `inverted` section (quiet primary) for bulk runs, or point the
> bench at a private frequency before running the others.

Override the defaults with environment variables:

```
RADIO2_REPEATER_PREFIX=3d3bc06e
RADIO2_REPEATER_PW=password
RADIO2_LAB_PSK=<32 hex chars>
MESHCORE_PY_SRC=/path/to/meshcore_py/src
```

## Run

Use the PlatformIO Python environment (it has the meshcore client deps):

```bash
cd MeshCore
~/.platformio/penv/bin/python radio2_onair_tests/test_radio2_onair.py --section cli
~/.platformio/penv/bin/python radio2_onair_tests/test_radio2_onair.py --section primary
~/.platformio/penv/bin/python radio2_onair_tests/test_radio2_onair.py --section all
```

`all` includes the destructive sections:

- `persistence` reboots the repeater and checks the saved config survives.
- `inverted` moves the repeater's primary to a quiet preset and its radio2
  preset to the live mesh, so the idle scheduler runs deterministically, then
  restores the live primary. This is the only way to exercise secondary-origin
  reception deterministically on a busy primary.

Sections `cli`, `primary`, `secondary`, `filter` and `observer` leave the
repeater's primary preset untouched.

The repeater must be reachable over the mesh from `nc4`; the script logs in
with the admin password and sends every command over the companion's remote CLI.
`start ota` (observer section) brings the repeater's Wi-Fi station up so the
update page answers on the LAN; a reboot returns it to its idle Wi-Fi-off state.
