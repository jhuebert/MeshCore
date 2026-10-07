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
  #define FLEET_REPLY_WINDOW_MS 60000UL   // !ack reply jitter window (default)
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

// Summary budget for one acknowledgement: the repeater name (max 31, CommonCLI
// prefs) + ": " + the job key (FLEET_CLI_KEY_LEN) + " " must share the
// createGroupDatagram data limit (184 B payload - 1 B hash - 15 B cipher pad =
// 168 B) with it, so 31 + 2 + 32 + 1 + summary <= 168 gives this 100-char cap;
// a longer summary is truncated at build time, never split across packets.
#define FLEET_REPLY_SUMMARY_LEN 100

#endif // _FLEET_CONFIG_H