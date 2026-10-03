// AdvertRateLimiter.cpp — see AdvertRateLimiter.h for the design overview.

#include "AdvertRateLimiter.h"

// cache key: 4 bytes sampled from the origin pubkey (payload[0..31]) at fixed
// offsets clear of the vanity zones at both ends — prefix/suffix grinding leaves
// the middle bytes uniformly random, so two vanity keys collide with plain
// random-chance odds instead of deterministically
static const uint8_t KEY_OFFSETS[4] = { 8, 14, 20, 26 };

AdvertRateLimiter::AdvertRateLimiter() {
  reset();
}

void AdvertRateLimiter::clearCache() {
  memset(cache, 0, sizeof(cache));
  cache_count = 0;
  cache_head = 0;
}

int AdvertRateLimiter::findOrigin(const mesh::Packet* pkt) const {
  uint8_t prefix[4];
  for (int i = 0; i < 4; i++) prefix[i] = pkt->payload[KEY_OFFSETS[i]];
  // ring start and total do not change while scanning: entries are appended until
  // the cache is full, then the oldest is overwritten
  const bool wrapped = cache_count >= FILTER_ADVERT_CACHE_SIZE;
  const int total = wrapped ? FILTER_ADVERT_CACHE_SIZE : cache_count;
  const int start = wrapped ? cache_head : 0;
  for (int i = 0; i < total; i++) {
    const int idx = (start + i) % FILTER_ADVERT_CACHE_SIZE;
    if (memcmp(cache[idx].pub_key_prefix, prefix, sizeof(prefix)) == 0) return idx;
  }
  return -1;
}

void AdvertRateLimiter::storeOrigin(const mesh::Packet* pkt, uint64_t now_millis) {
  AdvertSeenEntry* e;
  if (cache_count < FILTER_ADVERT_CACHE_SIZE) {
    e = &cache[cache_count++];
  } else {
    e = &cache[cache_head];   // overwrite the oldest entry
    cache_head = (cache_head + 1) % FILTER_ADVERT_CACHE_SIZE;
  }
  for (int i = 0; i < 4; i++) e->pub_key_prefix[i] = pkt->payload[KEY_OFFSETS[i]];
  e->first_seen_millis = now_millis;
}

bool AdvertRateLimiter::wouldDrop(const mesh::Packet* pkt, uint64_t now_millis) {
  if (hours == 0) return false;   // limiter off: nothing is ever suppressed
  const uint64_t window_ms = (uint64_t)hours * 3600ULL * 1000ULL;
  const int idx = findOrigin(pkt);
  // not relayed in this window, or the window has expired: let it through. The
  // stamp is refreshed on admission (recordForward), not here.
  if (idx < 0) return false;
  if (now_millis >= cache[idx].first_seen_millis &&
      now_millis - cache[idx].first_seen_millis >= window_ms) return false;
  drops++;
  return true;   // too soon: suppress the repeat
}

void AdvertRateLimiter::recordForward(const mesh::Packet* pkt, uint64_t now_millis) {
  if (hours == 0) return;   // limiter off: keep no history at all
  const int idx = findOrigin(pkt);
  if (idx >= 0) cache[idx].first_seen_millis = now_millis;   // refresh the window
  else storeOrigin(pkt, now_millis);
}
