// AdvertRateLimiter.h — per-node advert repeat window for the simple_repeater
// packet filter: "an advert for each origin at most once every N hours".
// Personal fork feature, forked out of FilterRules so the window's cache,
// counters and CLI-facing state have one home (see PacketFilter.h for the rule
// model).
//
// Core guarantees the forward hook sees each unique, signature-verified advert
// exactly once, so each origin is recorded exactly once per window. The window
// state is RAM-only: it starts empty on every boot, so the first advert from
// an origin after a reboot is always relayed.

#ifndef _ADVERT_RATE_LIMITER_H
#define _ADVERT_RATE_LIMITER_H

#include <Arduino.h>
#include <Mesh.h>

#include <string.h>

#include "PacketFilterConfig.h"

struct AdvertSeenEntry {      // RAM-only; cleared on reboot
  uint8_t  pub_key_prefix[4]; // 4 pubkey bytes sampled at fixed offsets (see
                              // AdvertRateLimiter::drop); collision odds ~0.001%
                              // per 256 distinct nodes, vanity-robust; worst
                              // case is one falsely suppressed advert/window
  uint32_t first_seen_millis; // by this repeater's own monotonic clock
};

class AdvertRateLimiter {
  AdvertSeenEntry cache[FILTER_ADVERT_CACHE_SIZE];
  int cache_count;     // number of used entries (0..FILTER_ADVERT_CACHE_SIZE)
  int cache_head;      // ring head (oldest entry) once the cache is full
  uint16_t hours;      // per-node advert repeat window; 0 = off
  uint32_t drops;      // adverts dropped by the limiter (RAM-only)

public:
  AdvertRateLimiter();

  uint16_t getHours() const { return hours; }
  void setHours(uint16_t h) { hours = h; }
  bool enabled() const { return hours > 0; }
  int getCacheCount() const { return cache_count; }
  uint32_t getDrops() const { return drops; }
  void clearCache();
  void resetDrops() { drops = 0; }

  // back to a freshly constructed limiter (empty cache, window off, no drops)
  void reset() {
    memset(cache, 0, sizeof(cache));
    cache_count = 0;
    cache_head = 0;
    hours = 0;
    drops = 0;
  }

  // True if this advert must be dropped because the same origin was already
  // relayed less than `hours` ago (counting that drop). Drops nothing while the
  // limiter is off (hours == 0) or the origin is seen for the first time.
  bool drop(const mesh::Packet* pkt, uint32_t now_millis);
};

#endif // _ADVERT_RATE_LIMITER_H
