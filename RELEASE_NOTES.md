- **Release naming changed.** Tags are now `v<version>` instead of
  `filter-v<version>`, release titles say `Firmware <version>`, and the
  firmware version string is the same sha-free `v<version>`. Existing release
  tags and asset names were renamed in place, so asset URLs from older
  announcements no longer work. Old tag refs remain as aliases, and branch
  names are unchanged.
- **Online OTA for repeater observer builds.** `ota fetch ver <version>`,
  `ota fetch sha <digest>` and `ota fetch start` download a release over HTTPS,
  stage it with the existing SHA-256 gate, and `ota apply` reboots into it with
  the usual boot health check and rollback. No manifest, GitHub API call or
  app change is needed. See [ONLINE_OTA.md](./ONLINE_OTA.md).
- **Release app assets are now named `<env>-v<version>.bin`**, without the
  short commit sha. Existing assets were renamed in place, so their bytes and
  digests are unchanged. Old asset URLs stop working after the rename. The
  firmware's own version string still carries the sha.
- `OP_INFO` reports one appended feature bit (`0x40`) for online fetch. No OTA
  op codes, report layouts or frozen CLI replies changed.
