// FleetManager.cpp — see FleetManager.h for the design overview.

#include "FleetManager.h"
#include "CliUtil.h"

#define FLEET_CFG_FILE "/fleet_cfg"

// The one definition of the out-of-the-box state, shared by the constructor
// and load(): fleet off, no channel, no tags, default reply window. The
// runner, scheduled store, reply store and telemetry are RAM-only and reset
// with everything else — a reboot forgets armed jobs and pending acks.
void FleetManager::resetToDefaults() {
  enabled = false;
  memset(psk, 0, sizeof(psk));
  psk_len = 0;
  chan_hash = 0;
  memset(tags, 0, sizeof(tags));
  tag_count = 0;
  reply_window_ms = FLEET_REPLY_WINDOW_MS;
  runner.reset();
  memset(sched, 0, sizeof(sched));
  sched_count = 0;
  memset(replies, 0, sizeof(replies));
  reply_count = 0;
  offered = 0;
  matched = 0;
  time_fn = NULL; time_ctx = NULL;
  jitter_fn = NULL; jitter_ctx = NULL;
  send_fn = NULL; send_ctx = NULL;
}

FleetManager::FleetManager() {
  resetToDefaults();
}

void FleetManager::begin(FILESYSTEM* fs) {
  load(fs);
}

void FleetManager::loop(FILESYSTEM* fs) {
  if (save_flag.due()) save(fs);
}

void FleetManager::setEnabled(bool on) {
  if (enabled == on) return;
  enabled = on;
  markDirty();
}

// ---------------------------------------------------------------- channel

bool FleetManager::setChannel(const char* psk_hex) {
  // exactly 16 or 32 bytes of hex; anything else is refused before decoding
  size_t hex_len = strlen(psk_hex);
  if (hex_len != 32 && hex_len != 64) return false;

  // build the whole candidate off to the side: a rejected key must leave the
  // live channel byte-for-byte unchanged
  uint8_t candidate[32];
  memset(candidate, 0, sizeof(candidate));
  int len = cliDecodeHex(psk_hex, hex_len, candidate, sizeof(candidate));
  if (len != 16 && len != 32) return false;

  memcpy(psk, candidate, sizeof(psk));   // zero-padded, like the filter's store
  psk_len = (uint8_t)len;
  mesh::Utils::sha256(&chan_hash, sizeof(chan_hash), psk, psk_len);
  markDirty();
  return true;
}

void FleetManager::clearChannel() {
  if (psk_len == 0) return;   // already unset: no dirty write for a no-op
  memset(psk, 0, sizeof(psk));
  psk_len = 0;
  chan_hash = 0;
  markDirty();
}

// ---------------------------------------------------------------- tags

bool FleetManager::hasTag(const char* tag) const {
  for (int i = 0; i < tag_count; i++) {
    if (strcmp(tags[i], tag) == 0) return true;
  }
  return false;
}

bool FleetManager::addTag(const char* tag) {
  // same charset and length grammar as a job key (CliUtil.h); an unknown tag
  // string must never be storable, or `!tags` targeting could silently miss it
  if (!cliValidToken(tag, FLEET_TAG_LEN)) return false;
  if (hasTag(tag)) return true;          // duplicate: no-op success
  if (tag_count >= FLEET_MAX_TAGS) return false;

  strcpy(tags[tag_count], tag);          // validated by cliValidToken()
  tag_count++;
  markDirty();
  return true;
}

bool FleetManager::delTag(const char* tag) {
  for (int i = 0; i < tag_count; i++) {
    if (strcmp(tags[i], tag) == 0) {
      memmove(&tags[i], &tags[i + 1], (tag_count - i - 1) * sizeof(tags[0]));
      memset(&tags[tag_count - 1], 0, sizeof(tags[0]));
      tag_count--;
      markDirty();
      return true;
    }
  }
  return false;
}

void FleetManager::clearTags() {
  if (tag_count == 0) return;
  memset(tags, 0, sizeof(tags));
  tag_count = 0;
  markDirty();
}

void FleetManager::setReplyWindowMs(uint32_t ms) {
  if (ms == 0 || ms > FLEET_REPLY_WINDOW_MAX_MS) return;
  if (reply_window_ms == ms) return;
  reply_window_ms = ms;
  markDirty();
}

// ---------------------------------------------------------------- scripts

void FleetManager::onGroupData(uint8_t type, const mesh::GroupChannel& channel,
                               const uint8_t* data, size_t len) {
  if (!enabled || psk_len == 0) return;   // hooks idle: config preserved
  if (type != PAYLOAD_TYPE_GRP_TXT) return;
  // keyed-channel identity: compare the delivered (MAC-proven) secret against
  // the stored PSK, both zero-padded — the same mechanism the filter's channel
  // store uses
  if (memcmp(psk, channel.secret, sizeof(psk)) != 0) return;

  // split the message body (sender prefix skipped; the fleet intake never
  // uses it — targeting is tags)
  char sender[MAX_PACKET_PAYLOAD + 1];
  char text[MAX_PACKET_PAYLOAD + 1];
  bool has_text = false, has_sender = false;
  parseGroupText(data, len, sender, sizeof(sender), text, sizeof(text), &has_text, &has_sender);
  if (!has_text) return;

  // ordinary chat on the fleet channel is not a script: parse is pure, so
  // non-scripts move nothing but the counters below
  CliScriptMeta meta;
  if (CliScriptRunner::parse(text, &meta) != CLI_ENQUEUE_OK) return;
  offered++;

  // tag targeting: absent !tags = broadcast; otherwise exact set membership —
  // the script runs iff its tag list names one of this repeater's tags
  if (meta.tag_count > 0) {
    bool hit = false;
    for (int i = 0; i < meta.tag_count && !hit; i++) hit = hasTag(meta.tags[i]);
    if (!hit) return;
  }
  matched++;

  if (meta.due_epoch != 0) {
    admitScheduled(text, meta, channel);
  } else {
    // mark-at-enqueue happens inside; refusals are counted there too. Nothing
    // executes in the receive path — runScripts() drains later.
    runner.enqueue(text, channel.secret, channel.hash[0]);
  }
}

void FleetManager::admitScheduled(const char* text, const CliScriptMeta& meta,
                                  const mesh::GroupChannel& chan) {
  // the runner stays clock-free: staleness and clock-floor refusals are
  // manager decisions, made with the node RTC
  uint32_t now = time_fn ? time_fn(time_ctx) : 0;
  if (now < FLEET_AT_CLOCK_FLOOR) { runner.noteRefused(); return; }   // RTC unset
  if ((int32_t)(now - meta.due_epoch) > FLEET_AT_STALE_SECS) {
    runner.noteRefused();   // a typo'd stale date must never run immediately
    return;
  }
  if (runner.keySeen(meta.key)) { runner.noteDup(); return; }
  if (sched_count >= FLEET_SCHED_STORE) { runner.noteRefused(); return; }

  // commit only once everything validated: store + mark-at-admission
  FleetSched* s = &sched[sched_count++];
  memset(s, 0, sizeof(*s));
  strcpy(s->text, text);
  strcpy(s->key, meta.key);
  s->ack = meta.ack;
  s->body_off = meta.body_off;
  s->due_epoch = meta.due_epoch;
  memcpy(s->chan_secret, chan.secret, sizeof(s->chan_secret));
  s->chan_hash = chan.hash[0];
  runner.markKeySeen(meta.key);
}

void FleetManager::scheduleAckIfNeeded(const CliRunResult& res) {
  if (res.ack == CLI_ACK_NONE) return;
  if (res.ack == CLI_ACK_ERR && !res.error) return;   // silence means success
  if (reply_count >= FLEET_REPLY_STORE) { runner.noteRefused(); return; }

  FleetReply* r = &replies[reply_count++];
  memset(r, 0, sizeof(*r));
  strcpy(r->key, res.key);
  memcpy(r->summary, res.summary, FLEET_REPLY_SUMMARY_LEN);
  memcpy(r->chan_secret, res.chan_secret, sizeof(r->chan_secret));
  r->chan_hash = res.chan_hash;
  // uniform-random jitter against reply storms, measured from when the script
  // finished; without an injected RNG the reply goes out immediately
  uint32_t jitter = jitter_fn ? jitter_fn(jitter_ctx, reply_window_ms) : 0;
  r->deadline_ms = millis() + jitter;
}

void FleetManager::runScripts(CliExecFn fn, void* exec_ctx) {
  // scheduled scripts that have come due move to the run queue; one that
  // cannot fit waits in the store for the next pass (no drops)
  if (sched_count > 0 && time_fn != NULL) {
    uint32_t now = time_fn(time_ctx);
    for (int i = 0; i < sched_count;) {
      if ((int32_t)(now - sched[i].due_epoch) < 0) { i++; continue; }
      if (!runner.enqueueValidated(sched[i].text, sched[i].key, sched[i].ack,
                                   sched[i].body_off, sched[i].chan_secret,
                                   sched[i].chan_hash)) {
        break;   // run queue full: keep the script armed, retry next pass
      }
      memmove(&sched[i], &sched[i + 1], (sched_count - i - 1) * sizeof(sched[0]));
      memset(&sched[sched_count - 1], 0, sizeof(sched[0]));
      sched_count--;
    }
  }

  // one script per pass, the same cadence the filter's drain had; the ack, if
  // any, is scheduled from the finished script's own result
  CliRunResult res;
  memset(&res, 0, sizeof(res));
  if (runner.run(fn, exec_ctx, &res)) {
    scheduleAckIfNeeded(res);
  }

  sendDueReplies();
}

void FleetManager::sendDueReplies() {
  for (int i = 0; i < reply_count;) {
    // signed difference: the deadline is at most one jitter window away, so
    // the subtraction only wraps when millis() itself does
    if ((int32_t)(millis() - replies[i].deadline_ms) < 0) { i++; continue; }
    if (send_fn != NULL) {
      char body[FLEET_CLI_KEY_LEN + 1 + FLEET_REPLY_SUMMARY_LEN];   // "<key> <summary>"
      snprintf(body, sizeof(body), "%s %s", replies[i].key, replies[i].summary);
      // the captured channel goes back out as-is; if the send cannot happen
      // (packet pool empty), the reply is skipped silently
      send_fn(send_ctx, replies[i].chan_secret, replies[i].chan_hash, body);
    }
    memmove(&replies[i], &replies[i + 1], (reply_count - i - 1) * sizeof(replies[0]));
    memset(&replies[reply_count - 1], 0, sizeof(replies[0]));
    reply_count--;
  }
}

// ---------------------------------------------------------------- relay side

bool FleetManager::checkForward(const mesh::Packet* packet) {
  // true = "let the battery gate decide" (not an allow). False = fleet-channel
  // group traffic: the gate is never consulted, so it never counts the packet
  // as a battery drop and the packet relays even while suspended.
  if (!enabled || psk_len == 0) return true;
  uint8_t type = packet->getPayloadType();
  if (type != PAYLOAD_TYPE_GRP_TXT && type != PAYLOAD_TYPE_GRP_DATA) return true;
  if (packet->payload_len < 1) return true;
  // payload[0] of a group packet IS the on-air channel hash (Mesh.cpp reads it
  // at offset 0) and the packet is still encrypted here: a one-byte compare on
  // the relay path. A hash-byte collision with some other channel would only
  // ever widen relay availability by one packet that still needs a valid MAC
  // to affect anything downstream.
  return packet->payload[0] != chan_hash;
}

// ---------------------------------------------------------------- channels

int FleetManager::appendChannelByHash(const uint8_t* hash, mesh::GroupChannel dest[],
                                      int max_matches) {
  if (!enabled || psk_len == 0) return 0;
  if (chan_hash != hash[0]) return 0;
  // the filter store may hold the same PSK: handing core the same secret twice
  // would burn one of its few candidate slots
  for (int i = 0; i < max_matches; i++) {
    if (memcmp(dest[i].secret, psk, sizeof(psk)) == 0) return 0;
  }
  dest[0].hash[0] = chan_hash;
  memcpy(dest[0].secret, psk, sizeof(dest[0].secret));
  return 1;
}

// ---------------------------------------------------------------- persistence

// Validate a record without touching the live manager: format-level checks
// only (magic, version, CRC, the field ranges this firmware writes). Value
// clamping happens in load() — an out-of-range tag_count is clamped and its
// valid prefix kept, per the fail-soft rule for string arrays.
static bool fleetValidateRecord(const FleetCfgRecord& rec) {
  if (memcmp(rec.magic, FLEET_CFG_MAGIC, 4) != 0) return false;
  if (rec.version != FLEET_CFG_VERSION) return false;
  if (rec.enabled > 1) return false;
  if (rec.psk_len != 0 && rec.psk_len != 16 && rec.psk_len != 32) return false;
  return true;
}

// --- staged save callbacks -------------------------------------------------

static bool fleetWriteFile(File& f, void* ctx) {
  const FleetManager& fleet = *(const FleetManager*)ctx;
  FleetCfgRecord rec;
  memset(&rec, 0, sizeof(rec));
  memcpy(rec.magic, FLEET_CFG_MAGIC, 4);
  rec.version = FLEET_CFG_VERSION;
  rec.enabled = fleet.isEnabled() ? 1 : 0;
  rec.psk_len = fleet.getChanSecretLen();
  memcpy(rec.psk, fleet.getChanSecret(), sizeof(rec.psk));
  rec.tag_count = (uint8_t)fleet.getTagCount();
  for (int i = 0; i < fleet.getTagCount(); i++) {
    strcpy(rec.tags[i], fleet.getTag(i));   // live tags are validated, NUL-terminated
  }
  rec.reply_window_ms = fleet.getReplyWindowMs();

  // record + trailing CRC-16 (LE) over the payload
  uint8_t buf[FLEET_CFG_RECORD_BYTES];
  memcpy(buf, &rec, FLEET_CFG_PAYLOAD_BYTES);
  uint16_t crc = crc16_ccitt(buf, FLEET_CFG_PAYLOAD_BYTES);
  memcpy(&buf[FLEET_CFG_PAYLOAD_BYTES], &crc, 2);
  return f.write(buf, FLEET_CFG_RECORD_BYTES) == FLEET_CFG_RECORD_BYTES;
}

static bool fleetValidateFile(File& f, void* ctx) {
  (void)ctx;
  uint8_t buf[FLEET_CFG_RECORD_BYTES];
  if (f.read(buf, FLEET_CFG_RECORD_BYTES) != FLEET_CFG_RECORD_BYTES) return false;
  uint16_t stored, computed;
  memcpy(&stored, &buf[FLEET_CFG_PAYLOAD_BYTES], 2);
  computed = crc16_ccitt(buf, FLEET_CFG_PAYLOAD_BYTES);
  if (stored != computed) return false;
  return fleetValidateRecord(*(const FleetCfgRecord*)buf);
}

void FleetManager::save(FILESYSTEM* fs) {
  // Staged save: scratch file, read back, keep the previous good file as a
  // backup, then promote — the same transaction every persisted config here uses.
  bool ok = saveStaged(fs, FLEET_CFG_FILE, fleetWriteFile, this,
                       fleetValidateFile, NULL, fleetValidateFile, NULL);
  if (ok) save_flag.clear();
  else save_flag.retryLater();
}

void FleetManager::load(FILESYSTEM* fs) {
  resetToDefaults();   // the file decides everything below; nothing survives from before
  save_flag.reset();   // a reload supersedes any edit still waiting to be written

  char path[64];
  PersistLoadSource src = chooseConfigToLoad(fs, FLEET_CFG_FILE, fleetValidateFile, NULL,
                                             path, sizeof(path));
  if (src == PERSIST_LOAD_NONE) return;   // nothing usable: keep the defaults
  File file = fsOpenRead(fs, path);
  if (!file) return;

  uint8_t buf[FLEET_CFG_RECORD_BYTES];
  if (file.read(buf, FLEET_CFG_RECORD_BYTES) != FLEET_CFG_RECORD_BYTES) {
    file.close();
    return;
  }
  file.close();

  uint16_t stored, computed;
  memcpy(&stored, &buf[FLEET_CFG_PAYLOAD_BYTES], 2);
  computed = crc16_ccitt(buf, FLEET_CFG_PAYLOAD_BYTES);
  if (stored != computed) return;   // chooseConfigToLoad validated it; re-verify anyway

  const FleetCfgRecord& rec = *(const FleetCfgRecord*)buf;
  if (!fleetValidateRecord(rec)) return;

  enabled = (rec.enabled == 1);
  psk_len = rec.psk_len;
  memcpy(psk, rec.psk, sizeof(psk));
  chan_hash = 0;
  if (psk_len > 0) {
    mesh::Utils::sha256(&chan_hash, sizeof(chan_hash), psk, psk_len);
  }

  // out-of-range tag_count clamps to the valid prefix; a slot that does not
  // terminate inside its own storage, or holds an empty string, ends the
  // prefix (fail-soft for string arrays)
  int n = rec.tag_count > FLEET_MAX_TAGS ? FLEET_MAX_TAGS : rec.tag_count;
  for (int i = 0; i < n; i++) {
    if (memchr(rec.tags[i], 0, sizeof(rec.tags[i])) == NULL) break;
    if (rec.tags[i][0] == 0) break;
    memcpy(tags[i], rec.tags[i], sizeof(rec.tags[i]));
    tag_count++;
  }

  // an out-of-range reply window means a build wrote it with different bounds:
  // fall back to the default rather than schedule replies outside it
  if (rec.reply_window_ms > 0 && rec.reply_window_ms <= FLEET_REPLY_WINDOW_MAX_MS) {
    reply_window_ms = rec.reply_window_ms;
  }

  // Loaded from the backup, so put a good canonical file back at the next
  // opportunity; the backup stays valid meanwhile.
  if (src == PERSIST_LOAD_RECOVERED) markDirty();
}
// ---------------------------------------------------------------- CLI

#define FLEET_USAGE "Err - usage: on|off|chan|tag|reply|seen|forget"
#define FLEET_TAG_USAGE "Err - usage: tag add <tag>|del <tag>|list|clear"
#define FLEET_CHAN_USAGE "Err - usage: chan set <psk-hex>|clear"

static void fleetStatus(FleetManager& fleet, char* reply) {
  char* out = reply;
  int remain = CLI_REPLY_MAX;
  radd(&out, &remain, "%s, chan ", fleet.isEnabled() ? "on" : "off");
  if (fleet.hasChannel()) radd(&out, &remain, "h=%02X", fleet.getChanHash());
  else radd(&out, &remain, "-");
  radd(&out, &remain, ", tags %d/%d", fleet.getTagCount(), FLEET_MAX_TAGS);
  auto& s = fleet.getScripts();
  radd(&out, &remain, "; scripts ran:%lu dup:%lu noid:%lu refused:%lu",
       (unsigned long)s.getRan(), (unsigned long)s.getDup(),
       (unsigned long)s.getNoId(), (unsigned long)s.getRefused());
  radd(&out, &remain, "; pending %d/%d", s.getPendingCount(), FLEET_CLI_QUEUE_DEPTH);
  radd(&out, &remain, "; sched %d/%d", fleet.getSchedCount(), FLEET_SCHED_STORE);
  radd(&out, &remain, "; seen %d/%d", s.getSeenCount(), FLEET_CLI_SEEN_SIZE);
  radd(&out, &remain, "; ackq %d/%d", fleet.getReplyCount(), FLEET_REPLY_STORE);
}

static void fleetTagCLI(FleetManager& fleet, char* params, char* reply) {
  char* p = params;
  char* sub = nextToken(&p);
  if (sub == NULL || strcmp(sub, "list") == 0) {
    if (sub != NULL && !cliNoExtra(p, reply, FLEET_TAG_USAGE)) return;
    if (fleet.getTagCount() == 0) {
      strcpy(reply, "tags: (none) - broadcast scripts only");
      return;
    }
    char* out = reply;
    int remain = CLI_REPLY_MAX;
    radd(&out, &remain, "tags:");
    for (int i = 0; i < fleet.getTagCount(); i++) {
      radd(&out, &remain, " %s", fleet.getTag(i));
    }
  } else if (strcmp(sub, "add") == 0) {
    char* tag = nextToken(&p);
    if (tag == NULL || !cliNoExtra(p, reply, FLEET_TAG_USAGE)) {
      strcpy(reply, FLEET_TAG_USAGE);
      return;
    }
    if (!cliValidToken(tag, FLEET_TAG_LEN)) {
      snprintf(reply, CLI_REPLY_MAX, "Err - tag must be 1..%d chars of [A-Za-z0-9._-]",
               FLEET_TAG_LEN);
      return;
    }
    if (fleet.hasTag(tag)) {   // duplicate: no-op success
      snprintf(reply, CLI_REPLY_MAX, "OK - tag %s already set (%d/%d)",
               tag, fleet.getTagCount(), FLEET_MAX_TAGS);
      return;
    }
    if (!fleet.addTag(tag)) {   // cap reached (charset was validated above)
      snprintf(reply, CLI_REPLY_MAX, "Err - tag store full (%d/%d)",
               fleet.getTagCount(), FLEET_MAX_TAGS);
      return;
    }
    snprintf(reply, CLI_REPLY_MAX, "OK - tag %s added (%d/%d)",
             tag, fleet.getTagCount(), FLEET_MAX_TAGS);
  } else if (strcmp(sub, "del") == 0) {
    char* tag = nextToken(&p);
    if (tag == NULL || !cliNoExtra(p, reply, FLEET_TAG_USAGE)) {
      strcpy(reply, FLEET_TAG_USAGE);
      return;
    }
    if (fleet.delTag(tag)) snprintf(reply, CLI_REPLY_MAX, "OK - tag %s deleted", tag);
    else snprintf(reply, CLI_REPLY_MAX, "Err - tag %s not set", tag);
  } else if (strcmp(sub, "clear") == 0) {
    if (!cliNoExtra(p, reply, FLEET_TAG_USAGE)) return;
    fleet.clearTags();
    strcpy(reply, "OK - tags cleared");
  } else {
    strcpy(reply, FLEET_TAG_USAGE);
  }
}

static void fleetChanCLI(FleetManager& fleet, char* params, char* reply) {
  char* p = params;
  char* sub = nextToken(&p);
  if (sub == NULL) {
    strcpy(reply, FLEET_CHAN_USAGE);
  } else if (strcmp(sub, "set") == 0) {
    char* psk = nextToken(&p);
    if (psk == NULL || !cliNoExtra(p, reply, FLEET_CHAN_USAGE)) {
      strcpy(reply, FLEET_CHAN_USAGE);
      return;
    }
    if (!fleet.setChannel(psk)) {
      strcpy(reply, "Err - psk must be 32 or 64 hex chars");
      return;
    }
    // never echo the psk itself; the hash is what the operator compares
    // against a companion's channel info
    snprintf(reply, CLI_REPLY_MAX, "OK - fleet channel set h=%02X (PSK holder = admin)",
             fleet.getChanHash());
  } else if (strcmp(sub, "clear") == 0) {
    if (!cliNoExtra(p, reply, FLEET_CHAN_USAGE)) return;
    fleet.clearChannel();
    strcpy(reply, "OK - fleet channel cleared");
  } else {
    strcpy(reply, FLEET_CHAN_USAGE);
  }
}

// `fleet reply ...` — the !ack jitter window, node-side tuning for dense fleets
static void fleetReplyCLI(FleetManager& fleet, char* params, char* reply) {
  char* p = params;
  char* tok = nextToken(&p);
  if (tok == NULL) {
    if (!cliNoExtra(p, reply, "Err - usage: reply [<secs>]")) return;
    snprintf(reply, CLI_REPLY_MAX, "reply window %lus",
             (unsigned long)(fleet.getReplyWindowMs() / 1000));
    return;
  }
  long v;
  if (!parseIntRange(tok, 1, FLEET_REPLY_WINDOW_MAX_MS / 1000, &v)) {
    snprintf(reply, CLI_REPLY_MAX, "Err - reply window must be 1..%d seconds",
             (int)(FLEET_REPLY_WINDOW_MAX_MS / 1000));
    return;
  }
  if (!cliNoExtra(p, reply, "Err - usage: reply [<secs>]")) return;
  fleet.setReplyWindowMs((uint32_t)v * 1000);
  snprintf(reply, CLI_REPLY_MAX, "OK - reply window %lds", v);
}

// `fleet seen/forget` — the seen-table levers, same replies as the filter's
// `filter cli` namespace they came from. Hashes stay internal: every lookup
// and reply names the key string the operator chose.
static void fleetSeenCLI(FleetManager& fleet, const char* sub, char* params, char* reply) {
  char* p = params;
  auto& s = fleet.getScripts();
  if (strcmp(sub, "seen") == 0) {
    char* key = nextToken(&p);
    if (key == NULL) {
      if (!cliNoExtra(p, reply, "Err - usage: seen [<key>]")) return;
      snprintf(reply, CLI_REPLY_MAX, "seen %d/%d", s.getSeenCount(), FLEET_CLI_SEEN_SIZE);
    } else {
      if (!cliNoExtra(p, reply, "Err - usage: seen [<key>]")) return;
      if (s.keySeen(key)) snprintf(reply, CLI_REPLY_MAX, "OK - key %s seen", key);
      else snprintf(reply, CLI_REPLY_MAX, "Err - key %s not seen", key);
    }
  } else if (strcmp(sub, "forget") == 0) {
    char* key = nextToken(&p);
    if (key == NULL) { strcpy(reply, "Err - usage: forget <key>|all"); return; }
    if (!cliNoExtra(p, reply, "Err - usage: forget <key>|all")) return;
    if (strcmp(key, "all") == 0) {
      s.forgetAll();
      strcpy(reply, "OK - seen table cleared");
    } else if (s.forgetKey(key)) {
      strcpy(reply, "OK - key forgotten");
    } else {
      strcpy(reply, "Err - key not seen");
    }
  } else {
    strcpy(reply, "Err - usage: seen [<key>]|forget <key>|all");
  }
}

void fleetCLI(FleetManager& fleet, const char* command, char* reply) {
  // An oversized command is refused whole rather than acted on as a prefix, and
  // unbalanced quotes are an error rather than a silent misparse. Both are
  // checked before anything is touched.
  char buf[MAX_PACKET_PAYLOAD + 1];
  if (!cliQuotesBalanced(command)) { strcpy(reply, "Err - unbalanced quotes"); return; }
  if (!cliCopyCommand(buf, sizeof(buf), command)) { strcpy(reply, "Err - command too long"); return; }
  char* p = buf;
  char* cmd = nextToken(&p);

  if (cmd == NULL) {
    if (!cliNoExtra(p, reply, FLEET_USAGE)) return;
    fleetStatus(fleet, reply);
  } else if (strcmp(cmd, "on") == 0) {
    if (!cliNoExtra(p, reply, FLEET_USAGE)) return;
    fleet.setEnabled(true);
    strcpy(reply, "OK - fleet on");
  } else if (strcmp(cmd, "off") == 0) {
    if (!cliNoExtra(p, reply, FLEET_USAGE)) return;
    fleet.setEnabled(false);
    strcpy(reply, "OK - fleet off");
  } else if (strcmp(cmd, "chan") == 0) {
    fleetChanCLI(fleet, p, reply);
  } else if (strcmp(cmd, "tag") == 0) {
    fleetTagCLI(fleet, p, reply);
  } else if (strcmp(cmd, "reply") == 0) {
    fleetReplyCLI(fleet, p, reply);
  } else if (strcmp(cmd, "seen") == 0 || strcmp(cmd, "forget") == 0) {
    fleetSeenCLI(fleet, cmd, p, reply);
  } else {
    strcpy(reply, FLEET_USAGE);
  }
}
