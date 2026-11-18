# LoRa OTA — User Guide

Over-the-air updates are driven by the
**[MeshCoreOTA](https://github.com/jhuebert/MeshCoreOTA)** app. The firmware
side only receives, verifies and applies an update — there is no need to
assemble OTA operations by hand. The app's own
[user guide](https://github.com/jhuebert/MeshCoreOTA#readme) covers the steps
in detail; this page is a short overview for repeater operators.

---

## What you need

- A repeater running an OTA-enabled build of this fork on an ESP32 board with a
  dual app partition. Other boards report the feature as unsupported.
- A stock MeshCore companion the app can reach — Bluetooth on Android, macOS
  or Linux, or over the network (TCP) on any platform. The companion does not
  need custom firmware.
- Admin access to the repeater. A blank password is enough when the companion
  is already approved as an administrator.
- The app: **[MeshCoreOTA releases](https://github.com/jhuebert/MeshCoreOTA/releases)**.

## Updating a repeater

1. Connect the app to the companion, pick the repeater and sign in.
2. Choose a firmware version. The app picks the image for the repeater's board
   and downloads it; you can also import your own `.bin`.
3. Choose where the transfer runs — the live mesh, a temporary private preset,
   or the repeater's second preset. The app restores the radios afterwards.
4. Review the plan, start the transfer, then confirm the update once the node
   reports that the image is staged and verified.

The app sends only what changed when it can match the running image, and it
confirms success by reading back the new image's hash after the reboot. A lost
reply is treated as unknown, not as success.

## If something goes wrong

Nothing boots until you confirm. If the new image cannot start its radio on
first boot, the repeater flips back to the previous firmware by itself. These
CLI commands are available for diagnostics and recovery:

```text
ota status      # session state and progress
ota abort       # discard a staged session
ota rollback    # flip back manually
```

The app remains the normal way to run an update.

## For implementers

The node-side contract is in
[`examples/simple_repeater/OtaUpdate.h`](./examples/simple_repeater/OtaUpdate.h).
It is documented for maintaining clients, not as a procedure to follow by
hand.
