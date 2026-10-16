// MeshClockSyncConfig.h — build-time tunables for mesh clock sync
// (MeshClockSync.h/.cpp). All values have an #ifndef default; override via
// build_flags (e.g. in platformio.local.ini: -DMESHCLOCK_SLOTS=15) or by
// editing the defaults below. See MESHCLOCK.md for the user manual.

#ifndef _MESH_CLOCK_SYNC_CONFIG_H
#define _MESH_CLOCK_SYNC_CONFIG_H

// distinct sources held per batch. This is also the quorum size: the batch
// fills, decides once, then clears, so there is no separate time window or
// second threshold. 33 is odd, so the median is the single middle value; a site
// with a sparser mesh can shrink both this and the quorum below.
#ifndef MESHCLOCK_SLOTS
  #define MESHCLOCK_SLOTS 33
#endif

// a candidate counts as agreeing with the median when it is within this many
// seconds of it
#ifndef MESHCLOCK_TOLERANCE_SECS
  #define MESHCLOCK_TOLERANCE_SECS 60
#endif

// agreeing sources required before the clock is stepped: two thirds of the
// default batch. Sites overriding MESHCLOCK_SLOTS should override this to keep
// their desired quorum fraction.
#ifndef MESHCLOCK_MIN_AGREE
  #define MESHCLOCK_MIN_AGREE 22
#endif

// deadband: a median closer than this to the node's own clock is "close
// enough", so the RTC is left alone and the batch reports in-sync
#ifndef MESHCLOCK_MIN_DIFF_SECS
  #define MESHCLOCK_MIN_DIFF_SECS 60
#endif

// persisted enabled flag (see MeshClockSync.cpp for the record)
#ifndef MESHCLOCK_CFG_FILE
  #define MESHCLOCK_CFG_FILE "/meshclock"
#endif
#define MESHCLOCK_CFG_VERSION 1

static_assert(MESHCLOCK_MIN_AGREE >= 2 && MESHCLOCK_MIN_AGREE <= MESHCLOCK_SLOTS,
              "MESHCLOCK_MIN_AGREE must be within the batch");

#endif // _MESH_CLOCK_SYNC_CONFIG_H
