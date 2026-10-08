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
#include "CliScript.h"

// Injected device services, so FleetManager stays host-testable. MyMesh wires
// them in begin(); the native tests stub them.
//   clock: unix epoch seconds (the node RTC)
//   jitter: a uniform random value in [0, window) for ack reply scheduling
//   sender: sends `body` as a plain group text (txt_type 0x00) on the captured
//           channel, with the repeater's own name as the sender
typedef uint32_t (*FleetTimeFn)(void* ctx);
typedef uint32_t (*FleetJitterFn)(void* ctx, uint32_t window);
typedef void (*FleetSendFn)(void* ctx, const uint8_t* chan_secret, uint8_t chan_hash,
                            const char* body);

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
  CliScriptRunner runner;    // script queue + seen ring + counters

  // armed !at scripts (RAM-only, FLEET_SCHED_STORE entries): a reboot forgets
  // them — re-send the job; the sync-verify-schedule recipe re-arms cheaply
  struct FleetSched {
    char text[MAX_PACKET_PAYLOAD + 1];
    char key[FLEET_CLI_KEY_LEN + 1];
    uint8_t ack;
    uint16_t body_off;                   // command-body offset from the parse
    uint8_t chan_secret[PUB_KEY_SIZE];   // captured channel of arrival
    uint8_t chan_hash;
    uint32_t due_epoch;                  // unix seconds UTC
  };
  FleetSched sched[FLEET_SCHED_STORE];
  int sched_count;
  // kill switch raised by `fleet off` and `fleet chan clear`: the next
  // runScripts() pass cancels every queued, sleeping and armed script
  bool cancel_pending;
  // content hash of the packet the core last MAC-verified under the fleet key:
  // set by onGroupData(), consumed by checkForward() for the same packet
  uint8_t auth_hash[MAX_HASH_SIZE];
  bool auth_valid;

  // pending acknowledgements (RAM-only, FLEET_REPLY_STORE entries): summary +
  // captured channel move here when a script finishes, and the reply is sent
  // once its jitter deadline passes
  struct FleetReply {
    char key[FLEET_CLI_KEY_LEN + 1];
    char summary[FLEET_REPLY_SUMMARY_LEN];
    uint8_t chan_secret[PUB_KEY_SIZE];
    uint8_t chan_hash;
    uint32_t deadline_ms;                // absolute millis(); wrap-safe compare
  };
  FleetReply replies[FLEET_REPLY_STORE];
  int reply_count;

  // RAM-only telemetry: scripts offered/matched on the fleet channel
  uint32_t offered;
  uint32_t matched;

  LazySave save_flag;        // needs save, written back by loop()

  // device services (see typedefs above); NULL = unset: the clock counts as
  // unset for !at admission, and replies are sent without jitter. Wiring,
  // not state: MyMesh::begin() sets them just before load(), so they get
  // in-class initializers and resetToDefaults() must never touch them.
  FleetTimeFn time_fn = NULL;       void* time_ctx = NULL;
  FleetJitterFn jitter_fn = NULL;   void* jitter_ctx = NULL;
  FleetSendFn send_fn = NULL;       void* send_ctx = NULL;

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

  // the script runner (queue + seen ring): thin forwards for the CLI, like
  // the filter's limiter accessors
  CliScriptRunner& getScripts() { return runner; }
  uint32_t getOffered() const { return offered; }
  uint32_t getMatched() const { return matched; }
  int getSchedCount() const { return sched_count; }
  int getReplyCount() const { return reply_count; }

  // device service wiring (MyMesh::begin); see the typedefs above
  void setClock(FleetTimeFn fn, void* ctx) { time_fn = fn; time_ctx = ctx; }
  void setJitter(FleetJitterFn fn, void* ctx) { jitter_fn = fn; jitter_ctx = ctx; }
  void setSender(FleetSendFn fn, void* ctx) { send_fn = fn; send_ctx = ctx; }

  // Receive-side hook: called from MyMesh::onGroupDataRecv() BEFORE the
  // battery gate's early return, so scripts still arrive while forwarding is
  // suspended (§ fleet supersedes the gate). Parses the message, matches tags,
  // and queues/admits the script — nothing executes here.
  // `packet` is the MAC-verified packet the core delivered (its content hash
  // is the relay exemption's identity; see checkForward()).
  void onGroupData(const mesh::Packet* packet, uint8_t type, const mesh::GroupChannel& channel,
                   const uint8_t* data, size_t len);

  // Relay-side hook (MyMesh::allowPacketForward): returns true — meaning "let
  // the battery gate decide", not "allow" — except for the GRP_TXT/GRP_DATA
  // packet onGroupData() just recorded as MAC-verified under the fleet key.
  // That one returns false so the gate is never consulted: fleet traffic relays
  // while suspended, and is never counted as a battery drop. The one-byte
  // channel hash alone never exempts a packet (1 in 256 channels collide).
  bool checkForward(const mesh::Packet* packet);

  // Main-loop drain, called from MyMesh::loop() after mesh::Mesh::loop():
  // moves due scheduled scripts into the run queue, works one queued script
  // through fn (same cadence as the old filter drain), and sends
  // acknowledgements whose jitter deadline has passed.
  void runScripts(CliExecFn fn, void* exec_ctx);

  // Keyed-channel supply for the core's group decryption: offers the fleet
  // channel when its hash matches, skipping a secret already in the `filled`
  // leading entries of `dest` (the filter store may hold the same PSK; a
  // duplicate would burn one of core's few candidate slots — and only that
  // prefix is initialised, core hands over raw stack for the rest). Appends
  // at dest[filled]; returns how many entries were appended.
  int appendChannelByHash(const uint8_t* hash, mesh::GroupChannel dest[], int max_matches,
                          int filled);

  void markDirty() { save_flag.markDirty(); }

  // persistence
  void load(FILESYSTEM* fs);
  void save(FILESYSTEM* fs);

private:
  // state of a fresh node, and the baseline load() resets to before reading
  void resetToDefaults();
  // scheduled-script admission (staleness, clock floor, store capacity) and
  // the acknowledgement machinery behind runScripts()
  void admitScheduled(const char* text, const CliScriptMeta& meta,
                      const mesh::GroupChannel& chan);
  void scheduleAckIfNeeded(const CliRunResult& res);
  // a script with this key is armed or queued: a re-send must not run it twice
  bool inFlight(const char* key) const;
  void sendDueReplies();
};

// CLI command handler: invoke with the command after "fleet" (prefix removed).
// Replies must fit the 160-byte CLI reply buffer.
void fleetCLI(FleetManager& fleet, const char* command, char* reply);

#endif // _FLEET_MANAGER_H