# Online OTA: User Guide

Online OTA lets a repeater observer download a release over HTTPS and install it, without a phone or a LoRa transfer. Three setup commands arm the job:

```
ota fetch ver 1.18.0-dev.11
ota fetch sha sha256:39483a72f358ea976125928a45394115d452ec29b4dbe4bf35c9e2ddf3f2d826
ota fetch start
ota status                   # watch progress, ends at state=ready
ota apply                    # flip the A/B slot and reboot
```

The first two commands only store values in RAM and do not touch WiFi. The third starts a background download. `ota status` and `ota apply` are separate steps: wait for `state=ready` before applying. All commands work from the serial CLI, an authenticated remote admin CLI message, and a fleet script. Only repeater observer builds support online OTA; plain repeaters, Pico W, nRF52, STM32 and companion firmware reply `Err - unsupported`.

## What it does

Release app images are named `<env>-v<version>.bin`, where `<env>` is the board's PlatformIO environment name. Firmware knows its own `OTA_PIO_ENV`, so it builds the download URL from the version token alone:

```
https://github.com/jhuebert/MeshCore/releases/download/v<version>/<own-env>-v<version>.bin
```

There is no manifest, no GitHub API call and no JSON in firmware. The node streams the download into the inactive app slot through the same staging session a LoRa transfer uses, then verifies the bytes it actually wrote against the digest. Only a verified image can be applied. `ota apply`, the boot health probe and the automatic rollback are unchanged. Every step is bounded so the radio keeps running. A download is refused while the battery gate is suspended and is aborted if suspension starts mid-download.

## Quick start

1. Flash a `*_repeater_observer` target. See [MQTT_OBSERVER.md](./MQTT_OBSERVER.md) for the observer build.
2. Set the station credentials (shared with the MQTT client):

   ```
   set wifi.ssid <your-ssid>
   set wifi.pwd <your-password>
   ```

3. Check the link and the clock:

   ```
   get wifi.status
   get mqtt.ntp
   ```

   The observer configures NTP whenever the station is up. A download starts only once the clock is past 2025-01-01, because TLS needs a valid time.

## Command reference

| Command | Reply | Notes |
|---|---|---|
| `ota fetch ver <version>` | `OK - online version set ver=<version>` | Stores the version. No network work. |
| `ota fetch sha [sha256:]<hex>` | `OK - online sha set sha=<16hex>` | Stores the expected digest. No network work. |
| `ota fetch start` | `OK - online fetch armed ver=<version>` | Starts the background job. Requires both values. |
| `ota net` | `net=<phase> ver=<version> sha=<8hex> got=<n> size=<n> err=<n>` | Detailed progress: `idle`, `wifi`, `clock`, `asset`, `verify`, `ready`, `error`. |
| `ota status` | unchanged | The frozen line. Progress shows as `state=staging size=<n> off=<n>`; failures as `state=error err=<n>`. |
| `ota apply` | unchanged | Works for a verified image from either source. |
| `ota abort` | `OK - aborted` | Clears the stored version and digest, cancels a running job and releases the station. |
| `ota rollback`, `ota stage` | unchanged | Existing LoRa OTA surface. |

The two setup commands can be given in either order and can be repeated before `start`. A second `start` while a job is running, or while an image is staged, is refused. The `start` reply only arms the job: poll `ota status` until `state=ready` before applying. A verified image is never silently discarded: use `ota abort`. After an error the stored values remain, so you can fix one and start again. Nothing is persisted: a reboot drops the values and the job.

### Version syntax

The version token selects the release tag `v<version>`. One leading `v` or `V` and a trailing `-filter-<sha>` are stripped, so a version copied out of `ota status` works:

```
ota fetch ver 1.18.0-dev.11
ota fetch ver v1.18.0-dev.11
ota fetch ver v1.18.0-dev.11-filter-10d4da2
ota fetch ver 1.18.0-r4
```

Only `[A-Za-z0-9._-]` is accepted, up to 63 bytes. There is no `latest` keyword, no downgrade check and no version comparison. URLs are never accepted.

### Digest syntax

The digest is 64 hex characters, with or without a `sha256:` prefix. Uppercase hex is accepted. It must be the SHA-256 of the exact `.bin` file for this board. The app already has this value from the GitHub API; the companion app should send it in the `ota fetch sha` command.

## Errors

Failures appear in both `ota net` (`err=<n>`) and the frozen `ota status` line (`state=error err=<n>`). Fix the cause and issue `start` again.

| Code | Name | Meaning |
|---|---|---|
| 10 | CFG | Online OTA unavailable on this build |
| 11 | WIFI | Station could not be associated in time |
| 12 | CLOCK | No valid time for TLS |
| 13 | TLS | Secure connection could not be set up |
| 14 | HTTP | HTTP error, bad Content-Length or too many redirects |
| 15 | DOWNLOAD | Short body or image write failed |
| 16 | HEAP | Not enough contiguous heap for a secure connection |

A suspended battery gate uses the existing code 2, and a SHA-256 mismatch uses the existing code 4.

## Recipes

A script line is one CLI command, so the three setup lines run in one pass and `start` returns as soon as the job is armed. Target observer nodes with `!tags`.

```
!id 2026-11-24-online-fetch
!tags canary
!ack err
ota fetch ver 1.18.0-dev.11
ota fetch sha sha256:39483a72f358ea976125928a45394115d452ec29b4dbe4bf35c9e2ddf3f2d826
ota fetch start
```

Then, once `ota status` shows `ready`, apply with a second job that `!at` can schedule:

```
!id 2026-11-24-online-apply
!tags canary
ota apply
```

Each node downloads independently over its own WiFi. Image bytes never travel over LoRa and `!ack` carries only short command replies. The fleet PSK remains full admin access. See [FLEET.md](./FLEET.md).

## Compatibility

**Note:** No new OTA op codes, report fields or frozen CLI replies were added. `OP_INFO` reports one appended feature bit, `0x40`, so the app can detect support. A free form remote CLI screen is not in the app yet, so that flow still needs an app change.

## Limits

- Release assets must be named `<env>-v<version>.bin`. New releases do this automatically; existing assets were renamed in place, without re-uploading.
- Merged images, `.uf2`, `.zip` and `.hex` are never selected. The URL is built from validated tokens, so a wrong board or a merged image cannot be named.
- One job at a time, in RAM only. A verified image needs `ota abort` before a second fetch.
- A chunked response is refused rather than mis-staged; release assets carry a Content-Length. The connect and header waits are bounded by short socket timeouts; the body is read incrementally.
