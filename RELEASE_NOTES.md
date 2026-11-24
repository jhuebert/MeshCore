- **Online OTA for repeater observer builds.** `ota fetch ver <version>`,
  `ota fetch sha <digest>` and `ota fetch start` download a release over HTTPS,
  stage it with the existing SHA-256 gate, and `ota apply` reboots into it with
  the usual boot health check and rollback. No manifest, GitHub API call or
  companion app change is needed. See [ONLINE_OTA.md](./ONLINE_OTA.md).
- The observer only brings up WiFi at `ota fetch start`. Setting the version or
  digest touches no network, the observer's clock and heap gate are reused, the
  station is released when the job ends, and a suspended battery gate refuses or
  aborts a download. A 15-second no-progress timeout stops a stalled server from
  holding the node awake.
- `ota abort` now cancels an in-flight online fetch as well as the LoRa session,
  clears the stored version and digest, and releases the station. A failed fetch
  is terminal for powersaving and reports a real error code, not `err=0`.
- **Release naming changed.** Tags are now `v<version>` instead of
  `filter-v<version>`, release titles say `Firmware <version>`, release app
  assets are `<env>-v<version>.bin`, and the firmware version string is the
  same sha-free `v<version>`. Existing tags, titles and assets were renamed in
  place, so older asset URLs no longer work. Old tag refs remain as aliases and
  branch names are unchanged.
- `OP_INFO` reports one appended feature bit (`0x40`) for online fetch. No OTA
  op codes, report layouts or frozen CLI replies changed.
- The release build and local `build.sh` now produce matching sha-free version
  strings and asset names. `scripts/rename-ota-tags.py`,
  `scripts/rename-ota-assets.py` and `scripts/rename-ota-release-titles.py`
  performed the one-time migration with dry-run reports.
- **Docs:** new `ONLINE_OTA.md`, a README paragraph that describes the fork as
  more than a packet filter, fleet recipes and updated user guides.
