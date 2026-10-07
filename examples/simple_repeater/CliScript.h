// CliScript.h — remote CLI scripts for the fleet manager: a decrypted group
// text on the fleet channel whose first line is a `!id` directive is queued as
// a CLI script, executed deferred (outside the receive path) and at most once
// per job key per boot. Fork-owned component; the runner is owned by
// FleetManager like AdvertRateLimiter is owned by FilterRules; see FLEET.md
// for the user manual and FleetManager.h for the channel/tag model.
//
// The script is ordinary group text:
//
//   !id <key>              line 1, always (doubles as the "this is a script"
//                          marker, so no message can run un-deduplicated by
//                          accident)
//   [!tags t1,t2,...]  \   the directive block — !tags, !ack and !at, each at
//   [!ack [err]]        >  most once, either order; a block line that is none
//   [!at <unix-ts>]    /   of them rejects the whole script (fail-safe)
//   <CLI command lines>    may include "!delay <ms>" anywhere, any number of
//                          times; any other '!' line rejects the script
//
// blank lines and '#' comments are skipped. `!id` targeting lives in
// FleetManager (the tag match is fleet config, not script machinery): the
// runner only parses and validates the block and hands the result back.
//
// The key string is hashed (FNV-1a 64) and only the hash is kept in the seen
// ring — the string itself is never stored in the ring, and hashes are never
// shown to a user.
//
// Trust model: possession of the channel PSK is full admin access (the keyed
// channel MAC must verify before anything reaches the manager). The seen table
// is RAM-only like the advert cache: it starts empty on every boot, so scripts
// must be safe to re-run — commands that set state are repeatable by
// convention; index-based mutations and one-shot effects (reboot, start ota)
// ship as their own jobs sent deliberately.
//
// No persistence code: the runner is pure RAM state. Everything resets on
// reboot; nothing touches flash.

#ifndef _CLI_SCRIPT_H
#define _CLI_SCRIPT_H

#include <Arduino.h>
#include <Mesh.h>

#include <string.h>

#include "FleetConfig.h"

// Execute one script line, in the style of the serial console. `key` is the
// job's !id key string (for logging only — never the stored hash). `reply` is
// a CLI_REPLY_MAX buffer the callee fills; `line` is mutable scratch the
// callee may tokenise in place, as the CLI handlers do.
typedef void (*CliExecFn)(void* ctx, const char* key, char* line, char* reply);

// what a script's !ack directive asked for
enum CliAckMode {
  CLI_ACK_NONE = 0,    // no directive: run silently
  CLI_ACK_ALWAYS = 1,  // !ack: reply with the result when the script finishes
  CLI_ACK_ERR = 2,     // !ack err: reply only when a command line failed
};

enum CliEnqueue {
  CLI_ENQUEUE_OK = 0,
  CLI_ENQUEUE_NO_ID,      // first line is not "!id ...": not a script (still forwarded)
  CLI_ENQUEUE_DUP,        // key already seen this boot: no-op
  CLI_ENQUEUE_FULL,       // pending queue full: refused, key NOT marked
  CLI_ENQUEUE_BAD_KEY,    // !id key is not 1..FLEET_CLI_KEY_LEN chars of [A-Za-z0-9._-]
  CLI_ENQUEUE_BAD_DIRECTIVE,  // unknown/malformed '!' directive: refuse the whole
                              // script (fail-safe)
};

// The directive block, returned by parse() to callers that need it (the tag
// match and reply decisions live in FleetManager, which owns the repeater's
// tag set and the reply store — the runner never stores tag strings beyond
// validating them). Only meaningful after CLI_ENQUEUE_OK.
struct CliScriptMeta {
  char key[FLEET_CLI_KEY_LEN + 1];   // the !id key, validated
  char tags[FLEET_MAX_SCRIPT_TAGS][FLEET_TAG_LEN + 1];
  uint8_t tag_count;                 // 0 = no !tags directive = broadcast
  uint8_t ack;                       // CliAckMode
  uint32_t due_epoch;                // !at target (unix seconds UTC); 0 = run now
  uint16_t body_off;                 // offset of the first command line in the
                                     // message text (strlen(text) when none)
};

// What a finished script reports, so the caller can schedule an
// acknowledgement without re-running anything. Filled by run() only when a
// script completed on that call.
struct CliRunResult {
  char key[FLEET_CLI_KEY_LEN + 1];
  char summary[FLEET_REPLY_SUMMARY_LEN];  // single command: its actual reply;
                                          // several: "ran N ok" or
                                          // "ran N; err: <first failing reply>"
  uint8_t ack;                            // CliAckMode of the finished script
  bool error;                             // at least one command line replied "Err ..."
};

// One queued script: the full message text plus the key string, which is only
// kept so execution logs can name the job (the seen ring stores the hash).
// The run-state fields make the runner resumable across `!delay` pauses
// (FleetManager calls run() once per loop pass).
struct CliPendingScript {
  char text[MAX_PACKET_PAYLOAD + 1];   // NUL-terminated message text
  char key[FLEET_CLI_KEY_LEN + 1];     // NUL-terminated !id key
  uint8_t ack;                         // CliAckMode from the directive block
  uint16_t body_off;                   // offset of the next line to execute
  bool sleeping;                       // paused on a !delay
  uint32_t sleep_until;                // millis() deadline; wrap-safe via the
                                       // signed-difference comparison in run()
  // reply-summary accumulation, updated as lines execute
  uint16_t ran_count;                  // command lines executed so far
  char last_reply[FLEET_REPLY_SUMMARY_LEN];   // reply of the most recent line
  char first_err[FLEET_REPLY_SUMMARY_LEN];    // first "Err ..." reply, or empty
};

class CliScriptRunner {
  CliPendingScript pending[FLEET_CLI_QUEUE_DEPTH];
  int pending_count;
  uint64_t seen[FLEET_CLI_SEEN_SIZE];   // job-key hashes, ring of the last N this boot
  int seen_count;
  int seen_head;                          // ring head (oldest entry) once full
  // counters (RAM-only telemetry; reset by reset(), i.e. every boot)
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

  // Pure validation of a message as a fleet script: line 1 must be "!id <key>",
  // the directive block must be well-formed, and every '!' line after the first
  // command must be a valid "!delay". No state is touched and no counter moves,
  // so callers may parse freely to make targeting decisions (FleetManager tags
  // every fleet-channel message). On CLI_ENQUEUE_OK, *meta carries the
  // directive block and the command-body offset.
  static CliEnqueue parse(const char* text, CliScriptMeta* meta);

  // Parse `text` (the message text the manager matched, sender prefix already
  // stripped by parseGroupText) and queue it. Marks the key hash at enqueue,
  // so duplicate flood deliveries and re-sends alike no-op; a refused script
  // is never marked, so a re-send still reaches it. Never executes anything.
  // (Re-parses internally: one authoritative parser, and the parse cost of a
  // 184-byte message is noise next to the radio.)
  CliEnqueue enqueue(const char* text);

  // Queue an already-parsed script (a scheduled script firing) without
  // touching the seen ring — its key was marked when the job was first
  // received. `body_off` comes from the same parse the caller admitted the
  // job with, so execution starts at the command body, not the directives.
  // Returns false when the queue is full; the caller keeps the script and
  // retries.
  bool enqueueValidated(const char* text, const char* key, uint8_t ack, uint16_t body_off);

  // Mark a key seen without queueing (the manager admits scheduled scripts to
  // its own store). Returns false — without marking — when the key was already
  // seen this boot, so the caller can count the duplicate.
  bool markKeySeen(const char* key);

  // Work on the head script (FIFO): execute lines through fn until the script
  // finishes, or until a `!delay` pauses it (resumed by later calls once the
  // deadline passes — a sleeping script holds its slot). With no delays the
  // behaviour is unchanged: one whole script per call. Returns true when work
  // happened (lines executed, a sleep armed or a script finished); false when
  // nothing could progress. When a script finishes, *out (if given) carries
  // its acknowledgement data.
  bool run(CliExecFn fn, void* ctx, CliRunResult* out = NULL);

  // seen-ring queries for the `fleet` namespace
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