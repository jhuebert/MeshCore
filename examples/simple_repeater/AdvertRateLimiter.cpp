// AdvertRateLimiter.cpp — see AdvertRateLimiter.h for the design overview.

#include "AdvertRateLimiter.h"

AdvertRateLimiter::AdvertRateLimiter() {
  reset();
}

void AdvertRateLimiter::clearCache() {
  memset(cache, 0, sizeof(cache));
  cache_count = 0;
  cache_head = 0;
}

bool AdvertRateLimiter::drop(const mesh::Packet* pkt, uint32_t now_millis) {
  if (hours == 0) return false;   // limiter off: nothing is ever suppressed
  uint32_t window_ms = (uint32_t)hours * 3600UL * 1000UL;

  // cache key: 4 bytes sampled from the origin pubkey (payload[0..31]) at fixed
  // offsets clear of the vanity zones at both ends — prefix/suffix grinding
  // leaves the middle bytes uniformly random, so two vanity keys collide with
  // plain random-chance odds instead of deterministically
  static const uint8_t KEY_OFFSETS[4] = { 8, 14, 20, 26 };
  uint8_t prefix[4];
  for (int i = 0; i < 4; i++) prefix[i] = pkt->payload[KEY_OFFSETS[i]];

  // ring start/total do not change while scanning: entries are appended until
  // the cache is full, then the oldest is overwritten
  const bool wrapped = cache_count >= FILTER_ADVERT_CACHE_SIZE;
  const int total = wrapped ? FILTER_ADVERT_CACHE_SIZE : cache_count;
  const int start = wrapped ? cache_head : 0;
  for (int i = 0; i < total; i++) {
    AdvertSeenEntry* e = &cache[(start + i) % FILTER_ADVERT_CACHE_SIZE];
    if (memcmp(e->pub_key_prefix, prefix, sizeof(e->pub_key_prefix)) == 0) {
      if (now_millis - e->first_seen_millis < window_ms) {
        drops++;
        return true;   // too soon: drop the repeat
      }
      e->first_seen_millis = now_millis;   // refresh: window restarts
      return false;
    }
  }

  // not seen in this window: record
  AdvertSeenEntry* e;
  if (!wrapped) {
    e = &cache[cache_count++];
  } else {
    e = &cache[cache_head];   // overwrite the oldest entry
    cache_head = (cache_head + 1) % FILTER_ADVERT_CACHE_SIZE;
  }
  memcpy(e->pub_key_prefix, prefix, sizeof(e->pub_key_prefix));
  e->first_seen_millis = now_millis;
  return false;
}
