// PacketFilter.h — remote-configurable packet drop rules for the simple_repeater
// firmware. Personal fork feature; all fork-owned code lives in this file set
// (PacketFilter.h/.cpp, PacketFilterConfig.h, TinyRegex.h/.cpp) plus minimal
// hook lines in MyMesh.h/MyMesh.cpp. No changes to MeshCore core sources.
//
// A rule is a conjunction of optional predicates; the enabled rules are
// evaluated in listed order and the first one whose predicates all match
// decides the packet's verdict (first match wins). Unspecified predicate =
// wildcard. On top of the predicates, prob= and throttle= are match gates: a
// rule whose predicates match still only decides a packet its gates admit
// (see probDecides/throttleDecides). Actions: drop (enforce), forward
// (terminal: stop the list, count the hit, let the packet through) and cli
// (forward plus queue the message text as a CLI script, executed deferred —
// see CliScript.h).
//
// For a decrypted group packet the whole rule list — packet-level and content
// predicates alike — is evaluated once in checkContent() (called from
// onGroupDataRecv(); keyed channel identity is proven by successful MAC
// verification) and the verdict is handed to checkPacket() via a
// pointer+content-hash-tagged stash. For every other packet (adverts, trace, direct,
// group that fails to decrypt) checkPacket() runs the packet-level predicates
// only: content predicates (keyed channel / sender / text) can only match
// decryption-proven traffic, so rules carrying them are skipped.
//
// CLI: "filter ..." commands, handled by filterCLI() (see PacketFilter.cpp).

#ifndef _PACKET_FILTER_H
#define _PACKET_FILTER_H

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/IdentityStore.h>   // FILESYSTEM typedef
#include <helpers/RegionMap.h>       // RegionEntry (region= predicate)
#include "PacketFilterConfig.h"
#include "PatternMatch.h"
#include "PersistUtil.h"
#include "AdvertRateLimiter.h"
#include "CliScript.h"

// actions
#define FILTER_ACT_ALLOW     0
#define FILTER_ACT_DROP      1
#define FILTER_ACT_FORWARD   2   // value 2 = the old logonly byte; configs
                                 // load identically across the rename
#define FILTER_ACT_CLI       3   // forward + enqueue the message text as a
                                 // deferred CLI script (CliScriptRunner); rides
                                 // the existing action byte, no record growth

// payload-type mask bits (indexed by mesh::Packet payload type, 4 bits).
// All types a rule can name, low byte first: the filter sees every packet the
// repeater would relay, so these are naming/visibility only.
#define FILTER_TYPE_REQ      (1 << PAYLOAD_TYPE_REQ)
#define FILTER_TYPE_RESPONSE (1 << PAYLOAD_TYPE_RESPONSE)
#define FILTER_TYPE_TXT_MSG  (1 << PAYLOAD_TYPE_TXT_MSG)
#define FILTER_TYPE_ACK      (1 << PAYLOAD_TYPE_ACK)
#define FILTER_TYPE_ADVERT  (1 << PAYLOAD_TYPE_ADVERT)
#define FILTER_TYPE_GRP_TXT (1 << PAYLOAD_TYPE_GRP_TXT)
#define FILTER_TYPE_GRP_DATA (1 << PAYLOAD_TYPE_GRP_DATA)
#define FILTER_TYPE_ANON_REQ (1 << PAYLOAD_TYPE_ANON_REQ)
#define FILTER_TYPE_PATH      (1 << PAYLOAD_TYPE_PATH)
#define FILTER_TYPE_TRACE     (1 << PAYLOAD_TYPE_TRACE)
#define FILTER_TYPE_MULTIPART (1 << PAYLOAD_TYPE_MULTIPART)
#define FILTER_TYPE_CONTROL   (1 << PAYLOAD_TYPE_CONTROL)
#define FILTER_TYPE_RAW       (1 << PAYLOAD_TYPE_RAW_CUSTOM)

// Every bit a rule may carry: payload types 0x00..0x0B and 0x0F are defined in
// Packet.h; bits 12..14 are not, so validRule() refuses them everywhere.
#define FILTER_TYPE_MASK_ALL (FILTER_TYPE_REQ | FILTER_TYPE_RESPONSE | FILTER_TYPE_TXT_MSG | \
                              FILTER_TYPE_ACK | FILTER_TYPE_ADVERT | FILTER_TYPE_GRP_TXT | \
                              FILTER_TYPE_GRP_DATA | FILTER_TYPE_ANON_REQ | FILTER_TYPE_PATH | \
                              FILTER_TYPE_TRACE | FILTER_TYPE_MULTIPART | FILTER_TYPE_CONTROL | \
                              FILTER_TYPE_RAW)

// route_mask bits
#define FILTER_ROUTE_FLOOD   0x01
#define FILTER_ROUTE_DIRECT  0x02

// path predicate position (anchor) flags
#define FILTER_PATH_ANY      0
#define FILTER_PATH_FIRST    1
#define FILTER_PATH_LAST     2

// chan_flags bits
#define FILTER_CHANFLG_MASK_SET   0x01
#define FILTER_CHANFLG_HASH_SET   0x02

// Interval flags: shared numeric-predicate form (hops, len, snr)
#define FILTER_IV_LO_INC  0x01   // lo endpoint inclusive
#define FILTER_IV_HI_INC  0x02   // hi endpoint inclusive
#define FILTER_IV_LO_ANY  0x04   // lo unbounded (*)
#define FILTER_IV_HI_ANY  0x08   // hi unbounded (*)
// flags == 0 -> predicate unset (wildcard). A both-exclusive interval
// "(a,b)" would otherwise encode as 0 and read back as a wildcard, so
// parseInterval() stores FILTER_IV_SET alone for that case (endpoint logic
// is unchanged: absent LO_INC/HI_INC still means exclusive).
#define FILTER_IV_SET     0x10   // predicate is set (both endpoints exclusive)

#if FILTER_MAX_CHANNELS > 32
  #error "FILTER_MAX_CHANNELS > 32 needs a wider FilterRule::chan_mask"
#endif

struct FilterChannel {                  // keyed channel store
  char     name[FILTER_CHAN_NAME_LEN]; // e.g. "Public", "#test" (hash channels keep their '#')
  uint8_t  secret[32];                 // zero-padded PSK (16 or 32 bytes used)
  uint8_t  secret_len;                 // 16 or 32
  uint8_t  hash;                       // sha256(secret)[0]: the 1-byte on-air
                                       // channel hash (core PATH_HASH_SIZE)
};

struct Interval {
  uint16_t lo, hi;       // predicate-specific units (hops, bytes, quarter-dB)
  uint8_t  flags;        // FILTER_IV_* bits; 0 = unset
};

struct FilterRule {
  bool     enabled;
  uint8_t  action;        // FILTER_ACT_DROP | FILTER_ACT_FORWARD | FILTER_ACT_CLI
  uint8_t  type_mask;     // bits 0..7 by payload type (see FILTER_TYPE_*); 0 = any
  uint8_t  route_mask;    // FILTER_ROUTE_* bits; 0 = any
  Interval hops;          // flood path length (getPathHashCount)
  Interval len;           // payload length
  Interval snr;           // SNR in quarter-dB units (int16 stored in lo/hi)
  struct {                // path predicate; count == 0 = unset
    uint8_t bytes[FILTER_PATH_HASH_SLOTS][4];   // leading bytes of repeater pubkey hashes
    uint8_t len[FILTER_PATH_HASH_SLOTS];        // 1-4 bytes matched per entry
    uint8_t count;        // hashes in the '>'-separated chain (1-4)
    uint8_t pos;          // FILTER_PATH_* ; anchor position
  } path;
  uint8_t  hash_size_mask; // bit0..3 = path hash size 1..4; 0 = any
  uint32_t chan_mask;      // keyed channels by stored name; bitmask over the
                           // FILTER_MAX_CHANNELS store; 0 = unset (deferred to
                           // onGroupDataRecv, identity proven by MAC decrypt)
  uint8_t  chan_hash;      // 1-byte air hash (chanhash=XX); valid only w/ flag
  uint8_t  chan_flags;     // FILTER_CHANFLG_* bits
  char     sender[FILTER_SENDER_PATTERN_LEN];  // regex over "<sender>:"; empty = wildcard
  char     text[FILTER_TEXT_PATTERN_LEN];      // regex over message text;   empty = wildcard
  char     regions[FILTER_REGION_LIST_LEN];    // comma list of canonical region names
                                           // and/or "unscoped"; empty = wildcard
  uint8_t  prob;           // match probability %; 0 = unset = always (100 %).
                           // Lands in the tail padding after regions, so the
                           // persisted record size is unchanged (v4 configs read
                           // back with prob == 0)
  uint16_t throttle;       // rate gate: the rule decides only matches over one
                           // per N seconds (1..65535 s); 0 = unset = no limit.
                           // (Historical: the v4/v5 record ended exactly where
                           // this field begins, which is why it sits here.)
  uint8_t  type_mask_hi;   // payload-type bits 8..15 (see FILTER_TYPE_*); 0 =
                           // any there. Sits in the 2 bytes v6 records left as
                           // unguaranteed tail padding before `hits`: v7 owns
                           // them now, one field plus one reserved byte.
  // byte 187 (the last before `hits`) is padding, format-guaranteed ZERO in
  // v7: the writer forces it and the reader refuses a v7 record that has it
  // set, so a future field can take the byte the way type_mask_hi took 186
  uint32_t hits;           // match counter (forward/drop telemetry + validation)
  uint32_t air_ms;         // estimated on-air time (ms) billed by this rule's
                           // DROP decisions; RAM-only stat, after `hits` so it
                           // is never persisted (persist ends at offsetof(hits))
  // RAM-only throttle state — never persisted (persist ends at offsetof(hits));
  // travels with the rule on move/del, like hits
  uint64_t throttle_last_ms; // stamp of the last within-budget pass, on this
                            // repeater's 64-bit monotonic clock (never persisted)
  uint32_t throttle_pass;    // within-budget passes (slipped past the rule)
  bool     throttle_seen;    // a pass has been stamped (first match = free pass)
};

// The v7 record layout is frozen: the persisted payload (struct bytes 0..187)
// ends where the RAM-only `hits` counter begins, and a 2-byte CRC-16 closes the
// 190-byte record on disk. type_mask_hi took the first of the 2 bytes v6 left
// as unguaranteed tail padding. A build-time override of FILTER_REGION_LIST_LEN,
// FILTER_SENDER_PATTERN_LEN, FILTER_TEXT_PATTERN_LEN or FILTER_PATH_HASH_SLOTS
// that moves any of the pinned fields would silently corrupt config upgrades;
// refuse to build. (Earlier layouts: v4/v5 ended at offsetof(throttle), v3 at
// 124 B; their records are migrated stepwise in load(), never re-derived.)
static_assert(offsetof(FilterRule, throttle) == 184,
              "FilterRule::throttle must stay at 184 (v7 record layout)");
static_assert(offsetof(FilterRule, type_mask_hi) == 186,
              "FilterRule::type_mask_hi must stay at 186 (v7 record layout)");
static_assert(offsetof(FilterRule, hits) == 188,
              "FilterRule::hits must stay at 188: the v7 payload is frozen at 188 B");
// FILTER_PATH_HASH_SLOTS sizes FilterRule::path, so it moves every field after
// it and a config saved by one build would not load in another. Name the knob
// that was raised here, rather than failing on the record invariant it broke.
static_assert(FILTER_PATH_HASH_SLOTS == 4,
              "FilterRule::path size is in the persisted record: this needs a "
              "FILTER_CFG_VERSION bump and migration code in load()");

// On-disk rule record sizes, in BYTES, frozen as literals.
//
// These used to be derived from offsetof(FilterRule, ...). That was a trap: the
// moment a field moved, every historical size silently changed too, and a config
// written by an older firmware would load as something it never was — with no
// compile error and no test failure to explain it. A format is a promise to
// bytes that already exist in the field, so the historical sizes are numbers,
// not expressions.
#define FILTER_RULE_V3_BYTES  124   // ends where `regions` began, padded to uint32_t
#define FILTER_RULE_V4_BYTES  156   // v4 and v5 share one size: ends where `throttle` begins
#define FILTER_RULE_V6_BYTES  160   // v6: ends where the RAM-only `hits` counter begins
#define FILTER_RULE_V7_PAYLOAD 188  // v7: struct bytes 0..187 (same rule as v6's 160)
#define FILTER_RULE_V7_BYTES  190   // v7 rule record on disk: payload + 2 B CRC-16
#define FILTER_CHAN_PERSIST_BYTES  50

// The frozen sizes must still describe THIS struct. These asserts are the
// tripwire: a new or moved field in the persisted prefix makes one of them fail,
// which is the moment to bump FILTER_CFG_VERSION and add migration code in
// load() — never to edit the frozen numbers to match.
static_assert(offsetof(FilterRule, hits) == FILTER_RULE_V7_PAYLOAD,
              "v7 payload size is frozen at 188 B; the live struct no longer matches");
static_assert(sizeof(FilterChannel) == FILTER_CHAN_PERSIST_BYTES,
              "channel record size is frozen at 50 B; the live struct no longer matches");

// One packet hash per scan, computed on first use. The prob roll and the
// content-verdict stash guard both need Packet::calculatePacketHash(), a SHA-256
// over the payload; hashing per prob-enabled rule made a list of prob rules pay
// one hash per matching rule on every packet.
class PacketHashCache {
  const mesh::Packet* pkt;
  uint8_t hash[MAX_HASH_SIZE];
  bool valid;
public:
  explicit PacketHashCache(const mesh::Packet* p) : pkt(p), valid(false) { memset(hash, 0, sizeof(hash)); }
  const uint8_t* get() {
    if (!valid) { pkt->calculatePacketHash(hash); valid = true; }
    return hash;
  }
};

class FilterRules {
  FilterRule rules[FILTER_MAX_RULES];
  int num_rules;
  FilterChannel channels[FILTER_MAX_CHANNELS];
  int num_channels;
  AdvertRateLimiter limiter;   // per-node advert repeat window
  CliScriptRunner cli_scripts;   // remote CLI scripts queued by action=cli rules
  // A 64-bit monotonic clock folded from the 32-bit millis(). Unsigned
  // subtraction handles exactly one wrap: it cannot tell 49.7 days + 1 s from
  // 1 s, so a repeater left up long enough would spuriously rate-limit a rule or
  // an origin the moment it returned. RAM-only; nothing here is persisted.
  uint64_t uptime_ms;
  uint32_t last_millis;
  uint32_t budget_aborts;     // regex evaluations aborted on step-budget exhaustion
  uint64_t air_saved_ms;      // estimated TX airtime (ms) saved by rule + limiter drops
  uint64_t air_evaluated_ms;  // estimated TX airtime (ms) evaluated; both counters RAM-only
  struct {                    // verdict stashed by checkContent() for the packet
    const mesh::Packet* pkt;  // currently being relayed; consumed by checkPacket()
    uint8_t hash[MAX_HASH_SIZE];  // pointer + content hash guard against pool reuse
    uint8_t verdict;          // FILTER_ACT_* (allow included)
  } content_verdict;
  bool enabled;
  LazySave save_flag;         // needs save, written back by loop()

public:
  FilterRules();

  void begin(FILESYSTEM* fs);      // load persisted config, pre-provision Public channel
  void loop(FILESYSTEM* fs);       // lazy dirty-flag save (same pattern as ClientACL)

  // Forget any content verdict stashed for the packet currently being received.
  // MyMesh::onRecvPacket() brackets one receive with this so a verdict can never
  // outlive the operation that produced it; checkPacket() also consumes it.
  void clearContentVerdict() { content_verdict.pkt = NULL; }

  // Fold the 32-bit millis() into the 64-bit accumulator and return it. Safe to
  // call as often as the loop runs — each call adds the time since the previous
  // one, and equal readings add zero — which is what lets loop() guarantee the
  // wrap is noticed even when no packet arrives.
  uint64_t uptimeMillis(uint32_t now_millis) {
    uptime_ms += (uint32_t)(now_millis - last_millis);
    last_millis = now_millis;
    return uptime_ms;
  }

  bool isEnabled() const { return enabled; }
  void setEnabled(bool on);

  // rule management
  int getNumRules() const { return num_rules; }
  FilterRule* getRule(int idx) { return &rules[idx]; }
  FilterRule* addRule();           // returns NULL if full
  void delRule(int idx);
  void moveRule(int from, int to); // rule ends up AT index `to`; hits travel with it
  void clearRules();

  // keyed channel store
  int getNumChannels() const { return num_channels; }
  FilterChannel* getChannel(int idx) { return &channels[idx]; }
  FilterChannel* findChannel(const char* name);
  // index of a stored channel, or -1; the store is dense, so the index is also
  // the bit a rule's chan_mask uses
  int indexOfChannel(const char* name) const;
  // addChannel: psk_hex required for non-'#' names; NULL/empty for '#name'
  // derives secret = sha256(name)[0..15] per the companion protocol.
  FilterChannel* addChannel(const char* name, const char* psk_hex);
  void delChannel(int idx);

  // For a packet whose verdict was stashed by checkContent() (same buffer and
  // content hash): return that verdict without rescanning. Otherwise
  // match packet-level predicates (type/route/region/hops/len/snr/chanhash/
  // path/hashsize; content rules are skipped) and apply the advert rate
  // limiter — it runs unless the verdict is DROP, so it still applies after a
  // forward verdict. `region` is the region the packet arrived in (NULL =
  // direct-routed or unknown transport code). `est_air_ms` is the estimated
  // time-on-air of retransmitting the packet (ms), billed to airtime telemetry
  // on DROP decisions (0 = unknown: nothing billed). Returns FILTER_ACT_*.
  uint8_t checkPacket(const mesh::Packet* pkt, uint32_t now_millis, const RegionEntry* region,
                      uint32_t est_air_ms = 0);

  // Commit a forwarding decision that actually succeeded: starts an advert's
  // rate-limit window. Called from the end of allowPacketForward(), never from
  // the check, so the window means "relayed", not "received".
  void onForwardAllowed(const mesh::Packet* pkt, uint32_t now_millis);

  // Single-pass evaluation of the ENTIRE rule list on a decrypted group
  // payload (packet-level predicates via ruleMatchesPacket, then chan keyed /
  // sender / text), in listed order; first enabled match decides (first match
  // wins) and its verdict — allow, forward, cli or drop — is stashed for
  // checkPacket(). A cli verdict additionally queues the message text as a
  // deferred CLI script (CliScriptRunner); the caller only special-cases DROP.
  // Caller drops the packet if this returns FILTER_ACT_DROP.
  // `est_air_ms`: see checkPacket().
  uint8_t checkContent(mesh::Packet* pkt, uint8_t type, const mesh::GroupChannel& channel,
                       const uint8_t* data, size_t len, const RegionEntry* region,
                       uint32_t est_air_ms = 0);

  // supply keyed-channel candidates for core's group decryption
  int searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel dest[], int max_matches);

  // advert rate limiter (state lives in AdvertRateLimiter)
  void setAdvertRatelimit(uint16_t hours) { limiter.setHours(hours); markDirty(); }
  uint16_t getAdvertRatelimit() const { return limiter.getHours(); }
  void clearAdvertCache() { limiter.clearCache(); }
  int getAdvertCacheCount() const { return limiter.getCacheCount(); }
  // remote CLI scripts (state lives in CliScriptRunner): thin forwards, like
  // the limiter's — no duplicated logic. runCliScripts returns whether a
  // script was drained, so the caller can loop until the queue is empty.
  bool runCliScripts(CliExecFn fn, void* ctx) { return cli_scripts.run(fn, ctx); }
  CliScriptRunner& getScripts() { return cli_scripts; }
  // Any rule or channel mutation: the config is changing, so a verdict stashed
  // under the old rules must not be honoured afterwards. markDirty() is the one
  // place every mutation already passes through.
  void markDirty() { clearContentVerdict(); save_flag.markDirty(); }
  uint32_t getLimiterDrops() const { return limiter.getDrops(); }
  uint32_t getBudgetAborts() const { return budget_aborts; }
  uint64_t getAirSavedMs() const { return air_saved_ms; }   // airtime not relayed (drops)
  uint64_t getAirEvaluatedMs() const { return air_evaluated_ms; }
  unsigned getAirSavedPercent() const {
    return air_evaluated_ms ? (air_saved_ms * 100 + air_evaluated_ms / 2) / air_evaluated_ms : 0;
  }
  void resetStats();

  // persistence
  void load(FILESYSTEM* fs);
  void save(FILESYSTEM* fs);

private:
  // state of a fresh node, and the baseline load() resets to before reading
  void resetToDefaults();
  // Airtime telemetry, both RAM-only. `billEvaluated` counts a packet that was
  // looked at exactly once per packet — checkPacket() bills every packet it
  // scans, except one checkContent() already dropped and billed there.
  // `billSaved` counts airtime a drop or a limiter drop will not spend.
  void billEvaluated(uint32_t est_air_ms) { air_evaluated_ms += est_air_ms; }
  void billSaved(uint32_t est_air_ms) { air_saved_ms += est_air_ms; }
  bool regexMatches(const char* pattern, const char* subject);
  bool channelMatchesStore(const FilterRule* r, const mesh::GroupChannel& channel) const;
  // match gates + commit, shared by checkPacket()/checkContent(): run the prob
  // roll and the throttle gate; when the rule decides, record the hit, bill
  // the saved airtime, and store its action in `out`. Returns false when a
  // gate slips the packet past the rule (evaluation continues with the next
  // rule, exactly as on a failed predicate).
  bool decideMatch(FilterRule* r, PacketHashCache& pkt_hash, uint64_t now_millis,
                   uint32_t est_air_ms, uint8_t& out);
};

// CLI command handler: invoke with the command after "filter" (prefix removed).
// `regions` resolves region names for the region= predicate.
// Replies must fit the 160-byte CLI reply buffer.
void filterCLI(FilterRules& filter, const char* command, char* reply, RegionMap* regions);

#endif // _PACKET_FILTER_H