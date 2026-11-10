// MeshClockSync.h — mesh clock sync for the simple_repeater firmware: estimate
// UTC from the signed adverts the repeater already hears, so features that need
// a plausible clock (notably the fleet feature's !at scheduling) work on a node
// with no GPS, no NTP and no battery-backed RTC. Personal fork feature; all
// fork-owned code lives in this file pair plus minimal hook lines in
// MyMesh.h/MyMesh.cpp (same footprint as the packet filter). No changes to
// MeshCore core sources. See MESHCLOCK.md for the user manual.
//
// Off by default. When enabled, each distinct source contributes one vote per
// batch (a 4-byte fingerprint of its advert key); when the batch fills, the
// batch's median predicted "now" is taken, and if enough sources agree it steps
// the RTC once and clears. The batch, the last result and its delta are
// RAM-only: a reboot starts an empty batch. The enabled flag and the tuning
// knobs persist, in /meshclock. The feature shares no state with
// AdvertRateLimiter or the filter, so it can be deleted cleanly if upstream
// ever adds its own clock feature.
//
// CLI: "clock mesh on|off", "clock mesh slots <slots> <min-agree>",
// "clock mesh tolerance <tolerance-s> <min-diff-s>", and a bare "clock mesh"
// status (with the node's current UTC time), handled by meshClockCLI() (see
// MeshClockSync.cpp). `clock mesh slots` sets the batch size and quorum (the
// compile-time MESHCLOCK_SLOTS is the shipped default and the upper bound —
// the sample array is sized for it). Both commands take both values — nothing
// is derived — and are rejected whole on a bad pair.

#ifndef _MESH_CLOCK_SYNC_H
#define _MESH_CLOCK_SYNC_H

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/IdentityStore.h>   // FILESYSTEM typedef
#include "MeshClockSyncConfig.h"
#include "PersistUtil.h"

// One source's vote: the fingerprint sampled from its advert key, and the
// modular difference between its signed advert timestamp and this node's clock
// at receipt. The offset is unsigned on purpose — modular arithmetic re-adds it
// with no sign handling, and only the ±2^31 (~68 year) ambiguity inherent to a
// 32-bit epoch remains, the same limit the RTC already has.
struct MeshClockSample {
  uint8_t  fp[4];    // fingerprint bytes sampled from the advert public key
  uint32_t offset;   // advert_timestamp - local_now_at_receipt (wraps)
};
static_assert(sizeof(MeshClockSample) == 8, "clock sample must stay 8 bytes");

// Persisted /meshclock record: the enabled flag plus the runtime tuning knobs
// and a CRC-16 over them. A torn write of exactly the right length is otherwise
// undetectable on a record this small. v1 records (enabled flag only) are still
// accepted by load() and migrated forward: the flag carries and the knobs take
// the compile-time defaults.
struct MeshClockCfg {
  uint8_t  version;   // MESHCLOCK_CFG_VERSION
  uint8_t  enabled;   // 0/1
  uint8_t  slots;     // runtime batch capacity
  uint8_t  min_agree; // agreeing sources required to step the clock
  uint16_t tolerance; // seconds an inlier may sit from the median
  uint16_t min_diff;  // seconds: closer than this to the node clock is "in sync"
  uint16_t crc;       // crc16_ccitt over the preceding bytes
};
static_assert(sizeof(MeshClockCfg) == 10, "clock config must stay 10 bytes");

class MeshClockSync {
public:
  enum Result { IDLE = 0, SET_CLOCK, IN_SYNC, NO_CONSENSUS };

  MeshClockSync();

  // fs = where the enabled flag is persisted
  void begin(FILESYSTEM* fs);   // load persisted config
  void loop(FILESYSTEM* fs);    // lazy dirty-flag save

  // Injected device service, so the unit is host-testable without MyMesh: called
  // with a chosen UTC epoch once a batch agrees. Wiring, not state.
  void setClockFn(void (*fn)(void* ctx, uint32_t epoch), void* ctx);

  bool isEnabled() const { return enabled; }
  void setEnabled(bool on);     // persists; clears the batch on change

  // Runtime tuning, persisted like the enabled flag. Both values must be sent
  // and are validated as a pair; on any change the batch is dropped (like
  // setEnabled), so samples gathered under old rules never decide under new
  // ones. Tolerance is the inlier window, min_diff the deadband. Return false
  // (nothing applied) on bad values.
  bool setSlots(uint8_t slots, uint8_t min_agree);
  bool setTolerance(uint16_t tolerance, uint16_t min_diff);

  // Feed one signature-verified advert (the core verifies the signature before
  // MyMesh::onAdvertRecv). advert_ts is the signed timestamp, pub_key the
  // PUB_KEY_SIZE origin key, local_now this node's RTC now.
  void onAdvert(uint32_t advert_ts, const uint8_t* pub_key, uint32_t local_now);

  // Back to a freshly constructed sync: disabled, empty batch, nothing pending
  // to save. Used by construction and load(), and directly by tests.
  void reset();

  // --- status for the CLI ---
  int getSampleCount() const { return count; }
  Result getLastResult() const { return last_result; }
  int32_t getLastDeltaSecs() const { return last_delta; }   // signed for display
  int getSlots() const { return slots; }
  int getMinAgree() const { return min_agree; }
  int getToleranceSecs() const { return tolerance; }
  int getMinDiffSecs() const { return min_diff; }

  // Lower plausibility bound for an advert timestamp: this firmware's build
  // epoch. Adverts dated earlier are refused. Exposed for the boundary test.
  static uint32_t buildFloor();

private:
  // sized for the compile-time maximum; the runtime `slots` caps how much of
  // it is used
  MeshClockSample samples[MESHCLOCK_SLOTS];
  int count;
  bool enabled;
  uint8_t slots;           // runtime batch capacity (<= MESHCLOCK_SLOTS)
  uint8_t min_agree;       // runtime quorum
  uint16_t tolerance;      // runtime inlier window, seconds
  uint16_t min_diff;       // runtime deadband, seconds
  Result last_result;
  int32_t last_delta;
  LazySave save_flag;      // needs save, written back by loop()

  // device service (see setClockFn); NULL = unset. Wiring, not state: it has an
  // in-class initializer and reset() must never touch it.
  void (*set_fn)(void*, uint32_t) = NULL;
  void* set_ctx = NULL;

  bool hasSource(const uint8_t* fp) const;
  void clear() { count = 0; }
  void evaluate(uint32_t local_now);

  // persistence
  void load(FILESYSTEM* fs);
  void save(FILESYSTEM* fs);
  void markDirty() { save_flag.markDirty(); }
};

// CLI command handler: invoke with the command after "clock mesh" (prefix
// removed). `now` is the node's current time, preformatted as an ISO 8601 UTC
// timestamp ("YYYY-MM-DDTHH:MM:SSZ"); the device layer supplies it so this
// unit stays free of RTClib and host-testable. Replies must fit the fork's
// bounded CLI reply buffer.
void meshClockCLI(MeshClockSync& clock, const char* now, const char* command, char* reply);

#endif // _MESH_CLOCK_SYNC_H
