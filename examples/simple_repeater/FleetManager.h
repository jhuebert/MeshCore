// FleetManager.h — fleet management for the simple_repeater firmware: one
// PSK-backed channel, this repeater's tag set, and the remote CLI scripts
// (CliScript.h) that arrive on that channel. Personal fork feature; all
// fork-owned code lives in this file pair plus minimal hook lines in
// MyMesh.h/MyMesh.cpp (same footprint as the packet filter). No changes to
// MeshCore core sources. See FLEET.md for the user manual.
//
// The fleet feature is a purely passive observer of decrypted group text on
// its channel: a message whose first line is a `!id` directive is a CLI
// script, and it runs on every repeater whose tag set intersects the script's
// `!tags` list (absent `!tags` = broadcast). Possession of the channel PSK is
// full admin access — the keyed-channel MAC must verify before anything
// reaches this manager.
//
// Config persists in /fleet_cfg, format v1 with a CRC-16 over the whole
// payload (a checksum rides a version bump, and v1 IS the version bump), using
// the same lazy-dirty-save and staged-save patterns as the filter. Pending
// acknowledgements, armed `!at` scripts and the runner's seen ring are
// RAM-only: a reboot forgets them, and re-sending a job re-arms it.
//
// CLI: "fleet ..." commands, handled by fleetCLI() (see FleetManager.cpp).

#ifndef _FLEET_MANAGER_H
#define _FLEET_MANAGER_H

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/IdentityStore.h>   // FILESYSTEM typedef
#include "FleetConfig.h"
#include "PersistUtil.h"

// ---------------------------------------------------------------- /fleet_cfg

#define FLEET_CFG_VERSION 1
#define FLEET_CFG_MAGIC   "FLTC"

// The persisted record (180 B payload + 2 B trailing CRC-16 on disk): magic +
// version + enabled + psk + tags + reply window, closed by a CRC-16 over the
// payload. The CRC rides v1 from day one — a torn write of exactly the right
// length is otherwise undetectable on a record this small, and no older format
// exists to stay compatible with. Fixed-size members, no pointers; the writer
// zero-fills before use, so a raw write/read is deterministic on a platform.
// FLEET_MAX_TAGS/FLEET_TAG_LEN size the tag array, so an override that moves
// reply_window_ms would silently corrupt configs between builds — refuse.
static_assert(FLEET_MAX_TAGS == 8 && FLEET_TAG_LEN == 16,
              "the tag caps size the /fleet_cfg record: raising one needs a "
              "FLEET_CFG_VERSION bump and migration code in load()");

struct FleetCfgRecord {
  char     magic[4];                                   // "FLTC"
  uint8_t  version;
  uint8_t  enabled;
  uint8_t  psk_len;                                    // 16 or 32; 0 = no channel
  uint8_t  psk[32];                                    // zero-padded
  uint8_t  tag_count;
  char     tags[FLEET_MAX_TAGS][FLEET_TAG_LEN + 1];
  uint32_t reply_window_ms;
};

static_assert(offsetof(FleetCfgRecord, reply_window_ms) == 176,
              "FleetCfgRecord::reply_window_ms must stay at 176 (v1 record layout)");
static_assert(sizeof(FleetCfgRecord) == 180,
              "FleetCfgRecord payload is frozen at 180 B; the live struct no longer matches");
#define FLEET_CFG_PAYLOAD_BYTES 180
#define FLEET_CFG_RECORD_BYTES  182   // payload + 2 B CRC-16

class FleetManager {
  bool enabled;
  uint8_t psk[32];           // zero-padded channel key (16 or 32 bytes used)
  uint8_t psk_len;           // 16 or 32; 0 = no channel set (receipt disabled)
  uint8_t chan_hash;         // sha256(psk)[0]: the 1-byte on-air channel hash;
                             // valid only when psk_len > 0
  char tags[FLEET_MAX_TAGS][FLEET_TAG_LEN + 1];
  int tag_count;
  uint32_t reply_window_ms;  // !ack reply jitter window (default 60 s)
  LazySave save_flag;        // needs save, written back by loop()

public:
  FleetManager();

  void begin(FILESYSTEM* fs);      // load persisted config
  void loop(FILESYSTEM* fs);       // lazy dirty-flag save (same pattern as filter)

  bool isEnabled() const { return enabled; }
  void setEnabled(bool on);

  // the one fleet channel: `psk_hex` is 16 or 32 bytes of hex and replaces
  // whatever was there; false when refused (nothing changed)
  bool setChannel(const char* psk_hex);
  void clearChannel();
  bool hasChannel() const { return psk_len > 0; }
  uint8_t getChanHash() const { return chan_hash; }
  uint8_t getChanSecretLen() const { return psk_len; }
  // zero-padded stored secret, for secret-equality comparison against a
  // delivered GroupChannel and for handing core a decrypt candidate
  const uint8_t* getChanSecret() const { return psk; }

  // this repeater's tag set (exact string identity; no wildcards)
  int getTagCount() const { return tag_count; }
  const char* getTag(int idx) const { return tags[idx]; }
  bool hasTag(const char* tag) const;
  // addTag validates charset/length (same grammar as a job key) and the cap;
  // duplicates no-op. false when refused.
  bool addTag(const char* tag);
  // false when the tag was not set
  bool delTag(const char* tag);
  void clearTags();

  uint32_t getReplyWindowMs() const { return reply_window_ms; }
  void setReplyWindowMs(uint32_t ms);

  void markDirty() { save_flag.markDirty(); }

  // persistence
  void load(FILESYSTEM* fs);
  void save(FILESYSTEM* fs);

private:
  // state of a fresh node, and the baseline load() resets to before reading
  void resetToDefaults();
};

// CLI command handler: invoke with the command after "fleet" (prefix removed).
// Replies must fit the 160-byte CLI reply buffer.
void fleetCLI(FleetManager& fleet, const char* command, char* reply);

#endif // _FLEET_MANAGER_H