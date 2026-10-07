// FleetManager.cpp — see FleetManager.h for the design overview.

#include "FleetManager.h"
#include "CliUtil.h"

#define FLEET_CFG_FILE "/fleet_cfg"

// The one definition of the out-of-the-box state, shared by the constructor
// and load(): fleet off, no channel, no tags, default reply window.
void FleetManager::resetToDefaults() {
  enabled = false;
  memset(psk, 0, sizeof(psk));
  psk_len = 0;
  chan_hash = 0;
  memset(tags, 0, sizeof(tags));
  tag_count = 0;
  reply_window_ms = FLEET_REPLY_WINDOW_MS;
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