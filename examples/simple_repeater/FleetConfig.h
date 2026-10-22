// FleetConfig.h — build-time tunables for the repeater fleet manager
// (FleetManager.h/.cpp + CliScript.h/.cpp).
//
// All capacity numbers live here so they can be tuned per build without
// touching any other file. Every value has an #ifndef default; override via
// build_flags (e.g. in platformio.local.ini: -DFLEET_MAX_TAGS=16) or by
// editing the defaults below.
//
// NOTE: FLEET_MAX_TAGS and FLEET_TAG_LEN live inside the persisted /fleet_cfg
// record, so changing either moves the reply_window_ms field after it and a
// config written by one build would not load in another. FleetManager.h
// static_asserts refuse to build when that happens; raising one is a layout
// change that needs a FLEET_CFG_VERSION bump plus migration code in load(),
// not an edit to this file alone.

#ifndef _FLEET_CONFIG_H
#define _FLEET_CONFIG_H

#include "CliUtil.h"   // CLI_REPLY_MAX: the reply the fleet keeps is a CLI reply

// tag store capacities (persisted in /fleet_cfg — see note above)
#ifndef FLEET_MAX_TAGS
  #define FLEET_MAX_TAGS 8            // tag slots on this repeater
#endif

#ifndef FLEET_TAG_LEN
  #define FLEET_TAG_LEN 16            // tag storage length (NUL incl.)
#endif

// The following are RAM-only capacities (CliScriptRunner state): changing them
// moves no persisted layout, so unlike the tag knobs above they need no
// static_assert and no FLEET_CFG_VERSION bump.
#ifndef FLEET_CLI_KEY_LEN
  #define FLEET_CLI_KEY_LEN 32        // !id key token max length (validated, then hashed)
#endif

#ifndef FLEET_CLI_SEEN_SIZE
  #define FLEET_CLI_SEEN_SIZE 32      // seen-key hash ring entries (8 B each)
#endif

#ifndef FLEET_CLI_QUEUE_DEPTH
  #define FLEET_CLI_QUEUE_DEPTH 2     // pending scripts; enqueue refuses when full
#endif

#ifndef FLEET_CLI_LINE_MAX
  #define FLEET_CLI_LINE_MAX 160      // script lines at least this long are skipped:
                                      // the serial CLI's own command buffer is this size
#endif

// --- script grammar limits (parse-time only; nothing persisted) ---

#ifndef FLEET_MAX_SCRIPT_TAGS
  #define FLEET_MAX_SCRIPT_TAGS 8     // tags accepted per !tags list (a parse limit)
#endif

#ifndef FLEET_DELAY_MAX_MS
  #define FLEET_DELAY_MAX_MS 300000   // !delay cap (5 min): bounds how long one
                                      // script can hold a run-queue slot
#endif

#ifndef FLEET_AT_STALE_SECS
  #define FLEET_AT_STALE_SECS 300     // a !at timestamp older than this is refused:
                                      // grace for sender/node clock skew + mesh transit
#endif

#ifndef FLEET_AT_CLOCK_FLOOR
  #define FLEET_AT_CLOCK_FLOOR 1735689600UL   // 2025-01-01 UTC: an RTC below this
                                              // counts as unset; !at scripts are
                                              // refused rather than run at an
                                              // unknown time
#endif

// --- acknowledgements ---

#ifndef FLEET_REPLY_WINDOW_MS
  #define FLEET_REPLY_WINDOW_MS 15000UL   // !ack reply jitter window (default):
                                          // sized for a small fleet; widen with
                                          // `fleet reply <secs>` as the fleet grows
                                          // (rule of thumb: ~5 s per repeater)
#endif

#ifndef FLEET_REPLY_WINDOW_MAX_MS
  #define FLEET_REPLY_WINDOW_MAX_MS 600000UL  // fleet reply <secs> cap (600 s)
#endif

#ifndef FLEET_REPLY_STORE
  #define FLEET_REPLY_STORE 2         // pending acknowledgements (RAM-only)
#endif

#ifndef FLEET_SCHED_STORE
  #define FLEET_SCHED_STORE 2         // armed !at scripts (RAM-only)
#endif

// --- acknowledgement repeat confirmation ---
//
// A reply is re-sent once, byte-identically, when no repeater is heard
// repeating it within FLEET_ACK_REPEAT_WINDOW_MS. Same frozen timestamp =>
// same packet hash, so every node that already relayed the first attempt
// suppresses the copy at the mesh layer: the retry can never duplicate on air.
// RAM-only, so no FLEET_CFG_VERSION interaction.

#ifndef FLEET_ACK_RETRY
  #define FLEET_ACK_RETRY 1                   // resend an !ack reply once,
                                              // byte-identically, if no repeater
                                              // is heard repeating it;
                                              // 0 = today's fire-and-forget
#endif

#ifndef FLEET_ACK_REPEAT_WINDOW_MS
  #define FLEET_ACK_REPEAT_WINDOW_MS 5000UL   // wait this long for a repeater to
                                              // repeat the reply before resending.
                                              // Covers a relay's calcRxDelay, not
                                              // just a good-link echo
#endif

#ifndef FLEET_ACK_RETRY_BACKOFF_MS
  #define FLEET_ACK_RETRY_BACKOFF_MS 1000UL   // re-try delay when the packet pool
                                              // refused the first send (it used
                                              // no attempt)
#endif

// How many attempts a refused acknowledgement send gets before the reply is
// dropped and counted as refused. The packet pool refuses a send when the mesh
// is saturated, and a reply that never leaves must not hold its store slot
// forever: two of them would block every later acknowledgement until a reboot.
#ifndef FLEET_ACK_SEND_MAX_ATTEMPTS
  #define FLEET_ACK_SEND_MAX_ATTEMPTS 3
#endif

// The data limit one group datagram can carry (createGroupDatagram refuses
// more): MAX_PACKET_PAYLOAD (184) - 1 B channel hash - 15 B cipher padding.
// Acknowledgement messages are composed inside this bound.
#ifndef FLEET_REPLY_DATA_MAX
  #define FLEET_REPLY_DATA_MAX 168
#endif

// The longest node name a reply can carry: CommonCLI keeps the name in
// node_name[32], so 31 characters plus the NUL.
#ifndef FLEET_MAX_NODE_NAME
  #define FLEET_MAX_NODE_NAME 31
#endif

// What a reply carries besides its own text: the group-text header (the
// 4-byte timestamp plus the 1-byte text type) and the "<name>: " prefix the
// sender puts in front of it. Both come out of the same datagram budget, so
// the room the reply text gets is the data limit less these two.
#ifndef FLEET_REPLY_HEADER_LEN
  #define FLEET_REPLY_HEADER_LEN 5
#endif
#ifndef FLEET_REPLY_NAME_PREFIX_LEN
  #define FLEET_REPLY_NAME_PREFIX_LEN 2
#endif

// "ran 65535; err: " — the longest prefix a multi-command summary carries.
#ifndef FLEET_REPLY_PREFIX_MAX
  #define FLEET_REPLY_PREFIX_MAX 16
#endif

// The reply the fleet keeps for one acknowledgement: the command's actual
// reply (a CLI reply buffer's worth) or "ran N; err: " + the first failing
// reply. It is kept whole and cut to the room the node name leaves when the
// acknowledgement is sent, so a short name buys a longer reply (see FLEET.md).
#ifndef FLEET_REPLY_SUMMARY_LEN
  #define FLEET_REPLY_SUMMARY_LEN (CLI_REPLY_BUFFER_MAX + FLEET_REPLY_PREFIX_MAX)
#endif

// The reply must always fit one datagram. The summary is cut to what the node
// name leaves of the packet, so the composed text never exceeds the data
// limit: 31 + 2 + 32 + 1 leaves 97 characters for the summary in the worst
// case, and more whenever the name or the job key is shorter.
static_assert(FLEET_REPLY_DATA_MAX - FLEET_REPLY_HEADER_LEN - FLEET_MAX_NODE_NAME
                - FLEET_REPLY_NAME_PREFIX_LEN - FLEET_CLI_KEY_LEN - 1 > 0,
              "the fleet reply must fit one group datagram");

// The summary must hold a whole CLI reply plus the "ran N; err: " prefix: a
// shorter buffer would drop the reply's tail before it is sent, and the tail
// is where the marker that says the reply was cut lives.
static_assert(FLEET_REPLY_SUMMARY_LEN >= CLI_REPLY_MAX + FLEET_REPLY_PREFIX_MAX,
              "the fleet summary must hold a whole reply");

#endif // _FLEET_CONFIG_H