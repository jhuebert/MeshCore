// PacketFilter.cpp — remote-configurable packet drop rules for simple_repeater.
// See PacketFilter.h for the rule model.

#include "PacketFilter.h"
#include "CliUtil.h"
#include "AdvertRateLimiter.h"
#include <inttypes.h>
#include <helpers/TxtDataHelpers.h>

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Hex decoder for PSK entry (16/32-byte keys); PSKs are shared/entered as hex.
// `capacity` is the caller's output buffer: an over-long or odd-length input is
// refused before any byte is written, so a rejected key can never partially
// overwrite a live slot.
static int filterDecodeHex(const char* in, size_t in_len, uint8_t* out, size_t capacity) {
  if ((in_len & 1) != 0 || in_len / 2 > capacity) return 0;
  for (size_t i = 0; i < in_len; i += 2) {
    int hi = hexVal(in[i]), lo = hexVal(in[i + 1]);
    if (hi < 0 || lo < 0) return 0;
    out[i / 2] = (uint8_t)((hi << 4) | lo);
  }
  return (int)(in_len / 2);
}

// The well-known Public channel PSK (16 bytes); its sha256()[0] air hash is 0x11.
#define FILTER_PUBLIC_PSK_HEX  "8b3387e9c5cdea6ac9e5edbaa115cd72"
#define FILTER_CFG_FILE        "/filter_cfg"
#define FILTER_CFG_VERSION     6   // v6: throttle= rate gate (record grows 4 B:
                                   // u16 throttle + 2 reserved pad bytes); the
                                   // v4/v5 record is a byte-identical prefix
                                   // ending at offsetof(throttle) and reads back
                                   // with throttle == 0 (= no limit)
// Rule and channel record sizes live in PacketFilter.h, frozen as literals
// alongside the static_asserts that tie them to the live struct.
#define FILTER_RULE_PERSIST_BYTES  FILTER_RULE_V6_BYTES   // config fields only; stats excluded

#define FILTER_ADVERT_HOURS_MAX 720          // ~30 days; millis() wraps at ~49.7 days

// ---------------------------------------------------------------- initialization

// The state a node starts in, and the state load() must return to before it
// reads the file: empty rule and channel stores, filter on, no RAM-only stats.
// One definition, so construction and reload cannot drift apart — a field added
// here is reset by both.
void FilterRules::resetToDefaults() {
  memset(rules, 0, sizeof(rules));   // unpersisted tail bytes (padding, stats) stay deterministic
  memset(channels, 0, sizeof(channels));
  num_rules = 0;
  num_channels = 0;
  limiter.reset();
  budget_aborts = 0;
  air_saved_ms = 0;
  air_evaluated_ms = 0;
  content_verdict.pkt = NULL;
  enabled = true;
}

FilterRules::FilterRules() {
  resetToDefaults();
}

void FilterRules::begin(FILESYSTEM* fs) {
  bool fresh = !fs->exists(FILTER_CFG_FILE);
  load(fs);

  // pre-provision the well-known Public channel on a fresh node
  if (fresh && findChannel("Public") == NULL) {
    addChannel("Public", FILTER_PUBLIC_PSK_HEX);
  }
}

void FilterRules::loop(FILESYSTEM* fs) {
  if (save_flag.due()) save(fs);
}

// ---------------------------------------------------------------- enable / stats

void FilterRules::setEnabled(bool on) {
  if (enabled != on) {
    enabled = on;
    markDirty();
  }
}

void FilterRules::resetStats() {
  limiter.resetDrops();
  budget_aborts = 0;
  air_saved_ms = 0;
  air_evaluated_ms = 0;
  for (int i = 0; i < num_rules; i++) {
    rules[i].hits = 0;
    rules[i].air_ms = 0;
    rules[i].throttle_pass = 0;   // rate state kept: resetting stats never grants a free pass
  }
}

// ---------------------------------------------------------------- rule management

FilterRule* FilterRules::addRule() {
  if (num_rules >= FILTER_MAX_RULES) return NULL;
  FilterRule* r = &rules[num_rules++];
  memset(r, 0, sizeof(FilterRule));
  r->enabled = true;
  r->action = FILTER_ACT_DROP;
  markDirty();
  return r;
}

void FilterRules::delRule(int idx) {
  if (idx < 0 || idx >= num_rules) return;
  memmove(&rules[idx], &rules[idx + 1], (num_rules - idx - 1) * sizeof(FilterRule));
  memset(&rules[num_rules - 1], 0, sizeof(FilterRule));
  num_rules--;
  markDirty();
}

void FilterRules::clearRules() {
  memset(rules, 0, sizeof(rules));
  num_rules = 0;
  markDirty();
}

void FilterRules::moveRule(int from, int to) {
  if (from < 0 || from >= num_rules || to < 0 || to >= num_rules || from == to) return;
  FilterRule tmp = rules[from];
  if (from < to) {
    memmove(&rules[from], &rules[from + 1], (to - from) * sizeof(FilterRule));
  } else {
    memmove(&rules[to + 1], &rules[to], (from - to) * sizeof(FilterRule));
  }
  rules[to] = tmp;   // hits travel with the rule
  markDirty();
}

// ---------------------------------------------------------------- channel store

FilterChannel* FilterRules::findChannel(const char* name) {
  int i = indexOfChannel(name);
  return i < 0 ? NULL : &channels[i];
}

int FilterRules::indexOfChannel(const char* name) const {
  for (int i = 0; i < num_channels; i++) {
    if (strcmp(channels[i].name, name) == 0) return i;   // every slot below the count is named
  }
  return -1;
}

// A channel name has to survive a round trip through a rule's chan= list,
// which is comma-separated and made of `key=value` tokens. ',', '=' and '"'
// therefore cannot be represented there, and a control character (a NUL above
// all) would truncate the stored name. Ordinary spaces are fine: the value is
// quoted where it is printed. Names already in the store are never re-validated
// — an old config stays loadable and visible even if it predates this rule.
static bool validChannelName(const char* name) {
  for (const unsigned char* c = (const unsigned char*)name; *c; c++) {
    if (*c < 0x20 || *c == 0x7f) return false;
    if (*c == ',' || *c == '=' || *c == '"') return false;
  }
  return true;
}

FilterChannel* FilterRules::addChannel(const char* name, const char* psk_hex) {
  if (num_channels >= FILTER_MAX_CHANNELS) return NULL;
  if (name[0] == 0 || strlen(name) >= FILTER_CHAN_NAME_LEN) return NULL;
  if (findChannel(name) != NULL) return NULL;   // already in the store
  if (!validChannelName(name)) return NULL;

  // build the whole entry off to the side: a rejected key must leave the next
  // live slot, and every existing channel, byte-for-byte unchanged
  FilterChannel candidate;
  memset(&candidate, 0, sizeof(candidate));

  if (psk_hex == NULL || psk_hex[0] == 0) {
    if (name[0] != '#') return NULL;   // psk required for non-hash channels
    // hashtag channels are public-by-construction: secret = sha256(name)[0..15]
    // (per the companion protocol; see plan §2.11)
    uint8_t digest[32];
    mesh::Utils::sha256(digest, sizeof(digest), (const uint8_t*)name, strlen(name));
    memcpy(candidate.secret, digest, 16);
    candidate.secret_len = 16;
  } else {
    // exactly 16 or 32 bytes of hex; anything else is refused before decoding
    size_t hex_len = strlen(psk_hex);
    if (hex_len != 32 && hex_len != 64) return NULL;
    int len = filterDecodeHex(psk_hex, hex_len, candidate.secret, sizeof(candidate.secret));
    if (len != 16 && len != 32) return NULL;
    candidate.secret_len = len;
  }

  mesh::Utils::sha256(&candidate.hash, sizeof(candidate.hash), candidate.secret, candidate.secret_len);
  StrHelper::strzcpy(candidate.name, name, FILTER_CHAN_NAME_LEN);
  channels[num_channels] = candidate;   // commit only once everything validated
  num_channels++;
  markDirty();
  return &channels[num_channels - 1];
}

void FilterRules::delChannel(int idx) {
  if (idx < 0 || idx >= num_channels) return;
  memmove(&channels[idx], &channels[idx + 1], (num_channels - idx - 1) * sizeof(FilterChannel));
  memset(&channels[num_channels - 1], 0, sizeof(FilterChannel));
  num_channels--;
  // channels above idx shift down one slot: drop the deleted channel's bit and
  // remap rule masks so they keep pointing at the same channels
  for (int i = 0; i < num_rules; i++) {
    FilterRule* r = &rules[i];
    uint16_t lo = r->chan_mask & (uint16_t)((1 << idx) - 1);
    uint16_t hi = (uint16_t)(r->chan_mask >> (idx + 1)) << idx;
    r->chan_mask = lo | hi;
  }
  markDirty();
}

int FilterRules::searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel dest[], int max_matches) {
  if (!enabled) return 0;   // filter off -> behave like stock (no channel DB)
  int n = 0;
  for (int i = 0; i < num_channels && n < max_matches; i++) {
    if (channels[i].name[0] == 0) continue;   // null-key guard (same as BaseChatMesh)
    if (channels[i].hash != hash[0]) continue;
    // Aliases of one key share a tag; handing core the same secret twice would
    // burn one of its few candidate slots and could hide a channel that would
    // actually decrypt. Compare the padded secrets, not the names.
    bool dup = false;
    for (int j = 0; j < n; j++) {
      if (memcmp(dest[j].secret, channels[i].secret, sizeof(channels[i].secret)) == 0) { dup = true; break; }
    }
    if (dup) continue;
    dest[n].hash[0] = channels[i].hash;
    memcpy(dest[n].secret, channels[i].secret, sizeof(dest[n].secret));
    n++;
  }
  return n;
}

// Distinct secrets behind one on-air tag: aliases of a single key cost core
// nothing, and this is what tells an admin a tag is carrying more keys than
// core will ever try.
static int distinctKeysOnHash(FilterRules& filter, uint8_t hash) {
  int n = 0;
  for (int i = 0; i < filter.getNumChannels(); i++) {
    auto c = filter.getChannel(i);
    if (c->hash != hash) continue;
    bool seen = false;
    for (int j = 0; j < i; j++) {
      auto o = filter.getChannel(j);
      if (o->hash == hash && memcmp(o->secret, c->secret, sizeof(c->secret)) == 0) { seen = true; break; }
    }
    if (!seen) n++;
  }
  return n;
}

// FNV-1a over the rule's stored predicate fields. Two jobs, deliberately the
// same bytes: the short digest `filter list` shows per rule, and the per-rule
// salt of the prob roll (see probDecides()).
//
// Its range is the whole persisted record — &action up to &hits — which
// includes reserved tail padding that is not format-guaranteed. So editing a
// rule, or reading it under firmware whose record differs, changes this digest
// and therefore re-rolls which packets a prob= rule decides. That is intended:
// prob is dosing, not a stable contract (see FILTER.md). Anything that needs a
// stable rule identity wants a different function, not this one.
static uint32_t ruleDigest(const FilterRule* r) {
  uint32_t h = 2166136261u;
  const uint8_t* p = (const uint8_t*)&r->action;
  const uint8_t* end = (const uint8_t*)&r->hits;
  for (; p < end; p++) {
    h ^= *p;
    h *= 16777619u;
  }
  return h;
}

// Deterministic per (packet, rule) match-probability roll: FNV-1a over the
// packet hash salted with the rule digest. rand() is never seeded anywhere, so
// it would replay the same sequence every boot; this roll is idempotent under
// the stash re-scan (same packet -> same verdict) and exact-testable.
static bool probDecides(const FilterRule* r, const uint8_t* pkt_hash) {
  if (r->prob == 0 || r->prob >= 100) return true;   // unset = always (100 %)
  uint32_t x = ruleDigest(r) ^ 2166136261u;   // per-rule salt
  for (int i = 0; i < MAX_HASH_SIZE; i++) { x ^= pkt_hash[i]; x *= 16777619u; }
  return (x % 100) < r->prob;
}

// Rate gate for throttle=N rules: the stateful counterpart of probDecides().
// Within budget the packet slips past the rule — the clock stamps, the pass
// counts, and evaluation continues with the next rule exactly as on a failed
// prob roll. Over rate (or throttle unset) the rule decides (fires): its
// action applies. Over-rate drops do NOT extend the window, so a matching
// stream sustains exactly one pass per N seconds however hard it is pushed
// (the advert limiter's model, per rule).
static bool throttleDecides(FilterRule* r, uint32_t now_millis) {
  if (r->throttle == 0) return true;   // unset = no limit: every match decides
  if (r->throttle_seen &&
      now_millis - r->throttle_last_ms < (uint32_t)r->throttle * 1000UL) {
    return true;                       // over rate: decide (action applies)
  }
  r->throttle_last_ms = now_millis;    // within budget: slips past the rule
  r->throttle_seen = true;
  r->throttle_pass++;
  return false;
}

// Shared tail of checkPacket()/checkContent(): gates first (prob roll, then
// throttle), then commit — count the hit and bill the saved airtime for DROP
// decisions. `out` receives the rule's action.
bool FilterRules::decideMatch(FilterRule* r, PacketHashCache& pkt_hash, uint32_t now_millis,
                              uint32_t est_air_ms, uint8_t& out) {
  // probDecides() would call pkt_hash.get() — a SHA-256 over the packet — even
  // for the common unset/100 rule that returns true immediately. Gate it at the
  // caller so only a rule that can actually fail the roll pays for the hash.
  // Probability still runs before throttle.
  if (r->prob > 0 && r->prob < 100 && !probDecides(r, pkt_hash.get())) return false;   // failed roll: fall through as if not matched
  if (!throttleDecides(r, now_millis)) return false;   // within budget: slips past, like a failed roll
  r->hits++;
  if (r->action == FILTER_ACT_DROP) {   // bill the airtime this drop saves
    r->air_ms += est_air_ms;
    billSaved(est_air_ms);
  }
  out = r->action;
  return true;
}

// ---------------------------------------------------------------- matching

static bool intervalMatches(const Interval& iv, int32_t v) {
  if (iv.flags == 0) return true;   // unset predicate = wildcard
  if (!(iv.flags & FILTER_IV_LO_ANY)) {
    int32_t lo = (int32_t)(int16_t)iv.lo;
    if (v < lo || (v == lo && !(iv.flags & FILTER_IV_LO_INC))) return false;
  }
  if (!(iv.flags & FILTER_IV_HI_ANY)) {
    int32_t hi = (int32_t)(int16_t)iv.hi;
    if (v > hi || (v == hi && !(iv.flags & FILTER_IV_HI_INC))) return false;
  }
  return true;
}

// Path chain predicate: sliding window of adjacent hash entries over pkt->path.
// Each entry is a prefix compare of min(rule_len, path_hash_size) bytes.
// Binary per-entry compare, NOT hex-string matching (odd-nibble alignment and
// variable entry sizes make string matching incorrect — plan §4.2a).
static bool pathMatches(const FilterRule* r, const mesh::Packet* pkt) {
  // A recorded path is the flood relay history. A routed-direct packet's path is
  // an itinerary between two endpoints, and a TRACE path collects SNRs rather
  // than repeater IDs — neither is relay history, so neither may satisfy path=.
  if (!pkt->isRouteFlood()) return false;
  uint8_t hsz = pkt->getPathHashSize();
  uint8_t n = pkt->getPathHashCount();
  if (n == 0) return false;          // 0-hop traffic never matches
  if (r->path.count > n) return false;

  int start_min = 0, start_max = n - r->path.count;
  switch (r->path.pos) {
    case FILTER_PATH_FIRST: start_min = 0; start_max = 0; break;
    case FILTER_PATH_LAST:  start_min = n - r->path.count; start_max = start_min; break;
    case FILTER_PATH_FIRST | FILTER_PATH_LAST:   // ^A$ / ^A>B$: whole path only
      if (n != r->path.count) return false;
      break;   // start_min == start_max == 0
    default: break;   // FILTER_PATH_ANY: every window position
  }
  for (int w = start_min; w <= start_max; w++) {
    bool ok = true;
    for (int e = 0; e < r->path.count; e++) {
      uint8_t cmp = r->path.len[e] < hsz ? r->path.len[e] : hsz;
      if (memcmp(&pkt->path[(w + e) * hsz], r->path.bytes[e], cmp) != 0) { ok = false; break; }
    }
    if (ok) return true;
  }
  return false;
}

// Which content predicates a rule carries. Content predicates can only be
// decided on decrypted group data, so the packet-level phase skips every rule
// with a non-zero set, and checkContent() switches on that same value: the two
// phases cannot disagree about what is deferred.
enum FilterContent : uint8_t {
  FILTER_CONTENT_NONE   = 0,
  FILTER_CONTENT_CHAN   = 1 << 0,   // chan=   : the MAC-proven delivery channel
  FILTER_CONTENT_SENDER = 1 << 1,   // sender= : regex over "<sender>:"
  FILTER_CONTENT_TEXT   = 1 << 2,   // text=   : regex over the message text
};

static uint8_t contentPredicates(const FilterRule* r) {
  uint8_t p = FILTER_CONTENT_NONE;
  if (r->chan_flags & FILTER_CHANFLG_MASK_SET) p |= FILTER_CONTENT_CHAN;
  if (r->sender[0] != 0) p |= FILTER_CONTENT_SENDER;
  if (r->text[0] != 0) p |= FILTER_CONTENT_TEXT;
  return p;
}

// Rules with content predicates are deferred to checkContent() (decrypted data).
static bool ruleIsDeferred(const FilterRule* r) {
  return contentPredicates(r) != FILTER_CONTENT_NONE;
}

// exact match of `name` against one comma token of `list`
static bool regionListContains(const char* list, const char* name) {
  size_t nlen = strlen(name);
  while (*list) {
    const char* comma = strchr(list, ',');
    size_t tlen = comma ? (size_t)(comma - list) : strlen(list);
    if (tlen == nlen && memcmp(list, name, tlen) == 0) return true;
    if (!comma) break;
    list = comma + 1;
  }
  return false;
}

// Evaluate the packet-level predicates of one rule (shared by both phases).
static bool ruleMatchesPacket(const FilterRule* r, const mesh::Packet* pkt, uint8_t payload_type,
                              const RegionEntry* region) {
  if (r->type_mask != 0 && !(r->type_mask & (1 << payload_type))) return false;

  if (r->route_mask != 0) {
    uint8_t bit = pkt->isRouteFlood() ? FILTER_ROUTE_FLOOD : FILTER_ROUTE_DIRECT;
    if (!(r->route_mask & bit)) return false;
  }

  if (r->regions[0]) {               // region= predicate: comma list of canonical names
    if (region == NULL) return false;   // direct-routed (or unknown code): no region at all
    const char* want = region->isWildcard() ? "unscoped" : region->name;
    if (!regionListContains(r->regions, want)) return false;
  }

  if (r->hops.flags != 0) {          // hop count is meaningful for flood path only
    if (!pkt->isRouteFlood()) return false;
    if (!intervalMatches(r->hops, pkt->getPathHashCount())) return false;
  }

  if (r->len.flags != 0 && !intervalMatches(r->len, pkt->payload_len)) return false;

  if (r->snr.flags != 0 && !intervalMatches(r->snr, (int16_t)lroundf(pkt->getSNR() * 4.0f))) return false;

  if (r->hash_size_mask != 0 &&
      !(r->hash_size_mask & (1 << (pkt->getPathHashSize() - 1)))) return false;

  if (r->chan_flags & FILTER_CHANFLG_HASH_SET) {   // bare 1-byte air hash (collision-prone)
    if (payload_type != PAYLOAD_TYPE_GRP_TXT && payload_type != PAYLOAD_TYPE_GRP_DATA) return false;
    if (pkt->payload_len < 1 || pkt->payload[0] != r->chan_hash) return false;
  }

  if (r->path.count > 0 && !pathMatches(r, pkt)) return false;

  return true;
}

uint8_t FilterRules::checkPacket(const mesh::Packet* pkt, uint32_t now_millis, const RegionEntry* region,
                                 uint32_t est_air_ms) {
  if (!enabled) return FILTER_ACT_ALLOW;

  // one hash for this packet, shared by the stash guard and any prob roll below
  PacketHashCache pkt_hash(pkt);

  // a verdict stashed by checkContent() for this exact packet is final:
  // return it without rescanning (no double-counted hits, no reordering).
  // Stale stash (different buffer, or the pool re-used it with new content,
  // caught by the content-hash tag) is cleared and a normal packet-level scan
  // runs.
  if (content_verdict.pkt == pkt) {
    content_verdict.pkt = NULL;   // consume once, whatever the tag says
    if (memcmp(pkt_hash.get(), content_verdict.hash, MAX_HASH_SIZE) == 0) {
      if (content_verdict.verdict != FILTER_ACT_DROP) billEvaluated(est_air_ms);
      return content_verdict.verdict;
    }
  }

  billEvaluated(est_air_ms);
  uint8_t payload_type = pkt->getPayloadType();
  uint8_t action = FILTER_ACT_ALLOW;
  for (int i = 0; i < num_rules; i++) {
    FilterRule* r = &rules[i];
    if (!r->enabled || ruleIsDeferred(r)) continue;   // deferred rules decide on decrypted content
    if (!ruleMatchesPacket(r, pkt, payload_type, region)) continue;
    if (decideMatch(r, pkt_hash, now_millis, est_air_ms, action)) break;   // first match wins
  }
  if (action == FILTER_ACT_DROP) return FILTER_ACT_DROP;

  // limiter runs unless the rule list already dropped; forward adverts too
  if (payload_type == PAYLOAD_TYPE_ADVERT && pkt->isRouteFlood() && limiter.drop(pkt, now_millis)) {
    billSaved(est_air_ms);   // a limiter drop saves the same airtime
    return FILTER_ACT_DROP;
  }

  return action;
}

// ---------------------------------------------------------------- content rules

// Split a decrypted group-text body into "<sender>: <text>". Content is
// sender-controlled, so parse defensively and bounded by len (never strlen()).
// The decrypted block is zero-padded to the packet buffer, so the visible field
// ends at the first NUL — parsing past it would invent a colon (and an empty
// sender) out of padding. `has_text`/`has_sender` report which fields were
// really present, so a caller can tell "no sender field" from "empty text".
static void parseGroupText(const uint8_t* data, size_t len, char* sender, size_t sender_sz,
                           char* text, size_t text_sz, bool* has_text, bool* has_sender) {
  sender[0] = 0;
  text[0] = 0;
  *has_text = false;
  *has_sender = false;
  if (len < 5) return;   // too short for ts(4) + txt_type(1)

  const uint8_t* p = data + 5;
  // bounded to the first NUL within the decrypted body: everything after it is
  // padding, not content
  size_t n = 0;
  const uint8_t* term = (const uint8_t*)memchr(p, 0, len - 5);
  if (term != NULL) n = (size_t)(term - p);
  else n = len - 5;

  const uint8_t* colon = NULL;
  for (size_t i = 0; i < n; i++) {
    if (p[i] == ':') { colon = &p[i]; break; }
  }

  if (colon == NULL) {   // no sender extractable; whole remainder is text
    *has_text = true;
    size_t cpy = n < text_sz - 1 ? n : text_sz - 1;
    memcpy(text, p, cpy);
    text[cpy] = 0;
    return;
  }

  size_t slen = (size_t)(colon - p);
  while (slen > 0 && (p[slen - 1] == ' ' || p[slen - 1] == '\t')) slen--;   // trim trailing spaces/tabs
  size_t cpy = slen < sender_sz - 1 ? slen : sender_sz - 1;
  memcpy(sender, p, cpy);
  sender[cpy] = 0;
  *has_sender = true;

  const uint8_t* tp = colon + 1;
  size_t tlen = n - (size_t)(tp - p);
  while (tlen > 0 && (*tp == ' ' || *tp == '\t' || *tp == '\r' || *tp == '\n')) { tp++; tlen--; }
  *has_text = true;
  cpy = tlen < text_sz - 1 ? tlen : text_sz - 1;
  memcpy(text, tp, cpy);
  text[cpy] = 0;
}

// A rule's keyed-channel predicate matches if the delivered (MAC-proven) channel
// is one of the store entries named by the rule's bitmask.
bool FilterRules::channelMatchesStore(const FilterRule* r, const mesh::GroupChannel& channel) const {
  for (int i = 0; i < num_channels; i++) {
    if (!(r->chan_mask & (1 << i))) continue;
    if (memcmp(channels[i].secret, channel.secret, sizeof(channel.secret)) == 0) return true;
  }
  return false;
}

bool FilterRules::regexMatches(const char* pattern, const char* subject) {
  bool aborted = false;
  if (patternMatches(pattern, subject, &aborted)) return true;
  if (aborted) budget_aborts++;   // fail-open: worst case is a spam message repeated
  return false;
}

uint8_t FilterRules::checkContent(mesh::Packet* pkt, uint8_t type, const mesh::GroupChannel& channel,
                                  const uint8_t* data, size_t len, const RegionEntry* region,
                                  uint32_t est_air_ms) {
  if (!enabled) return FILTER_ACT_ALLOW;
  if (type != PAYLOAD_TYPE_GRP_TXT && type != PAYLOAD_TYPE_GRP_DATA) return FILTER_ACT_ALLOW;

  // single pass over the WHOLE rule list in listed order: packet-level
  // predicates and content predicates alike — every predicate is computable
  // here, and first enabled match is terminal
  char sender[MAX_PACKET_PAYLOAD + 1];
  char text[MAX_PACKET_PAYLOAD + 1];
  bool has_text = false, has_sender = false;
  bool parsed = (type == PAYLOAD_TYPE_GRP_TXT);
  if (parsed) parseGroupText(data, len, sender, sizeof(sender), text, sizeof(text), &has_text, &has_sender);

  uint8_t verdict = FILTER_ACT_ALLOW;
  PacketHashCache pkt_hash(pkt);
  // one clock reading for the whole scan: the throttle gate stamps state that
  // checkPacket() writes too, and both phases read the same millis() clock
  const uint32_t now = millis();
  for (int i = 0; i < num_rules; i++) {
    FilterRule* r = &rules[i];
    if (!r->enabled) continue;
    if (!ruleMatchesPacket(r, pkt, type, region)) continue;
    uint8_t cp = contentPredicates(r);
    if ((cp & FILTER_CONTENT_CHAN) && !channelMatchesStore(r, channel)) continue;
    if ((cp & FILTER_CONTENT_SENDER) && (!parsed || !has_sender || !regexMatches(r->sender, sender))) continue;
    if ((cp & FILTER_CONTENT_TEXT) && (!parsed || !has_text || !regexMatches(r->text, text))) continue;
    if (decideMatch(r, pkt_hash, now, est_air_ms, verdict)) break;   // first match wins
  }

  // Content drops never reach the forwarding hook; passes are counted in checkPacket().
  if (verdict == FILTER_ACT_DROP) billEvaluated(est_air_ms);

  // stash the verdict (allow included) for checkPacket(); a drop verdict
  // lingers (core marks the packet DoNotRetransmit and never calls
  // checkPacket for it) until the next packet clears it by pointer or
  // content-hash mismatch
  content_verdict.pkt = pkt;
  memcpy(content_verdict.hash, pkt_hash.get(), MAX_HASH_SIZE);   // same hash the roll uses
  content_verdict.verdict = verdict;
  return verdict;
}

// ---------------------------------------------------------------- persistence
// Binary format (version 2): header + config-only rule records + raw channel
// structs. Rule records are the FilterRule struct up to (excluding) `hits` —
// stats are memory-only and never touch the file. All struct members are
// fixed-size arrays/scalars (no pointers) and structs are memset(0) before use,
// so a raw write/read is deterministic on a given platform; the version byte
// guards against layout drift.

// A config file is untrusted input, not a serialized live C++ object: these
// bytes came off a flash filesystem and may be truncated, stale, or hand-made.
// A record is adopted only if every field in it is one this firmware could
// itself have written.
//
// Malformed values REJECT the record rather than being masked or clamped into
// range. Sanitising would silently turn a rule into a *different* rule, and
// because matching is first-match-wins that changes which rule decides a packet
// — quietly losing a predicate is worse than not having the rule at all.
static bool validRule(const FilterRule* r) {
  if (r->action != FILTER_ACT_DROP && r->action != FILTER_ACT_FORWARD) return false;
  const uint8_t type_bits = FILTER_TYPE_ADVERT | FILTER_TYPE_GRP_TXT | FILTER_TYPE_GRP_DATA;
  if (r->type_mask & ~type_bits) return false;
  if (r->route_mask & ~(FILTER_ROUTE_FLOOD | FILTER_ROUTE_DIRECT)) return false;

  // interval predicates: known flag bits, and an endpoint pair that is not
  // inverted (lo > hi can never be satisfied)
  const uint8_t iv_bits = FILTER_IV_LO_INC | FILTER_IV_HI_INC | FILTER_IV_LO_ANY |
                          FILTER_IV_HI_ANY | FILTER_IV_SET;
  const Interval* ivs[3] = { &r->hops, &r->len, &r->snr };
  for (int i = 0; i < 3; i++) {
    if (ivs[i]->flags & ~iv_bits) return false;
    if (!(ivs[i]->flags & (FILTER_IV_LO_ANY | FILTER_IV_LO_INC)) &&
        !(ivs[i]->flags & (FILTER_IV_HI_ANY | FILTER_IV_HI_INC)) &&
        (int32_t)ivs[i]->lo > (int32_t)ivs[i]->hi) return false;
  }

  // path: a chain shorter than its slot count, entries no wider than a hash,
  // and an anchor this firmware knows
  if (r->path.count > FILTER_PATH_HASH_SLOTS) return false;
  if (r->path.pos & ~(FILTER_PATH_FIRST | FILTER_PATH_LAST)) return false;
  for (uint8_t i = 0; i < r->path.count; i++) {
    if (r->path.len[i] == 0 || r->path.len[i] > sizeof(r->path.bytes[i])) return false;
  }
  if (r->hash_size_mask & ~0x0F) return false;            // sizes 1..4

  if (r->chan_flags & ~(FILTER_CHANFLG_MASK_SET | FILTER_CHANFLG_HASH_SET)) return false;
  // A chan_mask naming a channel the file does not store is tolerated here and
  // confined after the channels are known: narrowing a rule's scope can only
  // make it match less, whereas rejecting the whole record would change which
  // rule decides a packet. MASK_SET with an empty mask is legal — a chan= whose
  // last channel was deleted is inert, never a wildcard.

  // every stored string must be NUL-terminated inside its own storage
  if (memchr(r->sender, 0, sizeof(r->sender)) == NULL) return false;
  if (memchr(r->text, 0, sizeof(r->text)) == NULL) return false;
  if (memchr(r->regions, 0, sizeof(r->regions)) == NULL) return false;

  if (r->prob > 100) return false;                       // 0 = unset = 100 %
  return true;
}

// A channel entry needs a name that fits and terminates, and a key length this
// firmware uses. The stored hash is a derived byte, not an input, so any value
// is accepted — but an empty name is not: it would occupy a slot that no rule
// and no `chan del <name>` can ever refer to.
static bool validChannel(const FilterChannel* c) {
  if (c->name[0] == 0) return false;
  if (memchr(c->name, 0, sizeof(c->name)) == NULL) return false;
  if (c->secret_len != 16 && c->secret_len != 32) return false;
  return true;
}

void FilterRules::load(FILESYSTEM* fs) {
  resetToDefaults();   // the file decides everything below; nothing survives from before
  save_flag.reset();   // a reload supersedes any edit still waiting to be written

  if (!fs->exists(FILTER_CFG_FILE)) return;
  File file = fsOpenRead(fs, FILTER_CFG_FILE);
  if (file) {
    uint8_t hdr[5];   // version, enabled, num_rules, num_channels, (spare)
    uint8_t ver;      // version byte; hdr[] is reused for the remaining fields
    // Record sizes are the frozen literals, and the version map names versions
    // outright rather than counting back from the current one.
    if (file.read(hdr, 1) == 1 &&
        (ver = hdr[0], ver >= 3 && ver <= FILTER_CFG_VERSION) &&
        file.read(hdr, 4) == 4) {
      // The header describes the file's own SHAPE, so a bad one means we no
      // longer know where the records are: reject the whole file rather than
      // guess. (A bad *record* is different — sizes are fixed, so the next
      // record is still locatable, which is why that case keeps a prefix.)
      if (hdr[0] > 1 || hdr[1] > FILTER_MAX_RULES || hdr[2] > FILTER_MAX_CHANNELS) {
        file.close();
        return;
      }
      uint8_t nr = hdr[1];
      uint8_t nc = hdr[2];
      size_t rule_bytes = (ver >= 6) ? FILTER_RULE_V6_BYTES
                        : (ver >= 4) ? FILTER_RULE_V4_BYTES
                                     : FILTER_RULE_V3_BYTES;
      uint16_t rl_hours;
      if (file.read((uint8_t*)&rl_hours, 2) == 2) {
        if (rl_hours > FILTER_ADVERT_HOURS_MAX) {   // not a window this firmware writes
          file.close();
          return;
        }
        limiter.setHours(rl_hours);

        // Read into locals and adopt only what validates, so a rejected record
        // cannot leave a half-built object behind for a later rule to read.
        // Records are fixed-size, so the valid PREFIX is what we keep: indices
        // of surviving rules stay put, which the CLI's index semantics depend on.
        FilterRule loaded[FILTER_MAX_RULES];
        memset(loaded, 0, sizeof(loaded));   // RAM-only stats/state stay zero
        int good_rules = 0;
        for (int i = 0; i < nr; i++) {
          uint8_t raw[FILTER_RULE_V6_BYTES];
          if (file.read(raw, rule_bytes) != rule_bytes) break;   // truncated
          // `enabled` is a bool, so a persisted byte it could never hold is
          // unrepresentable in the converted field and cannot be range-checked
          // there — it has to be refused while it is still just bytes.
          if (raw[0] > 1) break;
          memcpy(&loaded[i], raw, rule_bytes);
          if (ver < 5) {
            // pre-v5 record: the prob byte (v4 tail padding) is not format-guaranteed
            loaded[i].prob = 0;
          }
          if (rule_bytes == FILTER_RULE_V3_BYTES) {
            // v3 record: the read drags the old record's trailing padding bytes
            // into regions[0..1], so zero the whole regions field (predicate unset)
            memset(loaded[i].regions, 0, sizeof(loaded[i].regions));
          }
          if (!validRule(&loaded[i])) break;
          good_rules++;
        }

        FilterChannel loaded_ch[FILTER_MAX_CHANNELS];
        memset(loaded_ch, 0, sizeof(loaded_ch));
        int good_chans = 0;
        for (int i = 0; i < nc; i++) {
          if (file.read((uint8_t*)&loaded_ch[i], FILTER_CHAN_PERSIST_BYTES) != FILTER_CHAN_PERSIST_BYTES) break;
          if (!validChannel(&loaded_ch[i])) break;
          good_chans++;
        }

        enabled = hdr[0] != 0;
        if (good_rules > 0) { memcpy(rules, loaded, good_rules * sizeof(FilterRule)); }
        if (good_chans > 0) { memcpy(channels, loaded_ch, good_chans * sizeof(FilterChannel)); }
        num_rules = good_rules;
        num_channels = good_chans;
        // a rule whose chan_mask named a channel we did not adopt is still
        // adopted, but only after the mask is confined to the channels that
        // exist: MASK_SET with no bits is the documented inert rule, never a
        // catch-all (see section 5 of the review)
        for (int i = 0; i < num_rules; i++) {
          uint16_t keep = 0;
          for (int c = 0; c < num_channels; c++) if (rules[i].chan_mask & (1 << c)) keep |= (1 << c);
          rules[i].chan_mask = keep;
        }
      }
    }
    file.close();
  }
}

void FilterRules::save(FILESYSTEM* fs) {
  File file = fsOpenWrite(fs, FILTER_CFG_FILE);
  if (!file) { save_flag.retryLater(); return; }   // wait out another delay before retrying
  uint8_t hdr[5];
  hdr[0] = FILTER_CFG_VERSION;
  hdr[1] = enabled ? 1 : 0;
  hdr[2] = (uint8_t)num_rules;
  hdr[3] = (uint8_t)num_channels;
  hdr[4] = 0;
  bool ok = (file.write(hdr, 5) == 5);
  uint16_t rl_hours = limiter.getHours();
  ok = ok && (file.write((uint8_t*)&rl_hours, 2) == 2);
  for (int i = 0; ok && i < num_rules; i++) {
    ok = (file.write((uint8_t*)&rules[i], FILTER_RULE_PERSIST_BYTES) == FILTER_RULE_PERSIST_BYTES);
  }
  for (int i = 0; ok && i < num_channels; i++) {
    ok = (file.write((uint8_t*)&channels[i], FILTER_CHAN_PERSIST_BYTES) == FILTER_CHAN_PERSIST_BYTES);
  }
  file.close();
  // only once the config is actually on disk: a failed write stays pending, but
  // backs off a full delay instead of retrying on every loop
  if (ok) save_flag.clear();
  else save_flag.retryLater();
}

// ---------------------------------------------------------------- CLI

// Which scale an Interval's endpoints are written in (FilterRule::snr stores
// quarter-dB steps in the same int16 pair hops/len use as plain counts).
enum IvUnit { IV_UNSIGNED,   // hops, len: plain decimal
              IV_SNR_DB };    // snr: signed dB, snapped to the quarter-dB grid

// interval value: hops/len = unsigned decimal; snr = signed dB (snapped to the
// quarter-dB grid, stored as quarter-dB int)
static bool parseIvNum(const char* s, IvUnit unit, uint16_t* out) {
  if (unit == IV_SNR_DB) {
    char* end;
    float f = strtof(s, &end);
    if (end == s || *end != 0) return false;
    int32_t q = (int32_t)lroundf(f * 4.0f);
    if (q < -32768 || q > 32767) return false;
    *out = (uint16_t)(int16_t)q;
  } else {
    long v;
    if (!parseIntRange(s, 0, 32767, &v)) return false;   // endpoints are compared as int16
    *out = (uint16_t)v;
  }
  return true;
}

// "[a,b]" / "(a,*]" / bare value = exact; bracket/paren = inclusive/exclusive.
// IV_SNR_DB: values are signed dB (e.g. "0.0", "-3.25").
static bool parseInterval(const char* tok, Interval& iv, IvUnit unit) {
  memset(&iv, 0, sizeof(iv));
  if (tok[0] == '[' || tok[0] == '(') {
    uint8_t flags = (tok[0] == '[') ? FILTER_IV_LO_INC : 0;
    const char* comma = strchr(tok, ',');
    if (comma == NULL) return false;
    size_t len = strlen(tok);
    const char* hend = tok + len - 1;
    char endc = *hend;
    if (endc != ']' && endc != ')') return false;
    flags |= (endc == ']') ? FILTER_IV_HI_INC : 0;

    char los[24], his[24];
    size_t llen = (size_t)(comma - tok) - 1;
    size_t hlen = (size_t)(hend - (comma + 1));
    if (llen == 0 || llen >= sizeof(los) || hlen == 0 || hlen >= sizeof(his)) return false;
    memcpy(los, tok + 1, llen); los[llen] = 0;
    memcpy(his, comma + 1, hlen); his[hlen] = 0;

    if (strcmp(los, "*") == 0) {
      flags |= FILTER_IV_LO_ANY;
    } else if (!parseIvNum(los, unit, &iv.lo)) {
      return false;
    }
    if (strcmp(his, "*") == 0) {
      flags |= FILTER_IV_HI_ANY;
    } else if (!parseIvNum(his, unit, &iv.hi)) {
      return false;
    }
    if ((flags & FILTER_IV_LO_ANY) && (flags & FILTER_IV_HI_ANY)) {
      return true;   // (*,*) = "any": leave predicate unset
    }
    // both-exclusive (a,b) encodes to flags==0, which reads back as unset:
    // keep the predicate marked set via the sentinel bit
    iv.flags = flags ? flags : FILTER_IV_SET;
    return true;
  }

  // bare value = exact
  if (!parseIvNum(tok, unit, &iv.lo)) return false;
  iv.hi = iv.lo;
  iv.flags = FILTER_IV_LO_INC | FILTER_IV_HI_INC;
  return true;
}

static void formatInterval(const Interval& iv, char* dest, size_t sz, IvUnit unit) {
  char lo[16], hi[16];
  if (iv.flags & FILTER_IV_LO_ANY) strcpy(lo, "*");
  else if (unit == IV_SNR_DB) snprintf(lo, sizeof(lo), "%.2f", (float)(int16_t)iv.lo / 4.0f);
  else snprintf(lo, sizeof(lo), "%u", iv.lo);
  if (iv.flags & FILTER_IV_HI_ANY) strcpy(hi, "*");
  else if (unit == IV_SNR_DB) snprintf(hi, sizeof(hi), "%.2f", (float)(int16_t)iv.hi / 4.0f);
  else snprintf(hi, sizeof(hi), "%u", iv.hi);
  snprintf(dest, sz, "%c%s,%s%c",
           (iv.flags & FILTER_IV_LO_INC) ? '[' : '(',
           lo, hi,
           (iv.flags & FILTER_IV_HI_INC) ? ']' : ')');
}

// hex hash: 2..8 hex chars -> up to `max_bytes` leading bytes of a repeater
// pubkey hash (1 for chanhash, 4 for a path entry). `max_bytes` is the caller's
// storage size: a longer value is refused rather than written past it.
static bool parseHexHash(const char* s, uint8_t* out, size_t max_bytes, uint8_t* out_len) {
  size_t n = strlen(s);
  if (n < 2 || n > 8 || (n & 1) || n / 2 > max_bytes) return false;
  if (!filterDecodeHex(s, n, out, max_bytes)) return false;
  *out_len = (uint8_t)(n / 2);
  return true;
}

// path=^HEX>HEX>...>HEX$  (up to FILTER_PATH_HASH_SLOTS adjacent hashes)
// '^' anchors the first entry, '$' the last; both together mean the rule's
// chain must span the whole path (e.g. path=^10$ matches 1-hop paths only)
static bool parsePath(const char* tok, FilterRule* r) {
  // clear the whole substructure, not just count/pos: replacing a long chain with
  // a short one must leave no stale bytes behind for the rule digest to see
  memset(&r->path, 0, sizeof(r->path));
  r->path.pos = FILTER_PATH_ANY;
  const char* s = tok;
  if (*s == '^') { r->path.pos = FILTER_PATH_FIRST; s++; }

  // worst case: every slot at its 8-hex-char maximum, plus the '>' separators
  char buf[FILTER_PATH_HASH_SLOTS * 9];
  size_t tlen = strlen(s);
  if (tlen == 0 || tlen >= sizeof(buf)) return false;
  memcpy(buf, s, tlen + 1);
  if (buf[tlen - 1] == '$') {
    r->path.pos |= FILTER_PATH_LAST;
    buf[tlen - 1] = 0;
  }

  char* sp = buf;
  char* seg;
  while ((seg = strsep(&sp, ">")) != NULL) {
    if (r->path.count >= FILTER_PATH_HASH_SLOTS) return false;
    uint8_t len;
    if (!parseHexHash(seg, r->path.bytes[r->path.count],
                      sizeof(r->path.bytes[r->path.count]), &len)) return false;
    r->path.len[r->path.count] = len;
    r->path.count++;
  }
  return true;
}

// ---------------------------------------------------------------- filter add

// Store a sender=/text= pattern: reject a value too long for this rule's
// storage (never truncate a regex) or one the pattern wrapper refuses, then
// copy it verbatim. `field` names the pattern in the reply.
static bool setPattern(char* dest, size_t dest_sz, const char* pattern, const char* field,
                       char* reply) {
  // zeroed: a too-long pattern short-circuits patternValid(), so why stays
  // empty and the reply falls back to the length wording below
  char why[PATTERN_ERR_MAX] = {0};
  bool ok = strlen(pattern) < dest_sz && patternValid(pattern, why, sizeof(why));
  if (ok) {
    strcpy(dest, pattern);
    return true;
  }
  if (why[0]) snprintf(reply, CLI_REPLY_MAX, "Err - %s in %s regex", why, field);
  else snprintf(reply, CLI_REPLY_MAX, "Err - bad/long %s regex", field);
  return false;
}

// Copy a comma-separated CLI value into a scratch buffer for strsep() to walk
// in place. False if the value does not fit: a list is never truncated, so a
// too-long one is a clean error instead of a silently shortened rule.
static bool copyCsvList(const char* val, char* buf, size_t sz) {
  size_t len = strlen(val);
  if (len >= sz) return false;
  memcpy(buf, val, len + 1);
  return true;
}

static bool addRuleParam(FilterRules& filter, FilterRule* r, RegionMap* regions,
                         const char* key, const char* val, char* reply) {
  if (strcmp(key, "chan") == 0) {
    char names[80];
    if (!copyCsvList(val, names, sizeof(names))) { strcpy(reply, "Err - chan list too long"); return false; }
    uint16_t mask = 0;
    char* np = names;
    char* nm;
    while ((nm = strsep(&np, ",")) != NULL) {
      if (nm[0] == 0 || (nm[0] == '#' && nm[1] == 0)) { strcpy(reply, "Err - empty chan name"); return false; }
      int idx = filter.indexOfChannel(nm);
      if (idx < 0 && nm[0] == '#') {
        filter.addChannel(nm, NULL);   // auto-provision '#' names with derived PSK
        idx = filter.indexOfChannel(nm);
      }
      if (idx < 0) {
        if (nm[0] == '#' && filter.getNumChannels() >= FILTER_MAX_CHANNELS) {
          strcpy(reply, "Err - chan store full");
        } else {
          snprintf(reply, CLI_REPLY_MAX, "Err - unknown chan '%s'", nm);
        }
        return false;
      }
      mask |= (1 << idx);
    }
    // a repeated chan= replaces the earlier list rather than accumulating it;
    // chanhash= is a separate predicate and its flag is left alone
    r->chan_mask = mask;
    r->chan_flags |= FILTER_CHANFLG_MASK_SET;
    return true;
  }
  if (strcmp(key, "chanhash") == 0) {
    uint8_t len;
    if (!parseHexHash(val, &r->chan_hash, sizeof(r->chan_hash), &len)) {
      strcpy(reply, "Err - chanhash must be 2 hex chars");
      return false;
    }
    r->chan_flags |= FILTER_CHANFLG_HASH_SET;
    return true;
  }
  if (strcmp(key, "type") == 0) {
    char vals[24];
    if (!copyCsvList(val, vals, sizeof(vals))) { strcpy(reply, "Err - bad type"); return false; }
    uint8_t mask = 0;
    bool any = false;
    char* vp = vals;
    char* t;
    while ((t = strsep(&vp, ",")) != NULL) {
      if (strcmp(t, "advert") == 0) mask |= FILTER_TYPE_ADVERT;
      else if (strcmp(t, "txt") == 0) mask |= FILTER_TYPE_GRP_TXT;
      else if (strcmp(t, "data") == 0) mask |= FILTER_TYPE_GRP_DATA;
      else if (strcmp(t, "any") == 0) any = true;   // wildcard: wins even beside names
      else { snprintf(reply, CLI_REPLY_MAX, "Err - unknown type '%s'", t); return false; }
    }
    // a repeated type= replaces the earlier one. Alternatives inside ONE value
    // still OR, because that is how the list reads.
    r->type_mask = any ? 0 : mask;
    return true;
  }
  if (strcmp(key, "route") == 0) {
    if (strcmp(val, "flood") == 0) r->route_mask = FILTER_ROUTE_FLOOD;
    else if (strcmp(val, "direct") == 0) r->route_mask = FILTER_ROUTE_DIRECT;
    else { strcpy(reply, "Err - route must be flood|direct"); return false; }
    return true;
  }
  if (strcmp(key, "hops") == 0 || strcmp(key, "len") == 0 || strcmp(key, "snr") == 0) {
    const bool is_snr = (strcmp(key, "snr") == 0);
    Interval* iv = is_snr ? &r->snr : (strcmp(key, "hops") == 0 ? &r->hops : &r->len);
    if (!parseInterval(val, *iv, is_snr ? IV_SNR_DB : IV_UNSIGNED)) {
      snprintf(reply, CLI_REPLY_MAX, "Err - bad %s interval", key);
      return false;
    }
    return true;
  }
  if (strcmp(key, "path") == 0) {
    if (!parsePath(val, r) || r->path.count == 0) {
      strcpy(reply, "Err - bad path spec");
      return false;
    }
    return true;
  }
  if (strcmp(key, "region") == 0) {
    char vals[80];
    if (!copyCsvList(val, vals, sizeof(vals))) { strcpy(reply, "Err - region list too long"); return false; }
    char list[FILTER_REGION_LIST_LEN];
    list[0] = 0;
    char* vp = vals;
    char* t;
    while ((t = strsep(&vp, ",")) != NULL) {
      if (t[0] == 0) { strcpy(reply, "Err - empty region name"); return false; }
      const char* canon;
      if (strcmp(t, "unscoped") == 0 || strcmp(t, "*") == 0) {
        canon = "unscoped";   // keyword wins, even if a region were named "unscoped"
      } else {
        RegionEntry* reg = regions->findByNamePrefix(t);
        if (reg == NULL) { snprintf(reply, CLI_REPLY_MAX, "Err - unknown region '%s'", t); return false; }
        canon = reg->name;
      }
      size_t used = strlen(list);
      if (used + strlen(canon) + 2 > sizeof(list)) { strcpy(reply, "Err - region list too long"); return false; }
      if (used) list[used++] = ',';
      strcpy(&list[used], canon);
    }
    strcpy(r->regions, list);
    return true;
  }
  if (strcmp(key, "hsize") == 0) {
    char vals[12];
    if (!copyCsvList(val, vals, sizeof(vals))) { strcpy(reply, "Err - bad hsize"); return false; }
    uint8_t mask = 0;
    char* vp = vals;
    char* t;
    while ((t = strsep(&vp, ",")) != NULL) {
      long v;
      if (!parseIntRange(t, 1, 4, &v)) { strcpy(reply, "Err - hsize values are 1..4"); return false; }
      mask |= (1 << (v - 1));
    }
    r->hash_size_mask = mask;   // a repeated hsize= replaces the earlier list
    return true;
  }
  if (strcmp(key, "sender") == 0 || strcmp(key, "text") == 0) {
    if (val[0] == 0) { strcpy(reply, "Err - empty regex"); return false; }   // matches everything
    const bool is_sender = (strcmp(key, "sender") == 0);
    return setPattern(is_sender ? r->sender : r->text,
                      is_sender ? FILTER_SENDER_PATTERN_LEN : FILTER_TEXT_PATTERN_LEN,
                      val, key, reply);
  }
  if (strcmp(key, "action") == 0) {
    if (strcmp(val, "drop") == 0) r->action = FILTER_ACT_DROP;
    else if (strcmp(val, "forward") == 0) r->action = FILTER_ACT_FORWARD;
    else { strcpy(reply, "Err - action must be drop|forward"); return false; }
    return true;
  }
  if (strcmp(key, "prob") == 0) {
    long v;
    if (!parseIntRange(val, 1, 100, &v)) {
      strcpy(reply, "Err - prob must be 1..100 (omit for 100%)");
      return false;
    }
    r->prob = (uint8_t)v;
    return true;
  }
  if (strcmp(key, "throttle") == 0) {
    long v;
    if (!parseIntRange(val, 1, 65535, &v)) {
      strcpy(reply, "Err - throttle must be 1..65535 s (omit for no limit)");
      return false;
    }
    r->throttle = (uint16_t)v;
    return true;
  }
  snprintf(reply, CLI_REPLY_MAX, "Err - unknown param '%s'", key);   // key is a raw command token
  return false;
}

// ---------------------------------------------------------------- CLI commands

static void cliStatus(FilterRules& filter, char* reply) {
  char* out = reply;
  int remain = CLI_REPLY_MAX;
  radd(&out, &remain, "%s; rules %d/%d; chans %d/%d; ratelimit advert %uh; cache %d/%d",
       filter.isEnabled() ? "on" : "off", filter.getNumRules(), FILTER_MAX_RULES,
       filter.getNumChannels(), FILTER_MAX_CHANNELS, filter.getAdvertRatelimit(),
       filter.getAdvertRatelimit() ? filter.getAdvertCacheCount() : 0, FILTER_ADVERT_CACHE_SIZE);
  radd(&out, &remain, "; limiter %lu; aborted %lu", (unsigned long)filter.getLimiterDrops(),
       (unsigned long)filter.getBudgetAborts());
}

static void cliChanList(FilterRules& filter, char* reply) {
  char* out = reply;
  int remain = CLI_REPLY_MAX;
  if (filter.getNumChannels() == 0) {
    strcpy(reply, "no channels");
    return;
  }
  for (int i = 0; i < filter.getNumChannels(); i++) {
    auto ch = filter.getChannel(i);
    radd(&out, &remain, "%s%d:%s:%02X", i ? " " : "", i, ch->name, ch->hash);
  }
}

static void cliChanAdd(FilterRules& filter, char* params, char* reply) {
  char* p = params;
  char* name = nextToken(&p);
  char* psk = nextToken(&p);
  if (name == NULL) { strcpy(reply, "Err - usage: filter chan add <name> [<psk-hex>]"); return; }
  if (strlen(name) >= FILTER_CHAN_NAME_LEN) { strcpy(reply, "Err - name too long"); return; }
  if (name[0] == '#' && name[1] == 0) { strcpy(reply, "Err - empty chan name"); return; }
  if (!validChannelName(name)) {
    strcpy(reply, "Err - chan name must not contain , = \" or control chars");
    return;
  }
  if (filter.findChannel(name) != NULL) { strcpy(reply, "Err - channel exists"); return; }
  if (psk != NULL && psk[0]) {
    size_t hex_len = strlen(psk);
    if (hex_len != 32 && hex_len != 64) {
      strcpy(reply, "Err - psk must be 32 or 64 hex chars");
      return;
    }
  } else if (name[0] != '#') {
    strcpy(reply, "Err - psk required for non-# names");
    return;
  }
  auto ch = filter.addChannel(name, psk);
  if (ch == NULL) { strcpy(reply, "Err - bad psk or store full"); return; }
  // Core tries at most four distinct keys per on-air tag (Mesh.cpp). Aliases of
  // one key are free, but a tag carrying more distinct keys than that has
  // channels that will silently never decrypt here — say so instead.
  int distinct = distinctKeysOnHash(filter, ch->hash);
  snprintf(reply, CLI_REPLY_MAX, "OK - chan %s h=%02X%s%s", ch->name, ch->hash,
           psk == NULL ? " (derived)" : "",
           distinct > 1 ? " (multiple keys on this hash; core tries 4)" : "");
}

static void cliChanDel(FilterRules& filter, char* params, char* reply) {
  char* p = params;
  char* name = nextToken(&p);
  if (name == NULL) { strcpy(reply, "Err - usage: filter chan del <name>"); return; }
  int idx = filter.indexOfChannel(name);
  if (idx < 0) { strcpy(reply, "Err - unknown channel"); return; }
  filter.delChannel(idx);
  snprintf(reply, CLI_REPLY_MAX, "OK - chan %s deleted", name);
}

// Undo a rejected `filter add`: the half-built rule plus any channel `chan=`
// auto-provisioned while parsing. A stored '#' key is not inert — it makes the
// repeater a decryption candidate for that channel — so it must not outlive a
// failed add. Auto-provisioned names are appended, so dropping the tail is safe.
static void rollbackAdd(FilterRules& filter, int idx, int chans_before) {
  filter.delRule(idx);
  for (int i = filter.getNumChannels(); i > chans_before; i--) filter.delChannel(i - 1);
}

static void cliAdd(FilterRules& filter, RegionMap* regions, char* params, char* reply) {
  // an odd quote count would silently swallow the tokens that follow
  int quotes = 0;
  for (const char* s = params; *s; s++) if (*s == '"') quotes++;
  if (quotes & 1) { strcpy(reply, "Err - unbalanced quotes"); return; }

  FilterRule* r = filter.addRule();
  if (r == NULL) { strcpy(reply, "Err - rule list full"); return; }
  int idx = filter.getNumRules() - 1;
  int chans_before = filter.getNumChannels();

  char* p = params;
  char* tok;
  while ((tok = nextToken(&p)) != NULL) {
    char* eq = strchr(tok, '=');
    if (eq == NULL) {
      snprintf(reply, CLI_REPLY_MAX, "Err - expected key=value, got '%s'", tok);
      rollbackAdd(filter, idx, chans_before);
      return;
    }
    *eq = 0;
    if (!addRuleParam(filter, r, regions, tok, eq + 1, reply)) {
      rollbackAdd(filter, idx, chans_before);
      return;
    }
  }
  snprintf(reply, CLI_REPLY_MAX, "OK - rule %d added", idx);
}

static void cliList(FilterRules& filter, char* reply) {
  char* out = reply;
  int remain = CLI_REPLY_MAX;
  radd(&out, &remain, "%s %d/%d:", filter.isEnabled() ? "on" : "off",
       filter.getNumRules(), FILTER_MAX_RULES);
  for (int i = 0; i < filter.getNumRules(); i++) {
    auto r = filter.getRule(i);
    radd(&out, &remain, " %d%c%c%03X", i, r->enabled ? 'e' : 'd',
         r->action == FILTER_ACT_DROP ? 'D' : 'F', ruleDigest(r) & 0xFFF);
  }
}

static void cliGet(FilterRules& filter, int idx, char* reply) {
  if (idx < 0 || idx >= filter.getNumRules()) { strcpy(reply, "Err - no such rule"); return; }
  auto r = filter.getRule(idx);
  char* out = reply;
  int remain = CLI_REPLY_MAX;
  radd(&out, &remain, "r%d %s %s", idx, r->enabled ? "en" : "dis",
       r->action == FILTER_ACT_DROP ? "drop" : "forward");

  if (r->type_mask) {
    radd(&out, &remain, " type=");
    const char* sep = "";
    if (r->type_mask & FILTER_TYPE_ADVERT) { radd(&out, &remain, "%sadvert", sep); sep = ","; }
    if (r->type_mask & FILTER_TYPE_GRP_TXT) { radd(&out, &remain, "%stxt", sep); sep = ","; }
    if (r->type_mask & FILTER_TYPE_GRP_DATA) { radd(&out, &remain, "%sdata", sep); sep = ","; }
  }
  if (r->route_mask) radd(&out, &remain, " route=%s",
                          (r->route_mask & FILTER_ROUTE_FLOOD) ? "flood" : "direct");
  char ivs[24];
  if (r->hops.flags) { formatInterval(r->hops, ivs, sizeof(ivs), IV_UNSIGNED); radd(&out, &remain, " hops=%s", ivs); }
  if (r->len.flags) { formatInterval(r->len, ivs, sizeof(ivs), IV_UNSIGNED); radd(&out, &remain, " len=%s", ivs); }
  if (r->snr.flags) { formatInterval(r->snr, ivs, sizeof(ivs), IV_SNR_DB); radd(&out, &remain, " snr=%s", ivs); }
  if (r->path.count) {
    radd(&out, &remain, " path=%s", (r->path.pos & FILTER_PATH_FIRST) ? "^" : "");
    for (int e = 0; e < r->path.count; e++) {
      radd(&out, &remain, "%s", e ? ">" : "");
      for (int b = 0; b < r->path.len[e]; b++) radd(&out, &remain, "%02X", r->path.bytes[e][b]);
    }
    radd(&out, &remain, "%s", (r->path.pos & FILTER_PATH_LAST) ? "$" : "");
  }
  if (r->hash_size_mask) {
    radd(&out, &remain, " hsize=");
    const char* sep = "";
    for (int s = 1; s <= 4; s++) {
      if (r->hash_size_mask & (1 << (s - 1))) { radd(&out, &remain, "%s%d", sep, s); sep = ","; }
    }
  }
  if (r->chan_flags & FILTER_CHANFLG_MASK_SET) {
    // quote the whole value when a referenced name contains a space: nextToken
    // keeps it one token and the comma list still splits the names apart.
    // Names can no longer contain a comma, so the list stays unambiguous.
    bool quote = false;
    for (int c = 0; c < filter.getNumChannels(); c++) {
      if ((r->chan_mask & (1 << c)) && strchr(filter.getChannel(c)->name, ' ')) quote = true;
    }
    radd(&out, &remain, " chan=%s", quote ? "\"" : "");
    const char* sep = "";
    for (int c = 0; c < filter.getNumChannels(); c++) {
      if (r->chan_mask & (1 << c)) { radd(&out, &remain, "%s%s", sep, filter.getChannel(c)->name); sep = ","; }
    }
    radd(&out, &remain, "%s", quote ? "\"" : "");
  }
  if (r->chan_flags & FILTER_CHANFLG_HASH_SET) radd(&out, &remain, " chanhash=%02X", r->chan_hash);
  if (r->regions[0]) radd(&out, &remain, " region=%s", r->regions);
  if (r->sender[0]) {
    if (strchr(r->sender, ' ')) radd(&out, &remain, " sender=\"%s\"", r->sender);
    else radd(&out, &remain, " sender=%s", r->sender);
  }
  if (r->text[0]) {
    if (strchr(r->text, ' ')) radd(&out, &remain, " text=\"%s\"", r->text);
    else radd(&out, &remain, " text=%s", r->text);
  }
  if (r->prob) radd(&out, &remain, " prob=%u", r->prob);
  if (r->throttle) radd(&out, &remain, " throttle=%u pass=%lu", r->throttle, (unsigned long)r->throttle_pass);
  radd(&out, &remain, " hits=%lu", (unsigned long)r->hits);
  radd(&out, &remain, " air=%lu", (unsigned long)r->air_ms);
}

static void cliStats(FilterRules& filter, char* reply) {
  char* out = reply;
  int remain = CLI_REPLY_MAX;
  // globals first, per-rule hits last: if the reply truncates, hits detail
  // (recoverable via `get N`) is sacrificed before the summary counters
  radd(&out, &remain, "lim:%lu abort:%lu air:%" PRIu64 ":%u%%; hits:",
       (unsigned long)filter.getLimiterDrops(), (unsigned long)filter.getBudgetAborts(),
       filter.getAirSavedMs(), filter.getAirSavedPercent());
  for (int i = 0; i < filter.getNumRules(); i++) {
    radd(&out, &remain, " %d:%lu", i, (unsigned long)filter.getRule(i)->hits);
  }
}

static bool cliRuleIdx(FilterRules& filter, char* arg, int& idx, char* reply) {
  if (arg == NULL || arg[0] < '0' || arg[0] > '9') { strcpy(reply, "Err - rule index required"); return false; }
  char* end;
  long v = strtol(arg, &end, 10);   // full token must be consumed ("0x10" is not an index)
  if (*end != 0 || v < 0 || v >= filter.getNumRules()) { strcpy(reply, "Err - no such rule"); return false; }
  idx = (int)v;
  return true;
}

void filterCLI(FilterRules& filter, const char* command, char* reply, RegionMap* regions) {
  char buf[MAX_PACKET_PAYLOAD + 1];
  cliCopyCommand(buf, sizeof(buf), command);
  char* p = buf;
  char* cmd = nextToken(&p);

  if (cmd == NULL) {
    cliStatus(filter, reply);
  } else if (strcmp(cmd, "on") == 0) {
    filter.setEnabled(true);
    strcpy(reply, "OK - filter on");
  } else if (strcmp(cmd, "off") == 0) {
    filter.setEnabled(false);
    strcpy(reply, "OK - filter off");
  } else if (strcmp(cmd, "chan") == 0) {
    char* sub = nextToken(&p);
    if (sub == NULL || strcmp(sub, "list") == 0) {
      cliChanList(filter, reply);
    } else if (strcmp(sub, "add") == 0) {
      cliChanAdd(filter, p, reply);
    } else if (strcmp(sub, "del") == 0) {
      cliChanDel(filter, p, reply);
    } else {
      strcpy(reply, "Err - usage: chan list|add <name> [<psk>]|del <name>");
    }
  } else if (strcmp(cmd, "add") == 0) {
    cliAdd(filter, regions, p, reply);
  } else if (strcmp(cmd, "list") == 0) {
    cliList(filter, reply);
  } else if (strcmp(cmd, "get") == 0) {
    int idx;
    if (cliRuleIdx(filter, nextToken(&p), idx, reply)) cliGet(filter, idx, reply);
  } else if (strcmp(cmd, "enable") == 0 || strcmp(cmd, "disable") == 0) {
    bool on = (strcmp(cmd, "enable") == 0);
    int idx;
    if (cliRuleIdx(filter, nextToken(&p), idx, reply)) {
      filter.getRule(idx)->enabled = on;
      filter.markDirty();
      snprintf(reply, CLI_REPLY_MAX, "OK - rule %d %s", idx, on ? "enabled" : "disabled");
    }
  } else if (strcmp(cmd, "move") == 0) {
    int from, to;
    if (cliRuleIdx(filter, nextToken(&p), from, reply) &&
        cliRuleIdx(filter, nextToken(&p), to, reply)) {
      if (from == to) {
        strcpy(reply, "Err - move: source and target are the same rule");   // no-op move is rejected
      } else {
        filter.moveRule(from, to);
        snprintf(reply, CLI_REPLY_MAX, "OK - rule %d moved to %d", from, to);
      }
    }
  } else if (strcmp(cmd, "del") == 0) {
    int idx;
    if (cliRuleIdx(filter, nextToken(&p), idx, reply)) {
      filter.delRule(idx);
      snprintf(reply, CLI_REPLY_MAX, "OK - rule %d deleted", idx);
    }
  } else if (strcmp(cmd, "clear") == 0) {
    filter.clearRules();
    strcpy(reply, "OK - rules cleared (chans kept)");
  } else if (strcmp(cmd, "ratelimit") == 0) {
    char* sub = nextToken(&p);
    if (sub == NULL) {
      snprintf(reply, CLI_REPLY_MAX, "ratelimit advert %uh; cache %d/%d", filter.getAdvertRatelimit(),
              filter.getAdvertCacheCount(), FILTER_ADVERT_CACHE_SIZE);
    } else if (strcmp(sub, "advert") == 0) {
      long v;
      if (!parseIntRange(nextToken(&p), 0, FILTER_ADVERT_HOURS_MAX, &v)) {
        snprintf(reply, CLI_REPLY_MAX, "Err - hours must be 0..%d (0=off)", FILTER_ADVERT_HOURS_MAX);
      } else {
        filter.setAdvertRatelimit((uint16_t)v);
        snprintf(reply, CLI_REPLY_MAX, "OK - advert ratelimit %ldh", v);
      }
    } else if (strcmp(sub, "clear") == 0) {
      filter.clearAdvertCache();
      strcpy(reply, "OK - advert cache cleared");
    } else {
      strcpy(reply, "Err - usage: ratelimit advert <hours>|clear");
    }
  } else if (strcmp(cmd, "stats") == 0) {
    cliStats(filter, reply);
  } else {
    strcpy(reply, "Err - usage: on|off|add|list|get|enable|disable|move|del|clear|chan|ratelimit|stats");
  }
}