// HamMode.cpp: dual Part 97 / Part 15 TX power for the repeater.
// Fork-owned; see HAM.md.

#include "HamMode.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <Utils.h>
#include <helpers/UTF8Helpers.h>

#include "CliUtil.h"
#include "KeyedChannel.h"

// ---------------------------------------------------------------- helpers

static size_t boundedLen(const char* s, size_t cap) {
  size_t n = 0;
  while (n < cap && s[n]) n++;
  return n;
}

static bool isWordChar(unsigned char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9');
}

// nextToken() keeps spaces inside double quotes and does not treat tabs as
// separators, so a quoted or tabbed value must be rejected explicitly before
// it is stored as a "one word" callsign or channel name.
static bool isOneWord(const char* s) {
  if (s == NULL || s[0] == 0) return false;
  for (const char* p = s; *p; p++) {
    if (isspace((unsigned char)*p)) return false;
  }
  return true;
}

// A callsign identifies the station only when it stands alone in the node
// name. A bare substring would let N0CALL activate on "N0CALLS".
static bool containsCallWord(HamAdvertName advert, const char* call) {
  const char* name = advert.text;
  size_t n = strlen(call);
  size_t m = mesh::validUtf8PrefixLength(name, advert.budget);
  if (n == 0 || n > m) return false;
  for (size_t p = 0; p + n <= m; p++) {
    if (p > 0 && isWordChar((unsigned char)name[p - 1])) continue;
    if (isWordChar((unsigned char)name[p + n])) continue;
    size_t i = 0;
    while (i < n && tolower((unsigned char)name[p + i]) ==
                    tolower((unsigned char)call[i])) i++;
    if (i == n) return true;
  }
  return false;
}

// ---------------------------------------------------------------- lifecycle

HamMode::HamMode() {
  resetToDefaults();
}

void HamMode::resetToDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  memcpy(cfg.magic, HAM_CFG_MAGIC, 4);
  cfg.version = HAM_CFG_VERSION;
  cfg.enabled = 0;
  cfg.ham_dbm = (int8_t)HAM_TX_POWER;
  cfg.callsign[0] = 0;
  private_dbm = HAM_TX_POWER;
  applied_dbm = HAM_TX_POWER;
  power_known = restore_power = false;
  n_public = 0;
  n_private = 0;
  next_id_at = 0;
  was_active = false;
  rebuildKeys();
}

void HamMode::rebuildKeys() {
  memset(&public_key, 0, sizeof(public_key));
  mesh::Utils::fromHex(public_key.secret, 16, MESHCORE_PUBLIC_PSK_HEX);
  mesh::Utils::sha256(&public_key.hash, 1, public_key.secret, 16);
  public_key.used = true;

  for (int i = 0; i < HAM_MAX_TAGS; i++) {
    memset(&tag_keys[i], 0, sizeof(tag_keys[i]));
    if (cfg.tags[i][0]) {
      deriveChannelKey(cfg.tags[i], tag_keys[i].secret, &tag_keys[i].hash);
      tag_keys[i].used = true;
    }
  }
}

// ---------------------------------------------------------------- gate

const char* HamMode::inactiveReason(HamAdvertName node_name) const {
  if (!cfg.enabled) return "disabled";
  if (cfg.callsign[0] == 0) return "no callsign";
  if (!containsCallWord(node_name, cfg.callsign)) return "name lacks callsign";
  return NULL;
}

bool HamMode::isActive(HamAdvertName node_name) const {
  return inactiveReason(node_name) == NULL;
}

// ---------------------------------------------------------------- classification

bool HamMode::tryKnownKey(const KnownKey& key, uint8_t hash,
                          const uint8_t* payload, size_t len) const {
  if (!key.used || hash != key.hash) return false;
  // A 1-byte hash collides with private channels about 1 in 256 times, so the
  // 2-byte message MAC has to verify before traffic gets the higher power.
  // matchesKnownKey() has already checked the minimum group payload length.
  memset(hmac_secret, 0, sizeof(hmac_secret));
  memcpy(hmac_secret, key.secret, 16);
  return mesh::Utils::MACThenDecrypt(hmac_secret, decrypt_scratch,
                                     payload, (int)len) > 0;
}

bool HamMode::matchesKnownKey(const uint8_t* payload, size_t len) const {
  // group wire form: channel hash(1) | MAC(2) | ciphertext (block aligned)
  if (len < 1 + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE) return false;
  uint8_t hash = payload[0];
  const uint8_t* body = payload + 1;
  size_t body_len = len - 1;
  if (tryKnownKey(public_key, hash, body, body_len)) return true;
  for (int i = 0; i < HAM_MAX_TAGS; i++) {
    if (tryKnownKey(tag_keys[i], hash, body, body_len)) return true;
  }
  return false;
}

HamTxClass HamMode::classify(const mesh::Packet* pkt) const {
  switch (pkt->getPayloadType()) {
    case PAYLOAD_TYPE_ADVERT:    // signed plaintext, carries the station ID
    case PAYLOAD_TYPE_ACK:       // checksum only, obscures nothing
    case PAYLOAD_TYPE_TRACE:     // plaintext tag plus SNR list
    case PAYLOAD_TYPE_CONTROL:   // plaintext discovery
      return HAM_TX_PUBLIC;

    case PAYLOAD_TYPE_MULTIPART:  // only a multipart ACK is public
      return (pkt->payload_len >= 1 &&
              (pkt->payload[0] & 0x0F) == PAYLOAD_TYPE_ACK)
                 ? HAM_TX_PUBLIC : HAM_TX_ENCRYPTED;

    case PAYLOAD_TYPE_GRP_TXT:
    case PAYLOAD_TYPE_GRP_DATA:
      return matchesKnownKey(pkt->payload, pkt->payload_len)
                 ? HAM_TX_PUBLIC : HAM_TX_ENCRYPTED;

    default:  // REQ, RESPONSE, TXT_MSG, ANON_REQ, PATH, RAW_CUSTOM, unknown
      return HAM_TX_ENCRYPTED;
  }
}

int8_t HamMode::selectPower(const mesh::Packet* pkt, HamAdvertName node_name,
                            int8_t default_dbm) {
  return isActive(node_name) ? activePower(pkt, default_dbm) : default_dbm;
}

int8_t HamMode::activePower(const mesh::Packet* pkt, int8_t default_dbm) {
  if (classify(pkt) == HAM_TX_PUBLIC) {
    n_public++;
    return cfg.ham_dbm;
  }
  n_private++;
  return default_dbm;
}

void HamMode::noteRadioPower(int8_t dbm, bool ok) {
  private_dbm = dbm;
  power_known = ok;
  if (ok) {
    applied_dbm = dbm;
    restore_power = false;
  }
}

// ---------------------------------------------------------------- station ID

void HamMode::noteAdvertOnAir(uint32_t now) {
  if (!was_active) return;
  // The send is delayed by HAM_ID_SEND_DELAY_MS, so arm short by that much to
  // allow for that delay when scheduling the next ID.
  next_id_at = now + HAM_ID_INTERVAL_MS - HAM_ID_SEND_DELAY_MS;
}

bool HamMode::idDue(uint32_t now) const {
  // Unsigned subtraction gives the right answer across a millis() wrap for any
  // deadline within the 10-minute window.
  return was_active &&
         (uint32_t)(now - next_id_at) < 0x80000000UL;
}

uint32_t HamMode::secondsToId(uint32_t now) const {
  if (!was_active) return 0;
  uint32_t remaining = (uint32_t)(next_id_at - now);
  if (remaining > HAM_ID_INTERVAL_MS) return 0;   // past due
  return remaining / 1000UL;
}

void HamMode::loop(uint32_t now, FILESYSTEM* fs, HamAdvertName node_name) {
  if (save_flag.due()) save(fs);

  // Activation is a transition, not a command: `ham on`, a stored config at
  // boot, and `set name` after the callsign was set all take the same path.
  bool active = isActive(node_name);
  if (active && !was_active) {
    next_id_at = now;   // schedule an ID on this pass
  } else if (!active && was_active) {
    next_id_at = 0;
  }
  was_active = active;
}

// ---------------------------------------------------------------- settings

int HamMode::getTagCount() const {
  int n = 0;
  for (int i = 0; i < HAM_MAX_TAGS; i++) if (cfg.tags[i][0]) n++;
  return n;
}

int HamMode::findTag(const char* name) const {
  for (int i = 0; i < HAM_MAX_TAGS; i++) {
    if (cfg.tags[i][0] && strcmp(cfg.tags[i], name) == 0) return i;
  }
  return -1;
}

void HamMode::setEnabled(bool on) {
  if (cfg.enabled == (uint8_t)(on ? 1 : 0)) return;
  cfg.enabled = on ? 1 : 0;
  markDirty();
}

void HamMode::setCallsign(const char* call) {
  strncpy(cfg.callsign, call, HAM_CALLSIGN_MAX_LEN - 1);
  cfg.callsign[HAM_CALLSIGN_MAX_LEN - 1] = 0;
  markDirty();
}

void HamMode::setHamDbm(int8_t dbm) {
  if (dbm < -9 || dbm > 30) return;
  cfg.ham_dbm = dbm;
  markDirty();
}

bool HamMode::addTag(const char* name) {
  if (findTag(name) >= 0) return false;
  for (int i = 0; i < HAM_MAX_TAGS; i++) {
    if (!cfg.tags[i][0]) {
      strncpy(cfg.tags[i], name, HAM_TAG_MAX_LEN - 1);
      cfg.tags[i][HAM_TAG_MAX_LEN - 1] = 0;
      rebuildKeys();
      markDirty();
      return true;
    }
  }
  return false;   // full
}

bool HamMode::removeTag(const char* name) {
  int i = findTag(name);
  if (i < 0) return false;
  cfg.tags[i][0] = 0;
  rebuildKeys();
  markDirty();
  return true;
}

// ---------------------------------------------------------------- persistence

static bool hamValidateRecord(const HamCfgRecord& rec) {
  if (memcmp(rec.magic, HAM_CFG_MAGIC, 4) != 0) return false;
  if (rec.version != HAM_CFG_VERSION) return false;
  if (rec.enabled > 1) return false;
  if (rec.ham_dbm < -9 || rec.ham_dbm > 30) return false;
  size_t len = boundedLen(rec.callsign, HAM_CALLSIGN_MAX_LEN);
  if (len && (len < 3 || len == HAM_CALLSIGN_MAX_LEN || !isOneWord(rec.callsign))) return false;
  for (int i = 0; i < HAM_MAX_TAGS; i++) {
    if (!rec.tags[i][0]) continue;
    if (rec.tags[i][0] != '#') return false;
    size_t n = boundedLen(rec.tags[i], HAM_TAG_MAX_LEN);
    if (n < 2 || n >= HAM_TAG_MAX_LEN || !isOneWord(rec.tags[i])) return false;
  }
  return true;
}

static bool hamWriteFile(File& f, void* ctx) {
  HamMode& ham = *(HamMode*)ctx;
  uint8_t buf[HAM_CFG_RECORD_BYTES];
  memcpy(buf, &ham.getRecord(), HAM_CFG_PAYLOAD_BYTES);
  uint16_t crc = crc16_ccitt(buf, HAM_CFG_PAYLOAD_BYTES);
  memcpy(&buf[HAM_CFG_PAYLOAD_BYTES], &crc, 2);
  return f.write(buf, HAM_CFG_RECORD_BYTES) == HAM_CFG_RECORD_BYTES;
}

static bool hamValidateFile(File& f, void* ctx) {
  (void)ctx;
  uint8_t buf[HAM_CFG_RECORD_BYTES];
  if (f.read(buf, HAM_CFG_RECORD_BYTES) != HAM_CFG_RECORD_BYTES) return false;
  uint8_t extra;
  if (f.read(&extra, 1) != 0) return false;   // reject an overlong record
  uint16_t stored, computed;
  memcpy(&stored, &buf[HAM_CFG_PAYLOAD_BYTES], 2);
  computed = crc16_ccitt(buf, HAM_CFG_PAYLOAD_BYTES);
  if (stored != computed) return false;
  HamCfgRecord rec;
  memcpy(&rec, buf, sizeof(rec));
  return hamValidateRecord(rec);
}

void HamMode::save(FILESYSTEM* fs) {
  bool ok = saveStaged(fs, HAM_CFG_FILE, hamWriteFile, this,
                       hamValidateFile, NULL, hamValidateFile, NULL);
  if (ok) save_flag.clear();
  else save_flag.retryLater();
}

void HamMode::load(FILESYSTEM* fs) {
  resetToDefaults();   // a load failure always leaves the node looking stock
  save_flag.reset();

  char path[64];
  PersistLoadSource src = chooseConfigToLoad(fs, HAM_CFG_FILE, hamValidateFile,
                                              NULL, path, sizeof(path));
  if (src == PERSIST_LOAD_NONE) return;
  File file = fsOpenRead(fs, path);
  if (!file) return;
  uint8_t buf[HAM_CFG_RECORD_BYTES];
  bool got = file.read(buf, HAM_CFG_RECORD_BYTES) == HAM_CFG_RECORD_BYTES;
  file.close();
  if (!got) return;

  uint16_t stored, computed;
  memcpy(&stored, &buf[HAM_CFG_PAYLOAD_BYTES], 2);
  computed = crc16_ccitt(buf, HAM_CFG_PAYLOAD_BYTES);
  if (stored != computed) return;
  HamCfgRecord rec;
  memcpy(&rec, buf, sizeof(rec));
  if (!hamValidateRecord(rec)) return;

  applyRecord(rec);
  if (src == PERSIST_LOAD_RECOVERED) markDirty();
}

// ---------------------------------------------------------------- CLI

#define HAM_USAGE                                                              \
  "Err - usage: ham | ham on | ham off | ham callsign [CALL] | ham tx [N] | "   \
  "ham channel add|remove|list [#name] | ham channels | ham stats"

static void cliListChannels(HamMode& ham, char* reply) {
  CliBuf buf(reply, CLI_REPLY_MAX);
  buf.addStr("Public");
  for (int i = 0; i < HAM_MAX_TAGS; i++) {
    if (ham.getTag(i)[0]) buf.add(", %s", ham.getTag(i));
  }
}

static void cliStatus(HamMode& ham, HamAdvertName node_name, uint32_t now, char* reply) {
  CliBuf buf(reply, CLI_REPLY_MAX);
  if (!ham.isEnabled()) {
    buf.addStr("off - all packets use normal tx power");
  } else if (ham.getCallsign()[0] == 0) {
    buf.addStr("waiting - no callsign");
  } else if (!ham.isActive(node_name)) {
    buf.add("waiting - node name must include %s", ham.getCallsign());
  } else {
    buf.add("on - call %s, ham tx %d, set tx %d, tags %d, next ID %lus",
            ham.getCallsign(), ham.getHamDbm(), ham.getPrivateDbm(),
            ham.getTagCount(), (unsigned long)ham.secondsToId(now));
  }
}

void hamCLI(HamMode& ham, HamAdvertName node_name, const char* command, char* reply) {
  char scratch[MAX_PACKET_PAYLOAD + 1];
  if (!cliPrepare(command, scratch, sizeof(scratch), reply)) return;
  char* p = scratch;
  char* cmd = nextToken(&p);

  if (cmd == NULL || strcmp(cmd, "status") == 0) {
    if (!cliNoExtra(p, reply, HAM_USAGE)) return;
    cliStatus(ham, node_name, millis(), reply);
  } else if (strcmp(cmd, "help") == 0) {
    if (!cliNoExtra(p, reply, HAM_USAGE)) return;
    snprintf(reply, CLI_REPLY_MAX, "%s", HAM_USAGE);
  } else if (strcmp(cmd, "on") == 0) {
    if (!cliNoExtra(p, reply, HAM_USAGE)) return;
    ham.setEnabled(true);
    snprintf(reply, CLI_REPLY_MAX, "OK - ham on");
  } else if (strcmp(cmd, "off") == 0) {
    if (!cliNoExtra(p, reply, HAM_USAGE)) return;
    ham.setEnabled(false);
    snprintf(reply, CLI_REPLY_MAX, "OK - ham off");
  } else if (strcmp(cmd, "callsign") == 0) {
    char* tok = nextToken(&p);
    if (tok == NULL) {
      if (!cliNoExtra(p, reply, HAM_USAGE)) return;
      if (ham.getCallsign()[0]) snprintf(reply, CLI_REPLY_MAX, "callsign is %s", ham.getCallsign());
      else snprintf(reply, CLI_REPLY_MAX, "no callsign set");
    } else {
      if (!cliNoExtra(p, reply, HAM_USAGE)) return;
      size_t n = strlen(tok);
      if (!isOneWord(tok) || n < 3 || n > HAM_CALLSIGN_MAX_LEN - 1) {
        snprintf(reply, CLI_REPLY_MAX, "Err - callsign must be one word, 3..15 chars");
        return;
      }
      char up[HAM_CALLSIGN_MAX_LEN];
      for (size_t i = 0; i < n; i++) up[i] = (char)toupper((unsigned char)tok[i]);
      up[n] = 0;
      ham.setCallsign(up);
      CliBuf buf(reply, CLI_REPLY_MAX);
      buf.add("OK - callsign set to %s", up);
      // With lat/lon the advert name budget is 23 bytes. Warn when the live
      // name is over that, whatever the current position setting.
      if (strlen(node_name.text) > 23) {
        buf.addStr("; warning: name over 23 bytes may be cut from adverts with lat/lon");
      }
    }
  } else if (strcmp(cmd, "tx") == 0) {
    char* tok = nextToken(&p);
    if (tok == NULL) {
      if (!cliNoExtra(p, reply, HAM_USAGE)) return;
      snprintf(reply, CLI_REPLY_MAX, "ham tx is %d dBm", ham.getHamDbm());
    } else {
      long v;
      if (!parseIntRange(tok, -9, 30, &v)) {
        snprintf(reply, CLI_REPLY_MAX, "Err - ham tx must be a number from -9 to 30");
        return;
      }
      if (!cliNoExtra(p, reply, HAM_USAGE)) return;
      ham.setHamDbm((int8_t)v);
      snprintf(reply, CLI_REPLY_MAX, "OK - ham tx set to %ld dBm", v);
    }
  } else if (strcmp(cmd, "channels") == 0) {
    if (!cliNoExtra(p, reply, HAM_USAGE)) return;
    cliListChannels(ham, reply);
  } else if (strcmp(cmd, "channel") == 0) {
    char* sub = nextToken(&p);
    if (sub == NULL || strcmp(sub, "list") == 0) {
      if (!cliNoExtra(p, reply, HAM_USAGE)) return;
      cliListChannels(ham, reply);
      return;
    }
    bool add = strcmp(sub, "add") == 0;
    bool remove = strcmp(sub, "remove") == 0;
    if (!add && !remove) {
      if (cliNoExtra(p, reply, HAM_USAGE)) snprintf(reply, CLI_REPLY_MAX, "%s", HAM_USAGE);
      return;
    }
    char* tok = nextToken(&p);
    if (tok == NULL) {
      snprintf(reply, CLI_REPLY_MAX, "%s", HAM_USAGE);
      return;
    }
    if (!cliNoExtra(p, reply, HAM_USAGE)) return;

    size_t tlen = strlen(tok);
    size_t want = (tok[0] == '#') ? tlen : tlen + 1;
    if (!isOneWord(tok) || want < 2 || want > HAM_TAG_MAX_LEN - 1) {
      snprintf(reply, CLI_REPLY_MAX,
               "Err - channel name must be one word, max 14 characters after #");
      return;
    }
    char name[HAM_TAG_MAX_LEN];
    size_t n = 0;
    if (tok[0] != '#') name[n++] = '#';
    memcpy(name + n, tok, tlen);
    name[n + tlen] = 0;

    if (add) {
      if (ham.findTag(name) >= 0) {
        snprintf(reply, CLI_REPLY_MAX, "Err - %s is already on the list", name);
      } else if (ham.getTagCount() >= HAM_MAX_TAGS) {
        snprintf(reply, CLI_REPLY_MAX,
                 "Err - channel list full (max %d); remove one first", HAM_MAX_TAGS);
      } else {
        ham.addTag(name);
        snprintf(reply, CLI_REPLY_MAX, "OK - %s now uses ham power", name);
      }
    } else {
      if (ham.removeTag(name)) {
        snprintf(reply, CLI_REPLY_MAX, "OK - %s removed", name);
      } else {
        snprintf(reply, CLI_REPLY_MAX, "Err - %s is not on the list", name);
      }
    }
  } else if (strcmp(cmd, "stats") == 0) {
    if (!cliNoExtra(p, reply, HAM_USAGE)) return;
    CliBuf buf(reply, CLI_REPLY_MAX);
    buf.add("%lu public, %lu private; applied ",
            (unsigned long)ham.getPublicCount(), (unsigned long)ham.getPrivateCount());
    if (ham.powerKnown()) buf.add("%d dBm", ham.getAppliedDbm());
    else buf.addStr("unknown");
    buf.add("; next ID %lus", (unsigned long)ham.secondsToId(millis()));
  } else {
    snprintf(reply, CLI_REPLY_MAX, "%s", HAM_USAGE);
  }
}
