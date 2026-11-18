// ChatBot.cpp — see ChatBot.h for the wiring contract and BOT.md for behaviour.

#include "ChatBot.h"
#include "CliUtil.h"
#include "KeyedChannel.h"
#include <ctype.h>
#include <helpers/TxtDataHelpers.h>   // TXT_TYPE_PLAIN

// ---------------------------------------------------------------- state

ChatBot::ChatBot() : ctx(NULL), send(NULL), jitter(NULL) {
  resetToDefaults();
}

void ChatBot::resetToDefaults() {
  enabled = false;
  name[0] = 0;
  memset(psk, 0, sizeof(psk));
  psk_len = 0;
  chan_hash = 0;
  last_reply_ms = 0;
  // ctx/send/jitter are wiring, not config state: a load() must not clear them
  save_flag.reset();
}

void ChatBot::begin(FILESYSTEM* fs, void* ctx, SendFn send, JitterFn jitter) {
  // wiring is injected first: load() may decide the config needs a repair save
  this->ctx = ctx;
  this->send = send;
  this->jitter = jitter;
  load(fs);
}

void ChatBot::loop(FILESYSTEM* fs) {
  if (save_flag.due()) save(fs);
}

void ChatBot::setEnabled(bool on) {
  if (enabled == on) return;
  enabled = on;
  save_flag.markDirty();
}

void ChatBot::setName(const char* n) {
  if (n == NULL) n = "";
  if (strcmp(name, n) == 0) return;
  StrHelper::strzcpy(name, n, sizeof(name));
  save_flag.markDirty();
}

bool ChatBot::setChannel(const char* value) {
  if (value == NULL || value[0] == 0) return false;
  uint8_t key[32];
  uint8_t len;
  uint8_t hash;
  if (value[0] == '#') {
    size_t n = strlen(value);
    if (n < 2 || n > 15 || !validChannelName(value)) return false;
    deriveChannelKey(value, key, &hash);
    len = 16;
  } else {
    int n = 0;
    if (!cliDecodePskHex(value, key, sizeof(key), &n)) return false;
    len = (uint8_t)n;
    mesh::Utils::sha256(&hash, 1, key, len);
  }
  if (len == psk_len && hash == chan_hash && memcmp(psk, key, sizeof(psk)) == 0) return true;
  memcpy(psk, key, sizeof(psk));
  psk_len = len;
  chan_hash = hash;
  save_flag.markDirty();
  return true;
}

void ChatBot::clearChannel() {
  if (psk_len == 0) return;
  memset(psk, 0, sizeof(psk));
  psk_len = 0;
  chan_hash = 0;
  save_flag.markDirty();
}

int ChatBot::appendChannelByHash(const uint8_t* hash, mesh::GroupChannel dest[],
                                 int max_matches, int filled) {
  if (!enabled || psk_len == 0) return 0;
  return appendKeyedChannel(hash, psk, chan_hash, dest, max_matches, filled);
}

// ---------------------------------------------------------------- matching

enum BotReply { BOT_NONE, BOT_PING, BOT_HELP, BOT_PATH };

// Three ordered checks: the full meshcore-bot alias vocabulary is caught by
// substrings, while one-letter aliases must be the whole (trimmed) text or
// they would match inside almost every message. First match wins.
static BotReply classify(const char* text) {
  if (strstr(text, "ping")) return BOT_PING;
  if (strstr(text, "help") || strcmp(text, "h") == 0) return BOT_HELP;
  if (strstr(text, "test") || strstr(text, "path") || strstr(text, "mt") ||
      strcmp(text, "t") == 0 || strcmp(text, "p") == 0 || strcmp(text, "m") == 0) {
    return BOT_PATH;
  }
  return BOT_NONE;
}

// A sender name is an unauthenticated display label, not an identity.
static void sanitizeSender(const char* in, char* out) {
  size_t n = 0;
  for (const char* p = in; *p; p++) {
    unsigned char c = (unsigned char)*p;
    if (c >= 0x20 && c <= 0x7e && (n > 0 || c != ' ')) {
      if (n < BOT_NAME_MAX) out[n++] = (char)c;
    }
  }
  while (n && out[n - 1] == ' ') n--;
  out[n] = 0;
  if (n == 0) strcpy(out, "anon");
}

// ncbot's getPathItems(): one hex group per hop, sized by the sender's path
// hash size. An empty path is "direct".
static void appendPath(CliBuf& buf, const mesh::Packet* pkt) {
  uint8_t size = pkt->getPathHashSize();
  uint8_t bytes = pkt->getPathByteLen();
  if (bytes == 0 || size == 0) { buf.addStr("direct"); return; }
  if (bytes > MAX_PATH_SIZE) bytes = MAX_PATH_SIZE;
  for (uint8_t i = 0; i < bytes; i++) {
    if (i && i % size == 0) buf.addStr(" ");
    buf.add("%02X", pkt->path[i]);
  }
}

// ---------------------------------------------------------------- receive

void ChatBot::onGroupData(const mesh::Packet* pkt, uint8_t type,
                          const mesh::GroupChannel& channel, const uint8_t* data, size_t len,
                          const RegionEntry* region, const char* node_name) {
  (void)region;
  if (!enabled || psk_len == 0) return;                    // fork-wide off-path gate
  if (type != PAYLOAD_TYPE_GRP_TXT || len < 6 || send == NULL) return;
  if ((data[4] >> 2) != TXT_TYPE_PLAIN) return;
  if (memcmp(psk, channel.secret, sizeof(psk)) != 0) return;  // hash collision guard

  const uint32_t now = millis();
  if (last_reply_ms && (uint32_t)(now - last_reply_ms) < BOT_MIN_INTERVAL_MS) return;

  char sender[24], text[24];
  bool has_text = false, has_sender = false;
  parseGroupText(data, len, sender, sizeof(sender), text, sizeof(text), &has_text, &has_sender);
  (void)has_sender;
  if (!has_text || text[0] == 0) return;

  const char* p = text;
  while (*p == ' ' || *p == '\t') p++;
  if (*p == '!') p++;
  while (*p == ' ' || *p == '\t') p++;
  char lower[sizeof(text)];
  size_t i = 0;
  for (; *p && i + 1 < sizeof(lower); p++, i++) lower[i] = (char)tolower((unsigned char)*p);
  lower[i] = 0;

  BotReply kind = classify(lower);
  if (kind == BOT_NONE) return;

  char reply[BOT_TEXT_MAX + 1];
  if (kind == BOT_HELP) {
    snprintf(reply, sizeof(reply), "Commands: ping, test");
  } else {
    const char* nm = name[0] ? name : (node_name && node_name[0] ? node_name : "bot");
    char from[BOT_NAME_MAX + 1];
    sanitizeSender(sender, from);
    CliBuf buf(reply, sizeof(reply));
    buf.add("%s: @[%s]", nm, from);
    if (kind == BOT_PATH) {
      buf.addStr("\n");
      appendPath(buf, pkt);
    }
  }
  size_t reply_len = strlen(reply);

  uint32_t delay = jitter ? jitter(ctx) : BOT_REPLY_DELAY_MIN_MS;
  if (!send(ctx, psk, chan_hash, pkt->getRxProfile(), delay, reply, reply_len)) return;
  last_reply_ms = now;   // a suppressed send leaves no state behind
}

// ---------------------------------------------------------------- persistence

static bool botRecordValid(const BotCfgRecord& rec) {
  if (memcmp(rec.magic, BOT_CFG_MAGIC, 4) != 0) return false;
  if (rec.version != BOT_CFG_VERSION) return false;
  if (rec.psk_len != 0 && rec.psk_len != 16 && rec.psk_len != 32) return false;
  return memchr(rec.name, 0, sizeof(rec.name)) != NULL;
}

static bool botWriteFile(File& f, void* ctx) {
  const ChatBot& bot = *(const ChatBot*)ctx;
  BotCfgRecord rec;
  memset(&rec, 0, sizeof(rec));
  memcpy(rec.magic, BOT_CFG_MAGIC, 4);
  rec.version = BOT_CFG_VERSION;
  rec.enabled = bot.isEnabled() ? 1 : 0;
  memcpy(rec.name, bot.getName(), sizeof(rec.name));
  rec.psk_len = bot.getPskLen();
  memcpy(rec.psk, bot.getPsk(), sizeof(rec.psk));
  uint16_t crc = crc16_ccitt((const uint8_t*)&rec, sizeof(rec));
  return f.write((const uint8_t*)&rec, sizeof(rec)) == sizeof(rec)
      && f.write((const uint8_t*)&crc, sizeof(crc)) == sizeof(crc);
}

static bool botValidateFile(File& f, void* ctx) {
  (void)ctx;
  BotCfgRecord rec;
  uint8_t crc_bytes[2];
  uint8_t extra;
  if (f.read((uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) return false;
  if (f.read(crc_bytes, sizeof(crc_bytes)) != sizeof(crc_bytes)) return false;
  if (f.read(&extra, 1) != 0) return false;   // exactly one record
  if (!botRecordValid(rec)) return false;
  uint16_t crc;
  memcpy(&crc, crc_bytes, sizeof(crc));
  return crc == crc16_ccitt((const uint8_t*)&rec, sizeof(rec));
}

void ChatBot::save(FILESYSTEM* fs) {
  if (saveStaged(fs, BOT_CFG_FILE, botWriteFile, this,
                 botValidateFile, NULL, botValidateFile, NULL)) {
    save_flag.clear();
  } else {
    save_flag.retryLater();
  }
}

void ChatBot::load(FILESYSTEM* fs) {
  resetToDefaults();
  char path[64];
  PersistLoadSource src = chooseConfigToLoad(fs, BOT_CFG_FILE, botValidateFile, NULL,
                                             path, sizeof(path));
  if (src == PERSIST_LOAD_NONE) return;
  File file = fsOpenRead(fs, path);
  if (!file) return;
  BotCfgRecord rec;
  bool ok = file.read((uint8_t*)&rec, sizeof(rec)) == sizeof(rec);
  file.close();
  if (!ok || !botRecordValid(rec)) return;   // chooseConfigToLoad checked the CRC

  enabled = (rec.enabled == 1);
  memcpy(name, rec.name, sizeof(name));
  psk_len = rec.psk_len;
  memcpy(psk, rec.psk, sizeof(psk));
  if (psk_len) mesh::Utils::sha256(&chan_hash, 1, psk, psk_len);
  if (src == PERSIST_LOAD_RECOVERED) save_flag.markDirty();
}

// ---------------------------------------------------------------- CLI

void botCLI(ChatBot& bot, const char* command, char* reply) {
  char scratch[64];
  if (!cliPrepare(command, scratch, sizeof(scratch), reply)) return;
  char* p = scratch;
  char* verb = nextToken(&p);
  if (verb == NULL) {
    const char* nm = bot.getName()[0] ? bot.getName() : "(node)";
    char chan[16];
    if (bot.getPskLen()) snprintf(chan, sizeof(chan), "h=%02X", bot.getChanHash());
    else strcpy(chan, "(none)");
    snprintf(reply, CLI_REPLY_MAX, "bot: %s, name %s, chan %s",
             bot.isEnabled() ? "on" : "off", nm, chan);
    return;
  }

  if (strcmp(verb, "on") == 0 || strcmp(verb, "off") == 0) {
    if (!cliNoExtra(p, reply, "Err - usage: bot on|off")) return;
    bool on = (verb[1] == 'n');
    bot.setEnabled(on);
    snprintf(reply, CLI_REPLY_MAX, "OK - bot %s", on ? "on" : "off");
  } else if (strcmp(verb, "name") == 0) {
    char* arg = nextToken(&p);
    if (!cliNoExtra(p, reply, "Err - usage: bot name [<text>]")) return;
    if (arg == NULL) {
      bot.setName(NULL);
    } else {
      size_t n = strlen(arg);
      if (n < 1 || n > BOT_NAME_MAX) {
        snprintf(reply, CLI_REPLY_MAX, "Err - name must be 1..16 chars");
        return;
      }
      bot.setName(arg);
    }
    strcpy(reply, "OK - name set");
  } else if (strcmp(verb, "chan") == 0) {
    char* arg = nextToken(&p);
    const char* usage = "Err - usage: chan #<name>|<psk-hex> | chan off";
    if (arg == NULL || strcmp(arg, "off") == 0) {
      if (!cliNoExtra(p, reply, usage)) return;
      bot.clearChannel();
      strcpy(reply, "OK - chan cleared");
    } else {
      if (!cliNoExtra(p, reply, usage)) return;
      if (arg[0] == '#') {
        size_t n = strlen(arg);
        if (n < 2 || n > 15 || !validChannelName(arg)) {
          snprintf(reply, CLI_REPLY_MAX, "Err - chan name must be 1..15 chars");
          return;
        }
      }
      if (!bot.setChannel(arg)) {
        snprintf(reply, CLI_REPLY_MAX, "Err - psk must be 32 or 64 hex digits");
        return;
      }
      snprintf(reply, CLI_REPLY_MAX, "OK - chan h=%02X", bot.getChanHash());
    }
  } else {
    snprintf(reply, CLI_REPLY_MAX, "Err - usage: on|off|name|chan");
  }
}
