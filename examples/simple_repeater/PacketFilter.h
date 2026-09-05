// PacketFilter.h — remote-configurable packet drop rules for the simple_repeater
// firmware. Personal fork feature; all fork-owned code lives in this file set
// (PacketFilter.h/.cpp, PacketFilterConfig.h, TinyRegex.h/.cpp) plus minimal
// hook lines in MyMesh.h/MyMesh.cpp. No changes to MeshCore core sources.
//
// A rule is a conjunction of optional predicates; a packet is DROPPED if any
// enabled rule whose predicates all match is hit (first match wins).
// Unspecified predicate = wildcard. Actions: drop (enforce) and logonly
// (shadow mode: count hits, still forward).
//
// Rules with content predicates (keyed channel / sender / text) are evaluated
// in onGroupDataRecv() on the decrypted group payload — keyed channel identity
// is proven by successful MAC verification. All other predicates are evaluated
// packet-level in allowPacketForward().
//
// CLI: "filter ..." commands, handled by filterCLI() (see PacketFilter.cpp).

#ifndef _PACKET_FILTER_H
#define _PACKET_FILTER_H

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/IdentityStore.h>   // FILESYSTEM typedef
#include "PacketFilterConfig.h"
#include "TinyRegex.h"

// actions
#define FILTER_ACT_ALLOW     0
#define FILTER_ACT_DROP      1
#define FILTER_ACT_LOG_ONLY  2

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
// flags == 0 -> predicate unset (wildcard)

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
  uint8_t  action;        // FILTER_ACT_DROP | FILTER_ACT_LOG_ONLY
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
  uint32_t hits;           // match counter (logonly telemetry + validation)
};

struct AdvertSeenEntry {      // RAM-only; cleared on reboot
  uint8_t  pub_key_prefix[4]; // 4 pubkey bytes sampled at fixed offsets (see
                              // advertRatelimitDrop); collision odds ~0.001%
                              // per 256 distinct nodes, vanity-robust; worst
                              // case is one falsely suppressed advert/window
  uint32_t first_seen_millis; // by this repeater's own monotonic clock
};

class FilterRules {
  FilterRule rules[FILTER_MAX_RULES];
  int num_rules;
  FilterChannel channels[FILTER_MAX_CHANNELS];
  int num_channels;
  AdvertSeenEntry advert_cache[FILTER_ADVERT_CACHE_SIZE];
  int advert_cache_count;     // number of used entries (0..FILTER_ADVERT_CACHE_SIZE)
  int advert_cache_head;      // ring head (oldest entry) once the cache is full
  uint16_t ratelimit_hours;   // per-node advert repeat window; 0 = off
  uint32_t limiter_drops;     // adverts dropped by the rate limiter
  uint32_t budget_aborts;     // regex evaluations aborted on step-budget exhaustion
  bool enabled;
  bool dirty;                 // needs save
  unsigned long dirty_since;
  FILESYSTEM* _fs;

public:
  FilterRules();

  void begin(FILESYSTEM* fs);      // load persisted config, pre-provision Public channel
  void loop(FILESYSTEM* fs);       // lazy dirty-flag save (same pattern as ClientACL)
  bool isDirty() const { return dirty; }

  bool isEnabled() const { return enabled; }
  void setEnabled(bool on);

  // rule management
  int getNumRules() const { return num_rules; }
  FilterRule* getRule(int idx) { return &rules[idx]; }
  FilterRule* addRule();           // returns NULL if full
  void delRule(int idx);
  void clearRules();

  // keyed channel store
  int getNumChannels() const { return num_channels; }
  FilterChannel* getChannel(int idx) { return &channels[idx]; }
  FilterChannel* findChannel(const char* name);
  // addChannel: psk_base64 required for non-'#' names; NULL/empty for '#name'
  // derives secret = sha256(name)[0..15] per the companion protocol.
  FilterChannel* addChannel(const char* name, const char* psk_base64);
  void delChannel(int idx);

  // match packet-level predicates (type/route/hops/len/snr/chanhash/path/hashsize)
  // and apply the advert rate limiter. Returns FILTER_ACT_*.
  uint8_t checkPacket(const mesh::Packet* pkt, uint32_t now_millis);

  // evaluate content rules on a decrypted group payload (chan keyed, sender, text);
  // caller drops the packet if this returns FILTER_ACT_DROP.
  uint8_t checkContent(mesh::Packet* pkt, uint8_t type, const mesh::GroupChannel& channel,
                       const uint8_t* data, size_t len);

  // supply keyed-channel candidates for core's group decryption
  int searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel dest[], int max_matches);

  // advert rate limiter
  void setAdvertRatelimit(uint16_t hours);
  uint16_t getAdvertRatelimit() const { return ratelimit_hours; }
  void clearAdvertCache();
  int getAdvertCacheCount() const { return advert_cache_count; }
  void markDirty() { dirty = true; dirty_since = millis(); }
  uint32_t getLimiterDrops() const { return limiter_drops; }
  uint32_t getBudgetAborts() const { return budget_aborts; }
  void resetStats();

  // persistence
  void load(FILESYSTEM* fs);
  void save(FILESYSTEM* fs);

private:
  // per-node advert repeat window; returns true if the advert must be dropped
  bool advertRatelimitDrop(const mesh::Packet* pkt, uint32_t now_millis);
  bool regexMatches(const char* pattern, const char* subject);
  bool channelMatchesStore(const FilterRule* r, const mesh::GroupChannel& channel) const;
};

// CLI command handler: invoke with the command after "filter" (prefix removed).
// Replies must fit the 160-byte CLI reply buffer.
void filterCLI(FilterRules& filter, const char* command, char* reply);

#endif // _PACKET_FILTER_H