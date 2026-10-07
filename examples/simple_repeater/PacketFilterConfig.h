// PacketFilterConfig.h — build-time tunables for the repeater packet filter.
//
// All capacity numbers live here so they can be tuned per build without
// touching any other file. Every value has an #ifndef default; override via
// build_flags (e.g. in platformio.local.ini: -DFILTER_MAX_RULES=32) or by
// editing the defaults below.
//
// NOTE: FILTER_PATH_HASH_SLOTS, FILTER_REGION_LIST_LEN, FILTER_SENDER_PATTERN_LEN
// and FILTER_TEXT_PATTERN_LEN are not just capacities: all four live inside
// FilterRule, so changing any of them moves every field after it and a config
// written by one build would not load in another. PacketFilter.h static_asserts
// refuse to build when that happens; raising one is a layout change that needs
// a FILTER_CFG_VERSION bump plus migration code in load(), not an edit to this
// file alone.

#ifndef _PACKET_FILTER_CONFIG_H
#define _PACKET_FILTER_CONFIG_H

#ifndef FILTER_MAX_RULES
  #define FILTER_MAX_RULES 32          // rule slots (~216 B each, incl. patterns)
#endif

#ifndef FILTER_MAX_CHANNELS
  #define FILTER_MAX_CHANNELS 32       // keyed channel store entries
#endif

#ifndef FILTER_CHAN_NAME_LEN
  #define FILTER_CHAN_NAME_LEN 16      // channel name storage length (NUL incl.)
#endif

#ifndef FILTER_ADVERT_CACHE_SIZE
  #define FILTER_ADVERT_CACHE_SIZE 512 // advert rate-limit cache entries
#endif

#ifndef FILTER_SENDER_PATTERN_LEN
  #define FILTER_SENDER_PATTERN_LEN 32 // sender regex storage length (NUL incl.)
#endif

#ifndef FILTER_TEXT_PATTERN_LEN
  #define FILTER_TEXT_PATTERN_LEN 64   // text regex storage length (NUL incl.)
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

// The following are RAM-only capacities (CliScriptRunner state): changing them
// moves no persisted layout, so unlike the rule-model knobs above they need no
// static_assert and no FILTER_CFG_VERSION bump.
#ifndef FILTER_CLI_KEY_LEN
  #define FILTER_CLI_KEY_LEN 32        // !id key token max length (validated, then hashed)
#endif

#ifndef FILTER_CLI_SEEN_SIZE
  #define FILTER_CLI_SEEN_SIZE 32      // seen-key hash ring entries (8 B each)
#endif

#ifndef FILTER_CLI_QUEUE_DEPTH
  #define FILTER_CLI_QUEUE_DEPTH 2     // pending scripts; enqueue refuses when full
#endif

#ifndef FILTER_CLI_LINE_MAX
  #define FILTER_CLI_LINE_MAX 160      // script lines at least this long are skipped:
                                       // the serial CLI's own command buffer is this size
#endif

#endif // _PACKET_FILTER_CONFIG_H
