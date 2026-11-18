# Releasing Firmware

GitHub Actions is set up to automatically build and release firmware.

It will automatically build firmware when one of the following tag formats are pushed.

- `companion-v1.0.0`
- `repeater-v1.0.0`
- `room-server-v1.0.0`

> NOTE: replace `v1.0.0` with the version you want to release as.

- You can push one, or more tags on the same commit, and they will all build separately.
- Once the firmware has been built, a new (draft) GitHub Release will be created.
- You will need to update the release notes, and publish it.

When a release contains the ESP32 repeater partition change, paste the
"Action required" block from
[OTA.md](./OTA.md#action-required-esp32-repeater-partition-change) at the top of
the notes: the affected targets, the `get prv.key` backup step, and the
full-erase + merged-flash requirement.
