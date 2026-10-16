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
// RAM-only: a reboot starts an empty batch. Only the enabled flag persists, in
// /meshclock. The feature shares no state with AdvertRateLimiter or the filter,
// so it can be deleted cleanly if upstream ever adds its own clock feature.
//
// CLI: "clock mesh on|off" and a bare "clock mesh" status, handled by
// meshClockCLI() (see MeshClockSync.cpp).

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

// Persisted /meshclock record: the enabled flag plus a CRC-16 over it. A torn
// write of exactly the right length is otherwise undetectable on a record this
// small, and the CRC rides v1 from day one (no older format to stay compatible
// with).
struct MeshClockCfg {
  uint8_t  version;   // MESHCLOCK_CFG_VERSION
  uint8_t  enabled;   // 0/1
  uint16_t crc;       // crc16_ccitt over the version + enabled bytes
};
static_assert(sizeof(MeshClockCfg) == 4, "clock config must stay 4 bytes");

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

  // Feed one signature-verified advert (the core verifies the signature before
  // MyMesh::onAdvertRecv). advert_ts is the signed timestamp, pub_key the
  // PUB_KEY_SIZE origin key, local_now this node's RTC now.
  void onAdvert(uint32_t advert_ts, const uint8_t* pub_key, uint32_t local_now);

  // Back to a freshly constructed sync (disabled, empty batch), for tests.
  void reset();

  // --- status for the CLI ---
  int getSampleCount() const { return count; }
  Result getLastResult() const { return last_result; }
  int32_t getLastDeltaSecs() const { return last_delta; }   // signed for display

  // Lower plausibility bound for an advert timestamp, derived from the build
  // epoch. Exposed for a boundary test.
  static uint32_t buildFloor();

private:
  MeshClockSample samples[MESHCLOCK_SLOTS];
  int count;
  bool enabled;
  Result last_result;
  int32_t last_delta;
  LazySave save_flag;      // needs save, written back by loop()

  // device service (see setClockFn); NULL = unset. Wiring, not state: it has an
  // in-class initializer and reset() must never touch it.
  void (*set_fn)(void*, uint32_t) = NULL;
  void* set_ctx = NULL;

  // state of a fresh node, and the baseline load() resets to before it reads
  void resetToDefaults();
  bool hasSource(const uint8_t* fp) const;
  void clear() { count = 0; }
  void evaluate(uint32_t local_now);

  // persistence
  void load(FILESYSTEM* fs);
  void save(FILESYSTEM* fs);
  void markDirty() { save_flag.markDirty(); }
};

// CLI command handler: invoke with the command after "clock mesh" (prefix
// removed). Replies must fit the 160-byte CLI reply buffer.
void meshClockCLI(MeshClockSync& clock, const char* command, char* reply);

#endif // _MESH_CLOCK_SYNC_H
