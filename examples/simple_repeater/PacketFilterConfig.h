// PacketFilterConfig.h — build-time tunables for the repeater packet filter.
//
// All capacity numbers live here so they can be tuned per build without
// touching any other file. Every value has an #ifndef default; override via
// build_flags (e.g. in platformio.local.ini: -DFILTER_MAX_RULES=32) or by
// editing the defaults below.
//
// NOTE: FilterRule::chan_mask is 16 bits wide; if FILTER_MAX_CHANNELS is
// raised above 16, widen chan_mask accordingly (see PacketFilter.h).
//
// NOTE: FILTER_PATH_HASH_SLOTS and FILTER_REGION_LIST_LEN are not just
// capacities: both live inside FilterRule, so changing either moves every
// field after it and a config written by one build would not load in another.
// PacketFilter.h static_asserts refuse to build when that happens; raising one
// is a layout change that needs a FILTER_CFG_VERSION bump plus migration code
// in load(), not an edit to this file alone.

#ifndef _PACKET_FILTER_CONFIG_H
#define _PACKET_FILTER_CONFIG_H

#ifndef FILTER_MAX_RULES
  #define FILTER_MAX_RULES 16          // rule slots (~180 B each, incl. patterns)
#endif

#ifndef FILTER_MAX_CHANNELS
  #define FILTER_MAX_CHANNELS 16       // keyed channel store entries
#endif

#ifndef FILTER_CHAN_NAME_LEN
  #define FILTER_CHAN_NAME_LEN 16      // channel name storage length (NUL incl.)
#endif

#ifndef FILTER_ADVERT_CACHE_SIZE
  #define FILTER_ADVERT_CACHE_SIZE 256 // advert rate-limit cache entries
#endif

#ifndef FILTER_SENDER_PATTERN_LEN
  #define FILTER_SENDER_PATTERN_LEN 24 // sender regex storage length (NUL incl.)
#endif

#ifndef FILTER_TEXT_PATTERN_LEN
  #define FILTER_TEXT_PATTERN_LEN 48   // text regex storage length (NUL incl.)
#endif

#ifndef FILTER_PATTERN_MAX_ALTS
  // Alternatives ('|'-separated) allowed per sender=/text= pattern. A parse
  // limit, not a stored field: it does not affect the persisted record layout.
  #define FILTER_PATTERN_MAX_ALTS 8
#endif

#ifndef FILTER_REGION_LIST_LEN
  #define FILTER_REGION_LIST_LEN 32    // region= comma list storage length (NUL incl.)
#endif

#ifndef FILTER_PATH_HASH_SLOTS
  #define FILTER_PATH_HASH_SLOTS 4     // path chain hashes per rule (4 B each)
#endif

#endif // _PACKET_FILTER_CONFIG_H
