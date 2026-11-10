// MeshClockSyncConfig.h — build-time tunables for mesh clock sync
// (MeshClockSync.h/.cpp). All values have an #ifndef default; override via
// build_flags (e.g. in platformio.local.ini: -DMESHCLOCK_SLOTS=15) or by
// editing the defaults below. See MESHCLOCK.md for the user manual.

#ifndef _MESH_CLOCK_SYNC_CONFIG_H
#define _MESH_CLOCK_SYNC_CONFIG_H

// distinct sources held per batch. This is also the quorum size: the batch
// fills, decides once, then clears, so there is no separate time window or
// second threshold. This is both the shipped default and the compile-time
// upper bound the sample array is sized for; at runtime it can be lowered
// (with the quorum below) via `clock mesh slots` — see MESHCLOCK.md.
// 33 is odd, so the median is the single middle value.
#ifndef MESHCLOCK_SLOTS
  #define MESHCLOCK_SLOTS 33
#endif

// a candidate counts as agreeing with the median when it is within this many
// seconds of it (default; runtime-tunable via `clock mesh tolerance`)
#ifndef MESHCLOCK_TOLERANCE_SECS
  #define MESHCLOCK_TOLERANCE_SECS 60
#endif

// agreeing sources required before the clock is stepped: two thirds of the
// default batch. This is the shipped default; `clock mesh slots` sets it at
// runtime together with the batch size.
#ifndef MESHCLOCK_MIN_AGREE
  #define MESHCLOCK_MIN_AGREE 22
#endif

// deadband: a median closer than this to the node's own clock is "close
// enough", so the RTC is left alone and the batch reports in-sync
// (default; runtime-tunable via `clock mesh tolerance`)
#ifndef MESHCLOCK_MIN_DIFF_SECS
  #define MESHCLOCK_MIN_DIFF_SECS 60
#endif

// persisted config (see MeshClockSync.cpp for the record): v2 carries the
// enabled flag plus the runtime batch-size/quorum and tolerance/deadband knobs
#ifndef MESHCLOCK_CFG_FILE
  #define MESHCLOCK_CFG_FILE "/meshclock"
#endif
#define MESHCLOCK_CFG_VERSION 2

// hard floor for the runtime batch size (`clock mesh slots`); a smaller batch
// has no median worth taking. Not a tunable — a range bound the CLI and the
// loader both enforce.
#define MESHCLOCK_SLOTS_MIN 3

static_assert(MESHCLOCK_MIN_AGREE >= 2 && MESHCLOCK_MIN_AGREE <= MESHCLOCK_SLOTS,
              "MESHCLOCK_MIN_AGREE must be within the batch");
static_assert(MESHCLOCK_SLOTS_MIN >= 2 && MESHCLOCK_SLOTS_MIN <= MESHCLOCK_SLOTS,
              "MESHCLOCK_SLOTS_MIN must be within the batch");

#endif // _MESH_CLOCK_SYNC_CONFIG_H
