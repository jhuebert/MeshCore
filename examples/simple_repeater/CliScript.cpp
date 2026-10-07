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
};

// a job key is 1..FILTER_CLI_KEY_LEN chars of [A-Za-z0-9._-]
static bool validKey(const char* key) {
  size_t n = strlen(key);
  if (n == 0 || n > FILTER_CLI_KEY_LEN) return false;
  for (const char* c = key; *c; c++) {
    if (!((*c >= 'A' && *c <= 'Z') || (*c >= 'a' && *c <= 'z') ||
          (*c >= '0' && *c <= '9') || *c == '.' || *c == '_' || *c == '-')) return false;
  }
  return true;
}

CliEnqueue CliScriptRunner::enqueue(const char* text) {
  // parse a private copy so lines can be walked in place; `text` comes from
  // checkContent's bounded buffer, but refuse anything oversized outright — a
  // script that cannot be the payload of one message is not a script here
  size_t tlen = strlen(text);
  if (tlen > MAX_PACKET_PAYLOAD) { noid++; return CLI_ENQUEUE_NO_ID; }

  char buf[MAX_PACKET_PAYLOAD + 1];
  memcpy(buf, text, tlen + 1);
  LineWalker lines(buf);

  // Line 1 must be "!id <key>": the directive doubles as the script marker.
  char* id = lines.next();
  if (id == NULL || strncmp(id, "!id ", 4) != 0) { noid++; return CLI_ENQUEUE_NO_ID; }
  char* key = id + 4;
  if (!validKey(key)) { refused++; return CLI_ENQUEUE_BAD_KEY; }

  // Remaining lines are CLI commands, blank lines or '#' comments (the same
  // comment convention FILTER.md uses). Any other '!' directive is reserved
  // for the future and unknown here: refuse the whole script, fail-safe.
  char* line;
  while ((line = lines.next()) != NULL) {
    if (line[0] == 0 || line[0] == '#') continue;
    if (line[0] == '!') { refused++; return CLI_ENQUEUE_BAD_DIRECTIVE; }
  }

  const uint64_t hash = fnv1a64(key);
  if (findKey(hash) >= 0) { dup++; return CLI_ENQUEUE_DUP; }
  if (pending_count >= FILTER_CLI_QUEUE_DEPTH) { refused++; return CLI_ENQUEUE_FULL; }

  // commit only once everything validated: queue + mark-at-enqueue
  CliPendingScript* slot = &pending[pending_count++];
  memcpy(slot->text, text, tlen + 1);
  strcpy(slot->key, key);   // validated by validKey()
  markSeen(hash);
  return CLI_ENQUEUE_OK;
}

bool CliScriptRunner::run(CliExecFn fn, void* ctx) {
  if (fn == NULL || pending_count == 0) return false;

  // Take slot 0 and walk it in place: the callback may tokenise the line (the
  // CLI handlers do) and the slot is discarded afterwards anyway.
  CliPendingScript* slot = &pending[0];
  LineWalker lines(slot->text);
  lines.next();   // line 1 is the "!id" directive; enqueue validated it
  char reply[CLI_REPLY_MAX];
  char* line;
  while ((line = lines.next()) != NULL) {
    if (line[0] == 0 || line[0] == '#') continue;   // blank lines and comments
    // a line the serial CLI could not take whole is skipped, the run continues
    if (strlen(line) >= FILTER_CLI_LINE_MAX) { badline++; continue; }
    reply[0] = 0;
    fn(ctx, slot->key, line, reply);
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
  const bool wrapped = seen_count >= FILTER_CLI_SEEN_SIZE;
  const int total = wrapped ? FILTER_CLI_SEEN_SIZE : seen_count;
  const int start = wrapped ? seen_head : 0;
  for (int i = 0; i < total; i++) {
    const int idx = (start + i) % FILTER_CLI_SEEN_SIZE;
    if (seen[idx] == hash) return idx;
  }
  return -1;
}

void CliScriptRunner::markSeen(uint64_t hash) {
  if (seen_count < FILTER_CLI_SEEN_SIZE) {
    seen[seen_count++] = hash;
  } else {
    seen[seen_head] = hash;   // evict the oldest: a wrap-around re-arms only a
    seen_head = (seen_head + 1) % FILTER_CLI_SEEN_SIZE;   // very old job, and a
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
  uint64_t kept[FILTER_CLI_SEEN_SIZE];
  int n = 0;
  for (int k = 0; k < seen_count; k++) {
    const int phys = (seen_head + k) % FILTER_CLI_SEEN_SIZE;
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