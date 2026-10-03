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
  uint64_t first_seen_millis; // on this repeater's own 64-bit monotonic clock
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
  // RELAYED less than `hours` ago. This only answers the question — it records
  // nothing, because whether an advert actually gets relayed is decided after the
  // stock forwarding checks. Recording here would spend a node's 48 h budget on
  // adverts that never got forwarded: a repeater left in `set off`, or refusing
  // them for hop-limit/region/loop reasons, would silently stop relaying those
  // nodes for the rest of the window.
  bool wouldDrop(const mesh::Packet* pkt, uint64_t now_millis);

  // Record that this origin's advert was admitted for relay, starting its window.
  // Called from the successful end of the forwarding hook, never from the check.
  void recordForward(const mesh::Packet* pkt, uint64_t now_millis);

  // Restart an origin's expired window. Not used by the forwarding path — the
  // stamp is refreshed by recordForward() on admission — but the expiry semantics
  // are easier to state (and to test) as their own step.
  bool refreshExpired(const mesh::Packet* pkt, uint64_t now_millis);

  // Drop the cache entirely.
  void clearCache();

private:
  void clearCacheImpl();
  // index of the cache slot holding this origin's 4-byte key, or -1
  int findOrigin(const mesh::Packet* pkt) const;
  // store this origin at `now`, evicting the oldest entry if the cache is full
  void storeOrigin(const mesh::Packet* pkt, uint64_t now_millis);
};

#endif // _ADVERT_RATE_LIMITER_H
