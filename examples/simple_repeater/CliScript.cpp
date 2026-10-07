// CliScript.cpp — see CliScript.h for the design overview.

#include "CliScript.h"
#include "CliUtil.h"   // CLI_REPLY_MAX: execution replies use the same bound

// FNV-1a over the job key. 64-bit: the whole 32-entry ring costs 256 B and a
// collision between two chosen keys is ~n/2^64 — its worst case is one skipped
// job, visible in the dup counter. The hash is storage-internal; the key
// string the operator chose is the only name a job has on screen or in logs.
static uint64_t fnv1a64(const char* s) {
  uint64_t h = 14695981039346656037ULL;
  for (; *s; s++) {
    h ^= (uint8_t)*s;
    h *= 1099511628211ULL;
  }
  return h;
}

// Walk the '\n'-separated lines of a writable, NUL-terminated buffer in place:
// each next() NUL-terminates the current line and returns it, or NULL at the
// end. A trailing '\r' is stripped (companions may send CRLF).
class LineWalker {
  char* rest;
public:
  explicit LineWalker(char* buf) : rest(buf) {}
  char* next() {
    if (rest == NULL) return NULL;
    char* line = rest;
    char* nl = strchr(line, '\n');
    if (nl) { *nl = 0; rest = nl + 1; } else { rest = NULL; }
    size_t n = strlen(line);
    if (n > 0 && line[n - 1] == '\r') line[n - 1] = 0;
    return line;
  }
  // where the next line begins (or NULL): after next() returned a line, rest
  // already points past its terminator. Reading this — unlike calling next()
  // again — leaves the next line's terminator intact, which matters when the
  // walk resumes here on a later run() pass.
  char* restPos() const { return rest; }
};

// a job key is 1..FLEET_CLI_KEY_LEN chars of [A-Za-z0-9._-] (shared validator:
// fleet tags use the same charset via CliUtil.h)
static bool validKey(const char* key) {
  return cliValidToken(key, FLEET_CLI_KEY_LEN);
}

// Bounded copy of a line's reply into a summary slot: the first error and the
// last reply are both kept to FLEET_REPLY_SUMMARY_LEN so a finished script's
// acknowledgement always fits one packet.
static void copySummary(char* dst, const char* src) {
  size_t n = strlen(src);
  if (n >= FLEET_REPLY_SUMMARY_LEN) n = FLEET_REPLY_SUMMARY_LEN - 1;
  memcpy(dst, src, n);
  dst[n] = 0;
}

// ---------------------------------------------------------------- directives

// "!delay <ms>": 1..FLEET_DELAY_MAX_MS milliseconds. The pause bounds how long
// one script can hold a run-queue slot. Returns false on any malformed form —
// at parse time that rejects the whole script, fail-safe.
static bool parseDelayValue(const char* rest, uint32_t* out_ms) {
  long v;
  if (!parseIntRange(rest, 1, FLEET_DELAY_MAX_MS, &v)) return false;
  *out_ms = (uint32_t)v;
  return true;
}

// "!tags t1,t2,...": 1..FLEET_MAX_SCRIPT_TAGS tags, each 1..FLEET_TAG_LEN chars
// of the job-key charset. The comma list is exact — whitespace is not honored
// ("xiao, siteB" rejects, the space would be part of the second tag) — and
// empty items reject. Exact equality is the whole matching model (see
// FLEET.md): no contains, no prefix, no wildcards.
static bool parseTagsList(const char* rest, CliScriptMeta* meta) {
  if (rest[0] == 0) return false;
  char vals[MAX_PACKET_PAYLOAD];
  if (!cliCopyCommand(vals, sizeof(vals), rest)) return false;
  char* p = vals;
  char* t;
  while ((t = strsep(&p, ",")) != NULL) {
    if (!cliValidToken(t, FLEET_TAG_LEN)) return false;
    if (meta->tag_count >= FLEET_MAX_SCRIPT_TAGS) return false;
    strcpy(meta->tags[meta->tag_count], t);   // validated by cliValidToken()
    meta->tag_count++;
  }
  return meta->tag_count > 0;
}

// "!at <unix-ts>": exactly 10 digits — a unix epoch second in UTC, like every
// unix timestamp. A 10-digit value above UINT32_MAX (RTCClock's range) is
// malformed here, not merely in the future. The value is accumulated by hand:
// strtoul would clamp on the platforms whose unsigned long is 32-bit and turn
// an out-of-range epoch into 0xFFFFFFFF instead of refusing it. The runner
// stays clock-free: stale and clock-unset refusals are FleetManager's, which
// owns the RTC.
static bool parseAtValue(const char* rest, uint32_t* out_epoch) {
  if (strlen(rest) != 10) return false;
  uint64_t v = 0;
  for (const char* c = rest; *c; c++) {
    if (*c < '0' || *c > '9') return false;
    v = v * 10 + (uint64_t)(*c - '0');
  }
  if (v > 0xFFFFFFFFULL) return false;
  *out_epoch = (uint32_t)v;
  return true;
}

// one "line[0] == '!' and verb matches" check: `verb` must be followed by a
// space or the end of line, so "!tagsx" is never a !tags directive
static bool directiveIs(const char* line, const char* verb) {
  size_t n = strlen(verb);
  if (strncmp(line, verb, n) != 0) return false;
  return line[n] == ' ' || line[n] == 0;
}

// the text after a "verb" prefix: at the end of line when there was no space,
// never past the terminator
static const char* directiveRest(const char* line, const char* verb) {
  size_t n = strlen(verb);
  return line + n + (line[n] == ' ' ? 1 : 0);
}

CliEnqueue CliScriptRunner::parse(const char* text, CliScriptMeta* meta) {
  meta->key[0] = 0;
  meta->tag_count = 0;
  meta->ack = CLI_ACK_NONE;
  meta->due_epoch = 0;
  meta->body_off = 0;

  // parse a private copy so lines can be walked in place; refuse anything
  // oversized outright — a script that cannot be the payload of one message is
  // not a script here
  size_t tlen = strlen(text);
  if (tlen > MAX_PACKET_PAYLOAD) return CLI_ENQUEUE_NO_ID;

  char buf[MAX_PACKET_PAYLOAD + 1];
  memcpy(buf, text, tlen + 1);
  LineWalker lines(buf);

  // Line 1 must be "!id <key>": the directive doubles as the script marker.
  char* id = lines.next();
  if (id == NULL || strncmp(id, "!id ", 4) != 0) return CLI_ENQUEUE_NO_ID;
  char* key = id + 4;
  if (!validKey(key)) return CLI_ENQUEUE_BAD_KEY;
  strcpy(meta->key, key);

  // The directive block: the run of !-lines after !id, before the first
  // command. !tags, !ack and !at are the known block directives, each at most
  // once, in either order; anything else rejects the whole script. Blank lines
  // and '#' comments are skipped everywhere (the same comment convention
  // FILTER.md uses).
  bool in_block = true;
  bool seen_tags = false, seen_ack = false, seen_at = false;
  char* line;
  while ((line = lines.next()) != NULL) {
    if (line[0] == 0 || line[0] == '#') continue;
    if (line[0] == '!') {
      if (!in_block) {
        // body directives: only !delay, anywhere, any number of times
        uint32_t ms;
        if (!directiveIs(line, "!delay") || !parseDelayValue(directiveRest(line, "!delay"), &ms)) {
          return CLI_ENQUEUE_BAD_DIRECTIVE;
        }
        continue;
      }
      if (directiveIs(line, "!tags")) {
        if (seen_tags || !parseTagsList(directiveRest(line, "!tags"), meta)) {
          return CLI_ENQUEUE_BAD_DIRECTIVE;
        }
        seen_tags = true;
      } else if (directiveIs(line, "!ack")) {
        if (seen_ack) return CLI_ENQUEUE_BAD_DIRECTIVE;
        const char* rest = directiveRest(line, "!ack");
        if (strcmp(rest, "") == 0) meta->ack = CLI_ACK_ALWAYS;
        else if (strcmp(rest, "err") == 0) meta->ack = CLI_ACK_ERR;
        else return CLI_ENQUEUE_BAD_DIRECTIVE;
        seen_ack = true;
      } else if (directiveIs(line, "!at")) {
        if (seen_at || !parseAtValue(directiveRest(line, "!at"), &meta->due_epoch)) {
          return CLI_ENQUEUE_BAD_DIRECTIVE;
        }
        seen_at = true;
      } else {
        return CLI_ENQUEUE_BAD_DIRECTIVE;
      }
      continue;
    }
    // first command line: the metadata block ends, the work begins. The walker
    // only terminates lines, never shifts bytes, so copy offsets are text
    // offsets and run() can resume from this point.
    if (in_block) {
      in_block = false;
      meta->body_off = (uint16_t)(line - buf);
    }
  }
  if (in_block) meta->body_off = (uint16_t)tlen;   // directives only, no commands
  return CLI_ENQUEUE_OK;
}

CliEnqueue CliScriptRunner::enqueue(const char* text, const uint8_t* chan_secret,
                                    uint8_t chan_hash) {
  CliScriptMeta meta;
  CliEnqueue rc = parse(text, &meta);
  if (rc == CLI_ENQUEUE_NO_ID) { noid++; return rc; }
  if (rc != CLI_ENQUEUE_OK) { refused++; return rc; }

  const uint64_t hash = fnv1a64(meta.key);
  if (findKey(hash) >= 0) { dup++; return CLI_ENQUEUE_DUP; }
  if (pending_count >= FLEET_CLI_QUEUE_DEPTH) { refused++; return CLI_ENQUEUE_FULL; }

  // commit only once everything validated: queue + mark-at-enqueue. The text
  // buffer is zero-filled first so reads past the string (a resume offset one
  // past the final line) always find a terminator, never stale slot bytes.
  CliPendingScript* slot = &pending[pending_count++];
  memset(slot, 0, sizeof(*slot));
  memcpy(slot->text, text, strlen(text) + 1);
  strcpy(slot->key, meta.key);   // validated by validKey()
  slot->ack = meta.ack;
  slot->body_off = meta.body_off;
  if (chan_secret != NULL) {
    memcpy(slot->chan_secret, chan_secret, sizeof(slot->chan_secret));
    slot->chan_hash = chan_hash;
  }
  markSeen(hash);
  return CLI_ENQUEUE_OK;
}

bool CliScriptRunner::enqueueValidated(const char* text, const char* key, uint8_t ack,
                                       uint16_t body_off, const uint8_t* chan_secret,
                                       uint8_t chan_hash) {
  if (pending_count >= FLEET_CLI_QUEUE_DEPTH) return false;
  if (strlen(key) > FLEET_CLI_KEY_LEN) return false;   // defensive; callers pass parsed keys
  CliPendingScript* slot = &pending[pending_count++];
  memset(slot, 0, sizeof(*slot));
  memcpy(slot->text, text, strlen(text) + 1);
  strcpy(slot->key, key);
  slot->ack = ack;
  slot->body_off = body_off;
  if (chan_secret != NULL) {
    memcpy(slot->chan_secret, chan_secret, sizeof(slot->chan_secret));
    slot->chan_hash = chan_hash;
  }
  return true;
}

bool CliScriptRunner::markKeySeen(const char* key) {
  const uint64_t hash = fnv1a64(key);
  if (findKey(hash) >= 0) return false;
  markSeen(hash);
  return true;
}

bool CliScriptRunner::run(CliExecFn fn, void* ctx, CliRunResult* out) {
  if (fn == NULL || pending_count == 0) return false;

  // Take slot 0 and walk it in place: the callback may tokenise the line (the
  // CLI handlers do) and the slot is discarded afterwards anyway.
  CliPendingScript* slot = &pending[0];
  if (slot->sleeping) {
    // signed difference: the deadline is at most FLEET_DELAY_MAX_MS away, so
    // the subtraction only ever wraps when it is *supposed* to (millis() rollover)
    if ((int32_t)(millis() - slot->sleep_until) < 0) return false;
    slot->sleeping = false;
  }

  LineWalker lines(slot->text + slot->body_off);
  char reply[CLI_REPLY_MAX];
  char* line;
  bool slept = false;
  while ((line = lines.next()) != NULL) {
    if (line[0] == 0 || line[0] == '#') continue;   // blank lines and comments
    if (line[0] == '!') {
      // !delay: validated at enqueue, so it parses here; pause this script and
      // resume from the line after the directive on a later call
      uint32_t ms;
      if (!directiveIs(line, "!delay") || !parseDelayValue(directiveRest(line, "!delay"), &ms)) {
        continue;   // cannot happen after parse(); skip rather than wedge the slot
      }
      char* nxt = lines.restPos();   // the line after the directive, unterminated
      if (nxt != NULL) {
        slot->body_off = (uint16_t)(nxt - slot->text);
      } else {
        // the delay is the script's last line: resume one past its terminator
        // (clamped to the buffer's last byte, which the zero-fill below keeps
        // NUL) so the next pass finds an empty line and the script finishes
        size_t end = (size_t)(line - slot->text) + strlen(line) + 1;
        slot->body_off = (uint16_t)(end < MAX_PACKET_PAYLOAD ? end : MAX_PACKET_PAYLOAD);
      }
      slot->sleep_until = millis() + ms;
      slot->sleeping = true;
      slept = true;
      break;
    }
    // a line the serial CLI could not take whole is skipped, the run continues
    if (strlen(line) >= FLEET_CLI_LINE_MAX) { badline++; continue; }
    reply[0] = 0;
    fn(ctx, slot->key, line, reply);
    slot->ran_count++;
    copySummary(slot->last_reply, reply);
    if (strncmp(reply, "Err", 3) == 0 && slot->first_err[0] == 0) {
      copySummary(slot->first_err, reply);
    }
  }

  if (slept) return true;   // the script is not done; its slot keeps the state

  // the script finished: free the slot and report, so the caller can schedule
  // the acknowledgement without re-running anything
  char summary[FLEET_REPLY_SUMMARY_LEN];
  bool error = slot->first_err[0] != 0;
  if (slot->ran_count == 1) {
    copySummary(summary, slot->last_reply);   // the command's actual reply
  } else if (error) {
    snprintf(summary, sizeof(summary), "ran %u; err: %s",
             (unsigned)slot->ran_count, slot->first_err);
  } else {
    snprintf(summary, sizeof(summary), "ran %u ok", (unsigned)slot->ran_count);
  }

  if (out != NULL) {
    strcpy(out->key, slot->key);
    copySummary(out->summary, summary);
    out->ack = slot->ack;
    out->error = error;
    memcpy(out->chan_secret, slot->chan_secret, sizeof(out->chan_secret));
    out->chan_hash = slot->chan_hash;
  }

  pending_count--;
  memmove(pending, &pending[1], pending_count * sizeof(pending[0]));
  ran++;
  return true;
}

int CliScriptRunner::findKey(uint64_t hash) const {
  // ring start and total do not change while scanning: entries are appended
  // until the ring is full, then the oldest is overwritten (the
  // AdvertRateLimiter cache pattern)
  const bool wrapped = seen_count >= FLEET_CLI_SEEN_SIZE;
  const int total = wrapped ? FLEET_CLI_SEEN_SIZE : seen_count;
  const int start = wrapped ? seen_head : 0;
  for (int i = 0; i < total; i++) {
    const int idx = (start + i) % FLEET_CLI_SEEN_SIZE;
    if (seen[idx] == hash) return idx;
  }
  return -1;
}

void CliScriptRunner::markSeen(uint64_t hash) {
  if (seen_count < FLEET_CLI_SEEN_SIZE) {
    seen[seen_count++] = hash;
  } else {
    seen[seen_head] = hash;   // evict the oldest: a wrap-around re-arms only a
    seen_head = (seen_head + 1) % FLEET_CLI_SEEN_SIZE;   // very old job, and a
  }                         // full table that refused new jobs would stall
                            // script execution for the rest of an uptime
}

bool CliScriptRunner::keySeen(const char* key) const {
  return findKey(fnv1a64(key)) >= 0;
}

bool CliScriptRunner::forgetKey(const char* key) {
  const int idx = findKey(fnv1a64(key));
  if (idx < 0) return false;
  // compact the survivors back to a plain unwrapped ring in logical
  // (oldest-first) order: N is 32 and a forget is a manual command, so the
  // shift costs nothing and keeps eviction order and head bookkeeping simple
  uint64_t kept[FLEET_CLI_SEEN_SIZE];
  int n = 0;
  for (int k = 0; k < seen_count; k++) {
    const int phys = (seen_head + k) % FLEET_CLI_SEEN_SIZE;
    if (phys != idx) kept[n++] = seen[phys];
  }
  memcpy(seen, kept, n * sizeof(seen[0]));
  seen_head = 0;
  seen_count = n;
  return true;
}

void CliScriptRunner::forgetAll() {
  memset(seen, 0, sizeof(seen));
  seen_count = 0;
  seen_head = 0;
}