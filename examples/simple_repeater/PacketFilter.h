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
// (see probDecides/throttleDecides). Actions: drop (enforce) and forward
// (terminal: stop the list, count the hit, let the packet through).
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

// actions
#define FILTER_ACT_ALLOW     0
#define FILTER_ACT_DROP      1
#define FILTER_ACT_FORWARD   2   // value 2 = the old logonly byte; configs
                                 // load identically across the rename

// payload-type mask bits (indexed by mesh::Packet payload type, 4 bits)
#define FILTER_TYPE_ADVERT  (1 << PAYLOAD_TYPE_ADVERT)
#define FILTER_TYPE_GRP_TXT (1 << PAYLOAD_TYPE_GRP_TXT)
#define FILTER_TYPE_GRP_DATA (1 << PAYLOAD_TYPE_GRP_DATA)

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

#if FILTER_MAX_CHANNELS > 16
  #error "FILTER_MAX_CHANNELS > 16 needs a wider FilterRule::chan_mask"
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
  uint8_t  action;        // FILTER_ACT_DROP | FILTER_ACT_FORWARD
  uint8_t  type_mask;     // bits by payload type (see FILTER_TYPE_*); 0 = any
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
  uint16_t chan_mask;      // keyed channels by stored name; bitmask over the
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
                           // v6 record growth: the v4/v5 record ends exactly at
                           // offsetof(throttle) (byte-identical prefix)
  // the 2 bytes before hits are RESERVED tail padding (like prob's old v4
  // padding): written raw and not format-guaranteed in v6 files, so a future
  // field placed there must be forced to 0 when read from a v6-or-older record
  uint32_t hits;           // match counter (forward/drop telemetry + validation)
  uint32_t air_ms;         // estimated on-air time (ms) billed by this rule's
                           // DROP decisions; RAM-only stat, after `hits` so it
                           // is never persisted (persist ends at offsetof(hits))
  // RAM-only throttle state — never persisted (persist ends at offsetof(hits));
  // travels with the rule on move/del, like hits
  uint32_t throttle_last_ms; // millis() stamp of the last within-budget pass
  uint32_t throttle_pass;    // within-budget passes (slipped past the rule)
  bool     throttle_seen;    // a pass has been stamped (first match = free pass)
};

// v4/v5 records must stay byte-identical prefixes of a v6 record: throttle
// starts exactly where the old record ended (offsetof(hits) under v5 layout),
// and the 2 bytes before hits are reserved tail padding (throttle is u16). A
// build-time override of FILTER_REGION_LIST_LEN that breaks either invariant
// would silently corrupt config upgrades; refuse to build.
static_assert(offsetof(FilterRule, throttle) ==
                  ((offsetof(FilterRule, regions) + FILTER_REGION_LIST_LEN +
                    alignof(uint32_t) - 1) & ~(alignof(uint32_t) - 1)),
              "FilterRule::throttle must start where the v4/v5 record ended");
static_assert(offsetof(FilterRule, hits) == offsetof(FilterRule, throttle) + 4,
              "u16 throttle + 2 reserved bytes must fill the gap before hits");
// FILTER_PATH_HASH_SLOTS sizes FilterRule::path, so it moves every field after
// it and a config saved by one build would not load in another. Name the knob
// that was raised here, rather than failing on the record invariant it broke.
static_assert(FILTER_PATH_HASH_SLOTS == 4,
              "FilterRule::path size is in the persisted record: this needs a "
              "FILTER_CFG_VERSION bump and migration code in load()");

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

  // Single-pass evaluation of the ENTIRE rule list on a decrypted group
  // payload (packet-level predicates via ruleMatchesPacket, then chan keyed /
  // sender / text), in listed order; first enabled match decides (first match
  // wins) and its verdict — allow, forward or drop — is stashed for
  // checkPacket(). Caller drops the packet if this returns FILTER_ACT_DROP.
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
  void markDirty() { save_flag.markDirty(); }
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
  bool decideMatch(FilterRule* r, PacketHashCache& pkt_hash, uint32_t now_millis,
                   uint32_t est_air_ms, uint8_t& out);
};

// CLI command handler: invoke with the command after "filter" (prefix removed).
// `regions` resolves region names for the region= predicate.
// Replies must fit the 160-byte CLI reply buffer.
void filterCLI(FilterRules& filter, const char* command, char* reply, RegionMap* regions);

#endif // _PACKET_FILTER_H