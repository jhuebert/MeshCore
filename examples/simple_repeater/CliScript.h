// CliScript.h — remote CLI scripts for the simple_repeater packet filter:
// a rule with `action=cli` forwards a decrypted group text like `forward` AND
// queues its message text as a CLI script, executed deferred (outside the
// receive path) and at most once per job key per boot. Fork-owned component,
// owned by FilterRules like AdvertRateLimiter; see FILTER.md for the user
// manual and PacketFilter.h for the rule model.
//
// The script is ordinary group text: line 1 must be "!id <key>" (the directive
// doubles as the "this is a script" marker, so no message can run
// un-deduplicated by accident), the remaining lines are CLI commands, blank
// lines and '#' comments are skipped. The key string is hashed (FNV-1a 64) and
// only the hash is kept in the seen ring — the string itself is never stored,
// and hashes are never shown to a user.
//
// Trust model: possession of the channel PSK is full admin access (the keyed
// channel MAC must verify before anything reaches the filter). The seen table
// is RAM-only like the advert cache: it starts empty on every boot, so scripts
// must be safe to re-run — commands that set state are repeatable by
// convention; index-based mutations and one-shot effects (reset) ship as their
// own jobs sent deliberately.
//
// No persistence code: the runner is pure RAM state. Everything resets on
// reboot; nothing touches flash.

#ifndef _CLI_SCRIPT_H
#define _CLI_SCRIPT_H

#include <Arduino.h>
#include <Mesh.h>

#include <string.h>

#include "PacketFilterConfig.h"

// Execute one script line, in the style of the serial console. `key` is the
// job's !id key string (for logging only — never the stored hash). `reply` is
// a CLI_REPLY_MAX buffer the callee fills; `line` is mutable scratch the
// callee may tokenise in place, as the CLI handlers do.
typedef void (*CliExecFn)(void* ctx, const char* key, char* line, char* reply);

enum CliEnqueue {
  CLI_ENQUEUE_OK = 0,
  CLI_ENQUEUE_NO_ID,      // first line is not "!id ...": not a script (still forwarded)
  CLI_ENQUEUE_DUP,        // key already seen this boot: no-op
  CLI_ENQUEUE_FULL,       // pending queue full: refused, key NOT marked
  CLI_ENQUEUE_BAD_KEY,    // !id key is not 1..FILTER_CLI_KEY_LEN chars of [A-Za-z0-9._-]
  CLI_ENQUEUE_BAD_DIRECTIVE,  // unknown '!' directive: refuse the whole script (fail-safe)
};

// One queued script: the full message text plus the key string, which is only
// kept so execution logs can name the job (the seen ring stores the hash).
struct CliPendingScript {
  char text[MAX_PACKET_PAYLOAD + 1];   // NUL-terminated message text
  char key[FILTER_CLI_KEY_LEN + 1];    // NUL-terminated !id key
};

class CliScriptRunner {
  CliPendingScript pending[FILTER_CLI_QUEUE_DEPTH];
  int pending_count;
  uint64_t seen[FILTER_CLI_SEEN_SIZE];   // job-key hashes, ring of the last N this boot
  int seen_count;
  int seen_head;                          // ring head (oldest entry) once full
  // counters (RAM-only telemetry, reset by resetStats()-style boots only)
  uint32_t ran;       // scripts executed
  uint32_t dup;       // enqueues refused: key already seen
  uint32_t noid;      // enqueues refused: no !id marker
  uint32_t refused;   // enqueues refused: queue full, bad key or bad directive
  uint32_t badline;   // lines skipped at run time: too long for the CLI

public:
  CliScriptRunner() { reset(); }

  // back to a freshly constructed runner: empty queue, empty seen ring, zero counters
  void reset() {
    memset(pending, 0, sizeof(pending));
    pending_count = 0;
    memset(seen, 0, sizeof(seen));
    seen_count = 0;
    seen_head = 0;
    ran = 0;
    dup = 0;
    noid = 0;
    refused = 0;
    badline = 0;
  }

  // Parse `text` (the message text the filter matched, sender prefix already
  // stripped by parseGroupText) and queue it. Marks the key hash at enqueue,
  // so duplicate flood deliveries and re-sends alike no-op; a refused script
  // is never marked, so a re-send still reaches it. Never executes anything.
  CliEnqueue enqueue(const char* text);

  // Drain ONE pending script (FIFO): run each command line through fn, then
  // drop the slot. Callers invoke this from the main loop, after the receive/
  // forward path has finished — never while the filter is evaluating a packet.
  // Returns false when nothing was pending.
  bool run(CliExecFn fn, void* ctx);

  // seen-ring queries for the `filter cli` namespace
  int getSeenCount() const { return seen_count; }
  int getPendingCount() const { return pending_count; }
  bool keySeen(const char* key) const;
  // the deliberate re-run lever: forget one key (false if it was not seen)
  bool forgetKey(const char* key);
  void forgetAll();

  // counters
  uint32_t getRan() const { return ran; }
  uint32_t getDup() const { return dup; }
  uint32_t getNoId() const { return noid; }
  uint32_t getRefused() const { return refused; }
  uint32_t getBadLines() const { return badline; }

private:
  int findKey(uint64_t hash) const;
  void markSeen(uint64_t hash);
};

#endif // _CLI_SCRIPT_H