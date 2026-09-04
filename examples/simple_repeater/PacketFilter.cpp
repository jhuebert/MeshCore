// PacketFilter.cpp — remote-configurable packet drop rules for simple_repeater.
// See PacketFilter.h for the rule model.

#include "PacketFilter.h"
#include <helpers/TxtDataHelpers.h>

// Minimal base64 decoder for PSK entry (16/32-byte keys). Fork-owned: the
// densaugeo/base64 lib is not in the repeater envs' lib_deps.
static int filterDecodeBase64(const char* in, size_t in_len, uint8_t* out) {
  uint8_t quad[4];
  int nq = 0, out_len = 0;
  for (size_t i = 0; i < in_len; i++) {
    char c = in[i];
    if (c == '=' || c == '\r' || c == '\n') continue;
    int v;
    if (c >= 'A' && c <= 'Z') v = c - 'A';
    else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
    else if (c >= '0' && c <= '9') v = c - '0' + 52;
    else if (c == '+') v = 62;
    else if (c == '/') v = 63;
    else return 0;
    quad[nq++] = (uint8_t)v;
    if (nq == 4) {
      out[out_len++] = (quad[0] << 2) | (quad[1] >> 4);
      out[out_len++] = (quad[1] << 4) | (quad[2] >> 2);
      out[out_len++] = (quad[2] << 6) | quad[3];
      nq = 0;
    }
  }
  if (nq == 2) { out[out_len++] = (quad[0] << 2) | (quad[1] >> 4); }
  else if (nq == 3) {
    out[out_len++] = (quad[0] << 2) | (quad[1] >> 4);
    out[out_len++] = (quad[1] << 4) | (quad[2] >> 2);
  } else if (nq != 0) {
    return 0;
  }
  return out_len;
}

// The well-known Public channel PSK (16 bytes); its sha256()[0] air hash is 0x11.
#define FILTER_PUBLIC_PSK_B64  "izOH6cXN6mrJ5e26oRXNcg=="
#define FILTER_CFG_FILE        "/filter_cfg"
#define FILTER_CFG_VERSION     1
#define FILTER_SAVE_DELAY_MS   3000          // lazy dirty-write delay (like ClientACL)
#define FILTER_ADVERT_HOURS_MAX 720          // ~30 days; millis() wraps at ~49.7 days

// ---------------------------------------------------------------- initialization

FilterRules::FilterRules() {
  memset(rules, 0, sizeof(rules));
  memset(channels, 0, sizeof(channels));
  memset(advert_cache, 0, sizeof(advert_cache));
  num_rules = 0;
  num_channels = 0;
  advert_cache_count = 0;
  advert_cache_head = 0;
  ratelimit_hours = 0;
  limiter_drops = 0;
  budget_aborts = 0;
  enabled = true;
  dirty = false;
  dirty_since = 0;
  _fs = NULL;
}

void FilterRules::begin(FILESYSTEM* fs) {
  bool fresh = !fs->exists(FILTER_CFG_FILE);
  load(fs);

  // pre-provision the well-known Public channel on a fresh node
  if (fresh && findChannel("Public") == NULL) {
    addChannel("Public", FILTER_PUBLIC_PSK_B64);
  }
}

void FilterRules::loop(FILESYSTEM* fs) {
  if (dirty && millis() - dirty_since >= FILTER_SAVE_DELAY_MS) {
    save(fs);
  }
}

// ---------------------------------------------------------------- enable / stats

void FilterRules::setEnabled(bool on) {
  if (enabled != on) {
    enabled = on;
    dirty = true; dirty_since = millis();
  }
}

void FilterRules::resetStats() {
  limiter_drops = 0;
  budget_aborts = 0;
  for (int i = 0; i < num_rules; i++) rules[i].hits = 0;
}

// ---------------------------------------------------------------- rule management

FilterRule* FilterRules::addRule() {
  if (num_rules >= FILTER_MAX_RULES) return NULL;
  FilterRule* r = &rules[num_rules++];
  memset(r, 0, sizeof(FilterRule));
  r->enabled = true;
  r->action = FILTER_ACT_DROP;
  dirty = true; dirty_since = millis();
  return r;
}

void FilterRules::delRule(int idx) {
  if (idx < 0 || idx >= num_rules) return;
  memmove(&rules[idx], &rules[idx + 1], (num_rules - idx - 1) * sizeof(FilterRule));
  memset(&rules[num_rules - 1], 0, sizeof(FilterRule));
  num_rules--;
  dirty = true; dirty_since = millis();
}

void FilterRules::clearRules() {
  memset(rules, 0, sizeof(rules));
  num_rules = 0;
  dirty = true; dirty_since = millis();
}

// ---------------------------------------------------------------- channel store

FilterChannel* FilterRules::findChannel(const char* name) {
  for (int i = 0; i < num_channels; i++) {
    if (strcmp(channels[i].name, name) == 0) return &channels[i];
  }
  return NULL;
}

FilterChannel* FilterRules::addChannel(const char* name, const char* psk_base64) {
  if (num_channels >= FILTER_MAX_CHANNELS) return NULL;
  if (name[0] == 0 || strlen(name) >= FILTER_CHAN_NAME_LEN) return NULL;
  if (findChannel(name) != NULL) return NULL;   // already in the store

  FilterChannel* ch = &channels[num_channels];
  memset(ch, 0, sizeof(FilterChannel));

  if (psk_base64 == NULL || psk_base64[0] == 0) {
    if (name[0] != '#') return NULL;   // psk required for non-hash channels
    // hashtag channels are public-by-construction: secret = sha256(name)[0..15]
    // (per the companion protocol; see plan §2.11)
    uint8_t digest[32];
    mesh::Utils::sha256(digest, sizeof(digest), (const uint8_t*)name, strlen(name));
    memcpy(ch->secret, digest, 16);
    ch->secret_len = 16;
  } else {
    int len = filterDecodeBase64(psk_base64, strlen(psk_base64), ch->secret);
    if (len != 16 && len != 32) return NULL;
    ch->secret_len = len;
  }

  mesh::Utils::sha256(ch->hash, sizeof(ch->hash), ch->secret, ch->secret_len);
  StrHelper::strzcpy(ch->name, name, FILTER_CHAN_NAME_LEN);
  num_channels++;
  dirty = true; dirty_since = millis();
  return ch;
}

void FilterRules::delChannel(int idx) {
  if (idx < 0 || idx >= num_channels) return;
  memmove(&channels[idx], &channels[idx + 1], (num_channels - idx - 1) * sizeof(FilterChannel));
  memset(&channels[num_channels - 1], 0, sizeof(FilterChannel));
  num_channels--;
  dirty = true; dirty_since = millis();
}

int FilterRules::searchChannelsByHash(const uint8_t* hash, mesh::GroupChannel dest[], int max_matches) {
  if (!enabled) return 0;   // filter off -> behave like stock (no channel DB)
  int n = 0;
  for (int i = 0; i < num_channels && n < max_matches; i++) {
    if (channels[i].name[0] == 0) continue;   // null-key guard (same as BaseChatMesh)
    if (channels[i].hash[0] == hash[0]) {
      memcpy(dest[n].hash, channels[i].hash, sizeof(dest[n].hash));
      memset(dest[n].secret, 0, sizeof(dest[n].secret));
      memcpy(dest[n].secret, channels[i].secret, sizeof(dest[n].secret));
      n++;
    }
  }
  return n;
}

// ---------------------------------------------------------------- advert rate limiter

void FilterRules::setAdvertRatelimit(uint16_t hours) {
  ratelimit_hours = hours;
  dirty = true; dirty_since = millis();
}

void FilterRules::clearAdvertCache() {
  memset(advert_cache, 0, sizeof(advert_cache));
  advert_cache_count = 0;
  advert_cache_head = 0;
}

// per-node advert repeat window: "an advert for each node at most once every
// N hours". Core guarantees checkPacket() sees each unique, signature-verified
// advert exactly once, so each origin is recorded exactly once per window.
bool FilterRules::advertRatelimitDrop(const mesh::Packet* pkt, uint32_t now_millis) {
  uint32_t window_ms = (uint32_t)ratelimit_hours * 3600UL * 1000UL;
  const uint8_t* prefix = pkt->payload;   // origin pubkey = payload[0..31]

  int total = advert_cache_count < FILTER_ADVERT_CACHE_SIZE ? advert_cache_count
                                                           : FILTER_ADVERT_CACHE_SIZE;
  for (int i = 0; i < total; i++) {
    int idx = (advert_cache_count < FILTER_ADVERT_CACHE_SIZE)
              ? i : (advert_cache_head + i) % FILTER_ADVERT_CACHE_SIZE;
    AdvertSeenEntry* e = &advert_cache[idx];
    if (memcmp(e->pub_key_prefix, prefix, sizeof(e->pub_key_prefix)) == 0) {
      if (now_millis - e->first_seen_millis < window_ms) {
        limiter_drops++;
        return true;   // too soon: drop the repeat
      }
      e->first_seen_millis = now_millis;   // refresh: window restarts
      return false;
    }
  }

  // not seen in this window: record
  AdvertSeenEntry* e;
  if (advert_cache_count < FILTER_ADVERT_CACHE_SIZE) {
    e = &advert_cache[advert_cache_count++];
  } else {
    e = &advert_cache[advert_cache_head];   // overwrite the oldest entry
    advert_cache_head = (advert_cache_head + 1) % FILTER_ADVERT_CACHE_SIZE;
  }
  memcpy(e->pub_key_prefix, prefix, sizeof(e->pub_key_prefix));
  e->first_seen_millis = now_millis;
  return false;
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
  uint8_t hsz = pkt->getPathHashSize();
  uint8_t n = pkt->getPathHashCount();
  if (n == 0) return false;          // 0-hop traffic never matches
  if (r->path.count > n) return false;

  int start_min = 0, start_max = n - r->path.count;
  switch (r->path.pos) {
    case FILTER_PATH_FIRST: start_min = 0; start_max = 0; break;
    case FILTER_PATH_LAST:  start_min = n - r->path.count; start_max = start_min; break;
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

// Rules with content predicates are deferred to checkContent() (decrypted data).
static bool ruleIsDeferred(const FilterRule* r) {
  if (r->chan_flags & FILTER_CHANFLG_MASK_SET) return true;
  if (r->sender[0] != 0 || r->text[0] != 0) return true;
  return false;
}

// Evaluate the packet-level predicates of one rule (shared by both phases).
static bool ruleMatchesPacket(const FilterRule* r, const mesh::Packet* pkt, uint8_t payload_type) {
  if (r->type_mask != 0 && !(r->type_mask & (1 << payload_type))) return false;

  if (r->route_mask != 0) {
    uint8_t bit = pkt->isRouteFlood() ? FILTER_ROUTE_FLOOD : FILTER_ROUTE_DIRECT;
    if (!(r->route_mask & bit)) return false;
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

uint8_t FilterRules::checkPacket(const mesh::Packet* pkt, uint32_t now_millis) {
  if (!enabled) return FILTER_ACT_ALLOW;

  uint8_t payload_type = pkt->getPayloadType();
  uint8_t action = FILTER_ACT_ALLOW;
  for (int i = 0; i < num_rules; i++) {
    FilterRule* r = &rules[i];
    if (!r->enabled || ruleIsDeferred(r)) continue;   // deferred rules decide on decrypted content
    if (ruleMatchesPacket(r, pkt, payload_type)) {
      r->hits++;
      action = r->action;
      break;   // first match wins
    }
  }
  if (action == FILTER_ACT_DROP) return FILTER_ACT_DROP;

  // limiter runs unless the rule list already dropped; logonly adverts too
  if (payload_type == PAYLOAD_TYPE_ADVERT && pkt->isRouteFlood() &&
      ratelimit_hours > 0 && advertRatelimitDrop(pkt, now_millis)) {
    return FILTER_ACT_DROP;
  }

  return action;
}

// ---------------------------------------------------------------- content rules

// GRP_TXT payload (after decrypt): ts(4) | txt_type(1) | "<sender>: <text>".
// Content is sender-controlled: parse defensively, bounded by len (never strlen()).
static void parseGroupText(const uint8_t* data, size_t len, char* sender, size_t sender_sz,
                           char* text, size_t text_sz) {
  sender[0] = 0;
  text[0] = 0;
  if (len < 5) return;

  const uint8_t* p = data + 5;
  size_t n = len - 5;

  const uint8_t* colon = NULL;
  for (size_t i = 0; i < n; i++) {
    if (p[i] == ':') { colon = &p[i]; break; }
  }

  if (colon == NULL) {   // no sender extractable; whole remainder is text
    size_t cpy = n < text_sz - 1 ? n : text_sz - 1;
    memcpy(text, p, cpy);
    text[cpy] = 0;
    return;
  }

  size_t slen = colon - p;
  while (slen > 0 && (p[slen - 1] == ' ' || p[slen - 1] == '\t')) slen--;   // trim trailing spaces/tabs
  size_t cpy = slen < sender_sz - 1 ? slen : sender_sz - 1;
  memcpy(sender, p, cpy);
  sender[cpy] = 0;

  const uint8_t* tp = colon + 1;
  size_t tlen = n - (size_t)(tp - p);
  while (tlen > 0 && (*tp == ' ' || *tp == '\t' || *tp == '\r' || *tp == '\n')) { tp++; tlen--; }
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
  int matchlength;
  int idx = re_match(pattern, subject, &matchlength);
  if (re_budget_exhausted()) {
    budget_aborts++;     // fail-open: worst case is a spam message repeated
    return false;
  }
  return idx >= 0;   // re_matchp() sets matchlength=0 even on no-match; use idx
}

uint8_t FilterRules::checkContent(mesh::Packet* pkt, uint8_t type, const mesh::GroupChannel& channel,
                                  const uint8_t* data, size_t len) {
  if (!enabled) return FILTER_ACT_ALLOW;
  if (type != PAYLOAD_TYPE_GRP_TXT && type != PAYLOAD_TYPE_GRP_DATA) return FILTER_ACT_ALLOW;

  char sender[64];
  char text[MAX_PACKET_PAYLOAD + 1];
  bool parsed = (type == PAYLOAD_TYPE_GRP_TXT);
  if (parsed) parseGroupText(data, len, sender, sizeof(sender), text, sizeof(text));

  for (int i = 0; i < num_rules; i++) {
    FilterRule* r = &rules[i];
    if (!r->enabled || !ruleIsDeferred(r)) continue;
    if (!ruleMatchesPacket(r, pkt, type)) continue;
    if ((r->chan_flags & FILTER_CHANFLG_MASK_SET) && !channelMatchesStore(r, channel)) continue;
    if (r->sender[0] && (!parsed || !regexMatches(r->sender, sender))) continue;
    if (r->text[0] && (!parsed || !regexMatches(r->text, text))) continue;
    r->hits++;
    return r->action;   // first match wins
  }
  return FILTER_ACT_ALLOW;
}

// ---------------------------------------------------------------- persistence
// Binary format (version 1): header + raw rule structs + raw channel structs.
// All struct members are fixed-size arrays/scalars (no pointers) and structs
// are memset(0) before use, so a raw write/read is deterministic on a given
// platform; the version byte guards against layout drift.

void FilterRules::load(FILESYSTEM* fs) {
  _fs = fs;
  num_rules = 0;
  num_channels = 0;
  enabled = true;

  if (!fs->exists(FILTER_CFG_FILE)) return;
#if defined(RP2040_PLATFORM)
  File file = fs->open(FILTER_CFG_FILE, "r");
#else
  File file = fs->open(FILTER_CFG_FILE);
#endif
  if (file) {
    uint8_t hdr[5];   // version, enabled, num_rules, num_channels, (spare)
    if (file.read(hdr, 1) == 1 && hdr[0] == FILTER_CFG_VERSION && file.read(hdr, 4) == 4) {
      enabled = hdr[0] != 0;
      uint8_t nr = hdr[1] < FILTER_MAX_RULES ? hdr[1] : FILTER_MAX_RULES;
      uint8_t nc = hdr[2] < FILTER_MAX_CHANNELS ? hdr[2] : FILTER_MAX_CHANNELS;
      if (file.read((uint8_t*)&ratelimit_hours, 2) == 2) {
        bool ok = true;
        for (int i = 0; ok && i < nr; i++) {
          ok = (file.read((uint8_t*)&rules[i], sizeof(FilterRule)) == sizeof(FilterRule));
        }
        for (int i = 0; ok && i < nc; i++) {
          ok = (file.read((uint8_t*)&channels[i], sizeof(FilterChannel)) == sizeof(FilterChannel));
        }
        if (ok) {
          num_rules = nr;
          num_channels = nc;
        }
      }
    }
    file.close();
  }
}

static File filterOpenWrite(FILESYSTEM* fs, const char* filename) {
  #if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
    fs->remove(filename);
    return fs->open(filename, FILE_O_WRITE);
  #elif defined(RP2040_PLATFORM)
    return fs->open(filename, "w");
  #else
    return fs->open(filename, "w", true);
  #endif
}

void FilterRules::save(FILESYSTEM* fs) {
  dirty = false;
  File file = filterOpenWrite(fs, FILTER_CFG_FILE);
  if (file) {
    uint8_t hdr[5];
    hdr[0] = FILTER_CFG_VERSION;
    hdr[1] = enabled ? 1 : 0;
    hdr[2] = (uint8_t)num_rules;
    hdr[3] = (uint8_t)num_channels;
    hdr[4] = 0;
    bool ok = (file.write(hdr, 5) == 5);
    ok = ok && (file.write((uint8_t*)&ratelimit_hours, 2) == 2);
    for (int i = 0; ok && i < num_rules; i++) {
      ok = (file.write((uint8_t*)&rules[i], sizeof(FilterRule)) == sizeof(FilterRule));
    }
    for (int i = 0; ok && i < num_channels; i++) {
      ok = (file.write((uint8_t*)&channels[i], sizeof(FilterChannel)) == sizeof(FilterChannel));
    }
    file.close();
  }
}

// ---------------------------------------------------------------- CLI

static char* nextToken(char** p) {
  char* s = *p;
  while (*s == ' ') s++;
  if (*s == 0) { *p = s; return NULL; }
  char* t = s;
  while (*s && *s != ' ') s++;
  if (*s) { *s = 0; s++; }
  *p = s;
  return t;
}

static int hexVal(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// bounded reply append (CLI reply buffer is 160 bytes)
static void radd(char** out, int* remain, const char* fmt, ...) {
  if (*remain <= 0) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(*out, *remain, fmt, ap);
  va_end(ap);
  if (n < 0) { *remain = 0; return; }
  if (n >= *remain) { *out += *remain - 1; *remain = 0; return; }
  *out += n;
  *remain -= n;
}

// interval value: hops/len = unsigned decimal; snr = signed dB (snapped to the
// quarter-dB grid, stored as quarter-dB int)
static bool parseIvNum(const char* s, bool snr_mode, uint16_t* out) {
  if (snr_mode) {
    char* end;
    float f = strtof(s, &end);
    if (end == s || *end != 0) return false;
    int32_t q = (int32_t)lroundf(f * 4.0f);
    if (q < -32768 || q > 32767) return false;
    *out = (uint16_t)(int16_t)q;
  } else {
    char* end;
    long v = strtol(s, &end, 10);
    if (end == s || *end != 0 || v < 0 || v > 32767) return false;   // endpoints are compared as int16
    *out = (uint16_t)v;
  }
  return true;
}

// "[a,b]" / "(a,*]" / bare value = exact; bracket/paren = inclusive/exclusive.
// snr_mode: values are signed dB (e.g. "0.0", "-3.25").
static bool parseInterval(const char* tok, Interval& iv, bool snr_mode) {
  memset(&iv, 0, sizeof(iv));
  if (tok[0] == '[' || tok[0] == '(') {
    uint8_t flags = (tok[0] == '[') ? FILTER_IV_LO_INC : 0;
    const char* comma = strchr(tok, ',');
    if (comma == NULL) return false;
    size_t len = strlen(tok);
    char endc = tok[len - 1];
    if (endc != ']' && endc != ')') return false;
    flags |= (endc == ']') ? FILTER_IV_HI_INC : 0;

    char los[24], his[24];
    size_t llen = comma - tok - 1;
    size_t hlen = len - 1 - (size_t)(comma - tok) - 1;
    if (llen == 0 || llen >= sizeof(los) || hlen == 0 || hlen >= sizeof(his)) return false;
    memcpy(los, tok + 1, llen); los[llen] = 0;
    memcpy(his, comma + 1, hlen); his[hlen] = 0;

    if (strcmp(los, "*") == 0) {
      flags |= FILTER_IV_LO_ANY;
    } else if (!parseIvNum(los, snr_mode, &iv.lo)) {
      return false;
    }
    if (strcmp(his, "*") == 0) {
      flags |= FILTER_IV_HI_ANY;
    } else if (!parseIvNum(his, snr_mode, &iv.hi)) {
      return false;
    }
    if ((flags & FILTER_IV_LO_ANY) && (flags & FILTER_IV_HI_ANY)) {
      return true;   // (*,*) = "any": leave predicate unset
    }
    iv.flags = flags;
    return true;
  }

  // bare value = exact
  if (!parseIvNum(tok, snr_mode, &iv.lo)) return false;
  iv.hi = iv.lo;
  iv.flags = FILTER_IV_LO_INC | FILTER_IV_HI_INC;
  return true;
}

static void formatInterval(const Interval& iv, char* dest, size_t sz, bool snr_mode) {
  char lo[16], hi[16];
  if (iv.flags & FILTER_IV_LO_ANY) strcpy(lo, "*");
  else if (snr_mode) snprintf(lo, sizeof(lo), "%.2f", (float)(int16_t)iv.lo / 4.0f);
  else snprintf(lo, sizeof(lo), "%u", iv.lo);
  if (iv.flags & FILTER_IV_HI_ANY) strcpy(hi, "*");
  else if (snr_mode) snprintf(hi, sizeof(hi), "%.2f", (float)(int16_t)iv.hi / 4.0f);
  else snprintf(hi, sizeof(hi), "%u", iv.hi);
  snprintf(dest, sz, "%c%s,%s%c",
           (iv.flags & FILTER_IV_LO_INC) ? '[' : '(',
           lo, hi,
           (iv.flags & FILTER_IV_HI_INC) ? ']' : ')');
}

// hex hash: 2..8 hex chars -> up to 4 leading bytes of a repeater pubkey hash
static bool parseHexHash(const char* s, uint8_t* out, uint8_t* out_len) {
  size_t n = strlen(s);
  if (n < 2 || n > 8 || (n & 1)) return false;
  for (size_t i = 0; i < n; i += 2) {
    int hi = hexVal(s[i]), lo = hexVal(s[i + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i / 2] = (uint8_t)((hi << 4) | lo);
  }
  *out_len = (uint8_t)(n / 2);
  return true;
}

// path=^HEX>HEX>...>HEX$  (up to FILTER_PATH_HASH_SLOTS adjacent hashes)
static bool parsePath(const char* tok, FilterRule* r) {
  r->path.count = 0;
  r->path.pos = FILTER_PATH_ANY;
  const char* s = tok;
  if (*s == '^') { r->path.pos = FILTER_PATH_FIRST; s++; }

  char buf[4 * 9];
  size_t tlen = strlen(s);
  if (tlen == 0 || tlen >= sizeof(buf)) return false;
  memcpy(buf, s, tlen + 1);
  if (buf[tlen - 1] == '$') {
    r->path.pos = FILTER_PATH_LAST;
    buf[tlen - 1] = 0;
  }

  char* sp = buf;
  char* seg;
  while ((seg = strsep(&sp, ">")) != NULL) {
    if (r->path.count >= FILTER_PATH_HASH_SLOTS) return false;
    uint8_t len;
    if (!parseHexHash(seg, r->path.bytes[r->path.count], &len)) return false;
    r->path.len[r->path.count] = len;
    r->path.count++;
  }
  return true;
}

// FNV-1a over the rule's predicate fields (a short display digest for `filter list`)
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

// ---------------------------------------------------------------- filter add

static bool setPattern(char* dest, size_t dest_sz, const char* pattern) {
  if (strlen(pattern) >= dest_sz) return false;    // reject too-long, never truncate a regex
  strcpy(dest, pattern);
  return re_compile(pattern) != 0;                 // validate syntax at add time
}

static bool addRuleParam(FilterRules& filter, FilterRule* r, const char* key, const char* val,
                         char* reply) {
  if (strcmp(key, "chan") == 0) {
    char names[80];
    if (strlen(val) >= sizeof(names)) { strcpy(reply, "Err - chan list too long"); return false; }
    strcpy(names, val);
    char* np = names;
    char* nm;
    while ((nm = strsep(&np, ",")) != NULL) {
      if (nm[0] == 0) { strcpy(reply, "Err - empty chan name"); return false; }
      FilterChannel* ch = filter.findChannel(nm);
      if (ch == NULL && nm[0] == '#') {
        ch = filter.addChannel(nm, NULL);   // auto-provision '#' names with derived PSK
      }
      if (ch == NULL) { sprintf(reply, "Err - unknown chan '%s'", nm); return false; }
      int idx = ch - filter.getChannel(0);
      r->chan_mask |= (1 << idx);
      r->chan_flags |= FILTER_CHANFLG_MASK_SET;
    }
    return true;
  }
  if (strcmp(key, "chanhash") == 0) {
    uint8_t len;
    if (!parseHexHash(val, &r->chan_hash, &len) || len != 1) {
      strcpy(reply, "Err - chanhash must be 2 hex chars");
      return false;
    }
    r->chan_flags |= FILTER_CHANFLG_HASH_SET;
    return true;
  }
  if (strcmp(key, "type") == 0) {
    char vals[24];
    if (strlen(val) >= sizeof(vals)) { strcpy(reply, "Err - bad type"); return false; }
    strcpy(vals, val);
    char* vp = vals;
    char* t;
    while ((t = strsep(&vp, ",")) != NULL) {
      if (strcmp(t, "advert") == 0) r->type_mask |= FILTER_TYPE_ADVERT;
      else if (strcmp(t, "txt") == 0) r->type_mask |= FILTER_TYPE_GRP_TXT;
      else if (strcmp(t, "data") == 0) r->type_mask |= FILTER_TYPE_GRP_DATA;
      else if (strcmp(t, "any") == 0) { /* leave mask unset */ }
      else { sprintf(reply, "Err - unknown type '%s'", t); return false; }
    }
    return true;
  }
  if (strcmp(key, "route") == 0) {
    if (strcmp(val, "flood") == 0) r->route_mask = FILTER_ROUTE_FLOOD;
    else if (strcmp(val, "direct") == 0) r->route_mask = FILTER_ROUTE_DIRECT;
    else { strcpy(reply, "Err - route must be flood|direct"); return false; }
    return true;
  }
  if (strcmp(key, "hops") == 0) {
    if (!parseInterval(val, r->hops, false)) { strcpy(reply, "Err - bad hops interval"); return false; }
    return true;
  }
  if (strcmp(key, "len") == 0) {
    if (!parseInterval(val, r->len, false)) { strcpy(reply, "Err - bad len interval"); return false; }
    return true;
  }
  if (strcmp(key, "snr") == 0) {
    if (!parseInterval(val, r->snr, true)) { strcpy(reply, "Err - bad snr interval"); return false; }
    return true;
  }
  if (strcmp(key, "path") == 0) {
    if (!parsePath(val, r) || r->path.count == 0) {
      strcpy(reply, "Err - bad path spec");
      return false;
    }
    return true;
  }
  if (strcmp(key, "hsize") == 0) {
    char vals[12];
    if (strlen(val) >= sizeof(vals)) { strcpy(reply, "Err - bad hsize"); return false; }
    strcpy(vals, val);
    char* vp = vals;
    char* t;
    while ((t = strsep(&vp, ",")) != NULL) {
      int v = atoi(t);
      if (v < 1 || v > 4) { strcpy(reply, "Err - hsize values are 1..4"); return false; }
      r->hash_size_mask |= (1 << (v - 1));
    }
    return true;
  }
  if (strcmp(key, "sender") == 0) {
    if (!setPattern(r->sender, FILTER_SENDER_PATTERN_LEN, val)) {
      strcpy(reply, "Err - bad/long sender regex");
      return false;
    }
    return true;
  }
  if (strcmp(key, "text") == 0) {
    if (!setPattern(r->text, FILTER_TEXT_PATTERN_LEN, val)) {
      strcpy(reply, "Err - bad/long text regex");
      return false;
    }
    return true;
  }
  if (strcmp(key, "action") == 0) {
    if (strcmp(val, "drop") == 0) r->action = FILTER_ACT_DROP;
    else if (strcmp(val, "logonly") == 0) r->action = FILTER_ACT_LOG_ONLY;
    else { strcpy(reply, "Err - action must be drop|logonly"); return false; }
    return true;
  }
  sprintf(reply, "Err - unknown param '%s'", key);
  return false;
}

// ---------------------------------------------------------------- CLI commands

static void cliStatus(FilterRules& filter, char* reply) {
  int used = 0;
  for (int i = 0; i < filter.getNumChannels(); i++) {
    if (filter.getChannel(i)->name[0] != 0) used++;
  }
  char* out = reply;
  int remain = MAX_PACKET_PAYLOAD;
  radd(&out, &remain, "%s; rules %d/%d; chans %d/%d; ratelimit advert %uh; cache %d/%d",
       filter.isEnabled() ? "on" : "off", filter.getNumRules(), FILTER_MAX_RULES,
       used, FILTER_MAX_CHANNELS, filter.getAdvertRatelimit(),
       filter.getAdvertRatelimit() ? filter.getAdvertCacheCount() : 0, FILTER_ADVERT_CACHE_SIZE);
  radd(&out, &remain, "; limiter %lu; aborted %lu", (unsigned long)filter.getLimiterDrops(),
       (unsigned long)filter.getBudgetAborts());
}

static void cliChanList(FilterRules& filter, char* reply) {
  char* out = reply;
  int remain = MAX_PACKET_PAYLOAD;
  if (filter.getNumChannels() == 0) {
    strcpy(reply, "no channels");
    return;
  }
  for (int i = 0; i < filter.getNumChannels(); i++) {
    auto ch = filter.getChannel(i);
    radd(&out, &remain, "%s%d:%s k%u h%02X%s", i ? " " : "", i, ch->name,
         ch->secret_len, ch->hash[0], ch->secret_len == 16 && ch->name[0] == '#' ? " d" : "");
  }
}

static void cliChanAdd(FilterRules& filter, char* params, char* reply) {
  char* p = params;
  char* name = nextToken(&p);
  char* psk = nextToken(&p);
  if (name == NULL) { strcpy(reply, "Err - usage: filter chan add <name> [<psk-b64>]"); return; }
  if (strlen(name) >= FILTER_CHAN_NAME_LEN) { strcpy(reply, "Err - name too long"); return; }
  if (filter.findChannel(name) != NULL) { strcpy(reply, "Err - channel exists"); return; }
  if ((psk == NULL || psk[0] == 0) && name[0] != '#') {
    strcpy(reply, "Err - psk required for non-# names");
    return;
  }
  auto ch = filter.addChannel(name, psk);
  if (ch == NULL) { strcpy(reply, "Err - bad psk or store full"); return; }
  sprintf(reply, "OK - chan %s h=%02X%s", ch->name, ch->hash[0],
          psk == NULL ? " (derived)" : "");
}

static void cliChanDel(FilterRules& filter, char* params, char* reply) {
  char* p = params;
  char* name = nextToken(&p);
  if (name == NULL) { strcpy(reply, "Err - usage: filter chan del <name>"); return; }
  auto ch = filter.findChannel(name);
  if (ch == NULL) { strcpy(reply, "Err - unknown channel"); return; }
  filter.delChannel(ch - filter.getChannel(0));
  sprintf(reply, "OK - chan %s deleted", name);
}

static void cliAdd(FilterRules& filter, char* params, char* reply) {
  FilterRule* r = filter.addRule();
  if (r == NULL) { strcpy(reply, "Err - rule list full"); return; }
  int idx = filter.getNumRules() - 1;

  char* p = params;
  char* tok;
  while ((tok = nextToken(&p)) != NULL) {
    char* eq = strchr(tok, '=');
    if (eq == NULL) {
      sprintf(reply, "Err - expected key=value, got '%s'", tok);
      filter.delRule(idx);
      return;
    }
    *eq = 0;
    if (!addRuleParam(filter, r, tok, eq + 1, reply)) {
      filter.delRule(idx);   // roll back the half-added rule
      return;
    }
  }
  sprintf(reply, "OK - rule %d added", idx);
}

static void cliList(FilterRules& filter, char* reply) {
  char* out = reply;
  int remain = MAX_PACKET_PAYLOAD;
  radd(&out, &remain, "%s %d/%d:", filter.isEnabled() ? "on" : "off",
       filter.getNumRules(), FILTER_MAX_RULES);
  for (int i = 0; i < filter.getNumRules(); i++) {
    auto r = filter.getRule(i);
    radd(&out, &remain, " %d%c%c%03X", i, r->enabled ? 'e' : 'd',
         r->action == FILTER_ACT_DROP ? 'D' : 'L', ruleDigest(r) & 0xFFF);
  }
}

static void cliGet(FilterRules& filter, int idx, char* reply) {
  if (idx < 0 || idx >= filter.getNumRules()) { strcpy(reply, "Err - no such rule"); return; }
  auto r = filter.getRule(idx);
  char* out = reply;
  int remain = MAX_PACKET_PAYLOAD;
  radd(&out, &remain, "r%d %s %s", idx, r->enabled ? "en" : "dis",
       r->action == FILTER_ACT_DROP ? "drop" : "logonly");

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
  if (r->hops.flags) { formatInterval(r->hops, ivs, sizeof(ivs), false); radd(&out, &remain, " hops=%s", ivs); }
  if (r->len.flags) { formatInterval(r->len, ivs, sizeof(ivs), false); radd(&out, &remain, " len=%s", ivs); }
  if (r->snr.flags) { formatInterval(r->snr, ivs, sizeof(ivs), true); radd(&out, &remain, " snr=%s", ivs); }
  if (r->path.count) {
    radd(&out, &remain, " path=%s", r->path.pos == FILTER_PATH_FIRST ? "^" : "");
    for (int e = 0; e < r->path.count; e++) {
      radd(&out, &remain, "%s", e ? ">" : "");
      for (int b = 0; b < r->path.len[e]; b++) radd(&out, &remain, "%02X", r->path.bytes[e][b]);
    }
    radd(&out, &remain, "%s", r->path.pos == FILTER_PATH_LAST ? "$" : "");
  }
  if (r->hash_size_mask) {
    radd(&out, &remain, " hsize=");
    const char* sep = "";
    for (int s = 1; s <= 4; s++) {
      if (r->hash_size_mask & (1 << (s - 1))) { radd(&out, &remain, "%s%d", sep, s); sep = ","; }
    }
  }
  if (r->chan_flags & FILTER_CHANFLG_MASK_SET) {
    radd(&out, &remain, " chan=");
    const char* sep = "";
    for (int c = 0; c < filter.getNumChannels(); c++) {
      if (r->chan_mask & (1 << c)) { radd(&out, &remain, "%s%s", sep, filter.getChannel(c)->name); sep = ","; }
    }
  }
  if (r->chan_flags & FILTER_CHANFLG_HASH_SET) radd(&out, &remain, " chanhash=%02X", r->chan_hash);
  if (r->sender[0]) radd(&out, &remain, " sender=%s", r->sender);
  if (r->text[0]) radd(&out, &remain, " text=%s", r->text);
  radd(&out, &remain, " hits=%lu", (unsigned long)r->hits);
}

static void cliStats(FilterRules& filter, char* reply) {
  char* out = reply;
  int remain = MAX_PACKET_PAYLOAD;
  radd(&out, &remain, "hits:");
  for (int i = 0; i < filter.getNumRules(); i++) {
    radd(&out, &remain, " %d:%lu", i, (unsigned long)filter.getRule(i)->hits);
  }
  radd(&out, &remain, "; limiter:%lu aborted:%lu", (unsigned long)filter.getLimiterDrops(),
       (unsigned long)filter.getBudgetAborts());
}

static bool cliRuleIdx(FilterRules& filter, char* arg, int& idx, char* reply) {
  if (arg == NULL || arg[0] == 0) { strcpy(reply, "Err - rule index required"); return false; }
  idx = atoi(arg);
  if (idx < 0 || idx >= filter.getNumRules()) { strcpy(reply, "Err - no such rule"); return false; }
  return true;
}

void filterCLI(FilterRules& filter, const char* command, char* reply) {
  char buf[MAX_PACKET_PAYLOAD + 1];
  StrHelper::strzcpy(buf, command, sizeof(buf));
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
    cliAdd(filter, p, reply);
  } else if (strcmp(cmd, "list") == 0) {
    cliList(filter, reply);
  } else if (strcmp(cmd, "get") == 0) {
    int idx;
    if (cliRuleIdx(filter, nextToken(&p), idx, reply)) cliGet(filter, idx, reply);
  } else if (strcmp(cmd, "enable") == 0) {
    int idx;
    if (cliRuleIdx(filter, nextToken(&p), idx, reply)) {
      filter.getRule(idx)->enabled = true;
      filter.markDirty();
      sprintf(reply, "OK - rule %d enabled", idx);
    }
  } else if (strcmp(cmd, "disable") == 0) {
    int idx;
    if (cliRuleIdx(filter, nextToken(&p), idx, reply)) {
      filter.getRule(idx)->enabled = false;
      filter.markDirty();
      sprintf(reply, "OK - rule %d disabled", idx);
    }
  } else if (strcmp(cmd, "del") == 0) {
    int idx;
    if (cliRuleIdx(filter, nextToken(&p), idx, reply)) {
      filter.delRule(idx);
      sprintf(reply, "OK - rule %d deleted", idx);
    }
  } else if (strcmp(cmd, "clear") == 0) {
    filter.clearRules();
    strcpy(reply, "OK - rules cleared");
  } else if (strcmp(cmd, "ratelimit") == 0) {
    char* sub = nextToken(&p);
    if (sub == NULL) {
      sprintf(reply, "ratelimit advert %uh; cache %d/%d", filter.getAdvertRatelimit(),
              filter.getAdvertCacheCount(), FILTER_ADVERT_CACHE_SIZE);
    } else if (strcmp(sub, "advert") == 0) {
      char* hs = nextToken(&p);
      long v = hs ? strtol(hs, NULL, 10) : -1;
      if (v < 0 || v > FILTER_ADVERT_HOURS_MAX) {
        sprintf(reply, "Err - hours must be 0..%d (0=off)", FILTER_ADVERT_HOURS_MAX);
      } else {
        filter.setAdvertRatelimit((uint16_t)v);
        sprintf(reply, "OK - advert ratelimit %ldh", v);
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
    strcpy(reply, "Err - usage: on|off|add|list|get|enable|disable|del|clear|chan|ratelimit|stats");
  }
}