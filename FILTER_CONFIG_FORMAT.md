# Filter Config Format: Reference

Tooling-only reference for firmware forks and tools that read or write the filter config. Normal operators only need [FILTER.md](./FILTER.md). The rule and channel config is one binary file, `/filter_cfg`, written lazily through a staged save (scratch, readback, backup, promote).

## Version 7 layout

The header's first byte is the version. Version 7 stores:

- 5 header bytes: version, enabled, rule count, channel count, spare.
- A 2-byte advert ratelimit window.
- One fixed-size record per rule, then one fixed-size record per channel.

## Rule records

- Each rule record is **190 bytes**: 188 bytes of fields, then a 2-byte **CRC-16/CCITT-FALSE** (poly 0x1021, init 0xFFFF, little-endian) over the 188 payload bytes.
- Byte 187 of the payload is reserved padding and must be zero.
- A record whose CRC fails is rejected, and the rule list simply ends at that point: the valid prefix still loads, and later rules keep their indices. Records from before the CRC bump have no checksum and stay length-checked.

## Channel records

Channel records are 50 bytes with no CRC. Their integrity is covered by the bounds-checked header, fixed record lengths and the staged-save readback. Channel records and the battery config keep the length-only story until their own layout change adds a checksum.

## Migration chain

- **Older configs keep loading.** Records from v3, v4, v5 and v6 are migrated one version at a time (v3 to v4 to v5 to v6 to v7) on load, with the same outcomes those versions always produced. The next save rewrites everything as v7 with CRCs.
- The chain is one named step function per version pair in `PacketFilter.cpp`, kept contiguous. Never skip a middle step in code. Removing a version later is one excision: delete its step function, raise the floor in `rule_bytes_for()`, delete its fixtures. Older record loading is temporary and is expected to be dropped once the field has migrated.
- A config written by any firmware version still in the field must load with settings intact. A change is either directly backwards compatible, with new fields riding tail padding so the old record is a byte-identical prefix of the new one (enforced by the `static_assert`s in `PacketFilter.h`), or it ships migration code in `load()` plus a version bump. Every version bump keeps older configs loading, with roundtrip and old-version fixtures in the persistence suite.

## Downgrade reset

**Downgrading firmware is a reset.** A file written by a newer firmware carries a version byte this firmware rejects, so the whole file is discarded and the repeater starts from defaults. Rules can be re-added or re-flashed forward.

## Staged save mechanics

- Edits are lazy: an edit is written to flash about **3 seconds** after it is made, and a burst of commands costs one write, not one each. Power the repeater off within that window and the edit is lost; everything before it is safe.
- Saving is staged: the repeater writes a scratch copy, reads it back to confirm it, keeps the previous config as a backup, and only then swaps it in. If the main config ever fails to load, the backup is used and a repaired copy is written at the next save. The worst case is that the most recent edit is lost, not the whole rule list.
- This is recovery in the firmware, not a guarantee about the flash itself: a power cut *inside* the filesystem's own write can still damage the underlying storage. There is no sidecar file, because a second file is a second thing to go missing.
- A truncated save is detected on load; a torn write of exactly the right length is caught only by a record checksum. A checksum rides a version bump, never a patch.
