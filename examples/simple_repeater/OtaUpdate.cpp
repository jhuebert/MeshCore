// OtaUpdate.cpp — the LoRa OTA engine for the ESP32 simple_repeater firmware.
// See OtaUpdate.h for the wire format, the arm/token contract, and the
// boot-health marker. Fork-owned.

#include "OtaUpdate.h"

#include <Arduino.h>
#include <SHA256.h>
#include <Utils.h>

#include "BatteryGate.h"
#include "CliUtil.h"

#if defined(ESP32)
#include <esp_ota_ops.h>
#endif

#define OTA_USAGE "Usage: ota [status|stage <size> <sha256> <ver>|apply|abort|rollback]"

static uint16_t le16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ------------------------------------------------------------------ slot io

#if defined(ESP32)

// The only code in this file that knows about partitions. Wraps esp_ota_*;
// everything else is flash-agnostic and host-tested against a fake.
class Esp32SlotIo : public OtaSlotIo {
  const esp_partition_t* _inactive = esp_ota_get_next_update_partition(nullptr);
  const esp_partition_t* _running = esp_ota_get_running_partition();
  esp_ota_handle_t _handle = 0;
  bool _begun = false;

public:
  Esp32SlotIo() = default;   // partitions looked up statically at load

  uint32_t slotSize() const override { return _inactive ? _inactive->size : 0; }
  uint32_t sourceSize() const override { return _running ? _running->size : 0; }

  bool beginWrite(uint32_t image_size) override {
    _begun = false;
    if (_inactive == nullptr) return false;
    if (esp_ota_begin(_inactive, image_size, &_handle) != ESP_OK) {
      _handle = 0;
      return false;
    }
    _begun = true;
    return true;
  }
  bool append(const uint8_t* data, size_t len) override {
    return esp_ota_write(_handle, data, len) == ESP_OK;
  }
  bool readSource(uint32_t off, uint8_t* out, size_t len) override {
    return _running != nullptr && esp_partition_read(_running, off, out, len) == ESP_OK;
  }
  bool readStaged(uint32_t off, uint8_t* out, size_t len) override {
    return _inactive != nullptr && esp_partition_read(_inactive, off, out, len) == ESP_OK;
  }
  bool endWrite() override {
    if (!_begun) return false;
    bool ok = esp_ota_end(_handle) == ESP_OK;
    _handle = 0;
    _begun = false;
    return ok;
  }
  void abortWrite() override {
    if (_handle != 0) {
      esp_ota_abort(_handle);
      _handle = 0;
    }
    _begun = false;
  }
  bool setInactiveAsBoot() override {
    return _inactive != nullptr && esp_ota_set_boot_partition(_inactive) == ESP_OK;
  }
};

#endif   // ESP32

// SHA-256 over `size` bytes of one slot, read in 1 KB slices. Shared by the
// OP_END verification and `ota status` (the app's delta base match).
static bool hashSlot(OtaSlotIo* io, bool staged, uint32_t size, uint8_t out[32]) {
  SHA256 sha;
  uint8_t buf[1024];
  uint32_t off = 0;
  while (off < size) {
    size_t n = size - off < sizeof(buf) ? size - off : sizeof(buf);
    if (staged ? !io->readStaged(off, buf, n) : !io->readSource(off, buf, n)) return false;
    sha.update(buf, n);
    off += n;
  }
  sha.finalize(out, 32);
  return true;
}

// Flip the boot partition to the inactive slot and reboot. Returns false when
// the partition cannot be selected (the node keeps running the current
// firmware); on-device reboot() does not return.
static bool flipAndReboot(OtaSlotIo* io, mesh::MainBoard* board) {
  if (io == nullptr || !io->setInactiveAsBoot()) return false;
  if (board != nullptr) board->reboot();   // does not return on-device
  return true;
}

// --------------------------------------------------------------- session

OtaUpdate::OtaUpdate() = default;
OtaUpdate::~OtaUpdate() { resetSession(); }

void OtaUpdate::begin(FILESYSTEM* fs, BatteryGate* gate, mesh::MainBoard* board, const char* firmware_version, mesh::RNG* rng) {
  _fs = fs;
  _gate = gate;
  _board = board;
  _rng = rng;
  _version = firmware_version ? firmware_version : "";

#if defined(ESP32)
  if (_io == nullptr) _io = new Esp32SlotIo();   // lives for the board's lifetime
#endif

  uint16_t nonce;
  if (readPend(&nonce)) {
    // A freshly-applied image is booting: owe the board a verdict
    _health_pending = true;
    _health_nonce = nonce;
  }
}

void OtaUpdate::loop() {
  if (_health_checked) {
    // wrap-safe deadline test: passed once elapsed millis lands in the positive u32 half
    if (_health_pending && (uint32_t)(millis() - _health_deadline) < 0x80000000u) {
      rollbackToPrevious("health timeout");
    }
    return;
  }
  _health_checked = true;
  if (!_health_pending) return;

  if (sendHealthProbe()) {
    _health_sent = true;
    _health_deadline = millis() + kHealthTimeoutMs;
  } else {
    rollbackToPrevious("health probe not sent");
  }
}

void OtaUpdate::onTxResult(const mesh::Packet* pkt, bool ok) {
  if (!_health_pending || !_health_sent) return;
  if (pkt == nullptr || pkt->payload_len != OTA_HEALTH_LEN) return;
  if (pkt->payload[0] != OTA_MAGIC0 || pkt->payload[1] != OTA_MAGIC1 || pkt->payload[2] != OTA_OP_NOP) return;
  if (le16(pkt->payload + 3) != _health_nonce) return;
  if (ok) {
    deletePend();
    _health_pending = false;
  } else {
    rollbackToPrevious("health probe tx failed");
  }
}

void OtaUpdate::resetSession() {
  if (_io != nullptr) _io->abortWrite();
  _state = State::Idle;
  _err = OTA_ERR_NONE;
  _size = 0;
  _written = 0;
  _expect = 1;
}

void OtaUpdate::abortStaging(uint8_t err) {
  MESH_DEBUG_PRINTLN("ota: staging aborted (err=%u)", err);
  resetSession();
  _state = State::Error;
  _err = err;
}

bool OtaUpdate::armSession(uint32_t size, const uint8_t sha[32]) {
  if (_state != State::Idle) return false;   // a busy session keeps its place
  if (_io == nullptr) { _err = OTA_ERR_SLOT; return false; }
  if (_gate != nullptr && _gate->isSuspended()) {
    _err = OTA_ERR_BATTERY;
    return false;
  }
  if (size == 0 || size > _io->slotSize()) {
    _err = OTA_ERR_SLOT;
    return false;
  }
  if (!_io->beginWrite(size)) {
    _err = OTA_ERR_IO;
    return false;
  }
  _size = size;
  _written = 0;
  _expect = 1;
  memcpy(_expected_sha, sha, 32);
  _state = State::Staging;
  _err = OTA_ERR_NONE;
  return true;
}

uint32_t OtaUpdate::ownTargetId() const {
#if defined(OTA_PIO_ENV)
  static uint32_t id = 0;
  if (id == 0) {
    uint8_t hash[4];
    mesh::Utils::sha256(hash, 4, (const uint8_t*)OTA_PIO_ENV, (int)strlen(OTA_PIO_ENV));
    id = le32(hash);
  }
  return id;
#else
  return 0;
#endif
}

void OtaUpdate::onPacket(const mesh::Packet* pkt) {
  if (!otaIsOtaPacket(pkt)) return;
  const uint8_t* payload = pkt->payload;
  const size_t n = pkt->payload_len;

  uint16_t stored_crc = le16(payload + n - 2);
  if (crc16_ccitt(payload, n - 2) != stored_crc) return;   // bad CRC: drop

  const uint8_t op = payload[2];

  if (_io == nullptr) return;   // not an ESP32 (or slot lookup failed): inert

  if (op == OTA_OP_NOP || op == OTA_OP_REPORT) return;

  if (op == OTA_OP_ASK) {
    if (!stagingArmed()) return;
    const uint8_t* body = payload + 3;
    const uint16_t body_len = (uint16_t)(n - 5);
    if (body_len < 1) return;
    const uint8_t ret_path_len = body[0];
    if (!mesh::Packet::isValidPathLen(ret_path_len)) return;   // malformed ask: drop
    const uint16_t path_bytes = (uint16_t)((ret_path_len & 63) * ((ret_path_len >> 6) + 1));
    if (body_len != (uint16_t)(1 + path_bytes)) return;
    sendReport(body + 1, ret_path_len);
    return;
  }

  const uint16_t seq = le16(payload + 3);
  const uint8_t* body = payload + 5;
  const uint16_t body_len = (uint16_t)(n - OTA_FRAME_OVERHEAD);

  switch (op) {
    case OTA_OP_START:
      if (seq != 0) return;          // OP_START is always seq 0
      handleStart(body, body_len);
      return;
    default:
      break;
  }
  if (!stagingArmed()) return;
  if (seq != _expect) return;   // strictly in-order delivery; everything else is a gap or a dup

  applyOp(body, body_len, op);
  if (_state == State::Staging) _expect++;   // Ready/Error need no watermark
}

void OtaUpdate::handleStart(const uint8_t* body, uint16_t len) {
  if (len != OTA_OP_START_MIN) { _err = OTA_ERR_BOUNDS; return; }
  uint32_t size = le32(body);
  const uint8_t* sha = body + 4;
  uint32_t target_id = le32(body + 36);
  const uint8_t* token = body + 40;
  if (target_id != ownTargetId()) { _err = OTA_ERR_TARGET; return; }
  if (_state != State::Idle) return;   // a busy session keeps its place

  // The armed contract decides: single-use, time-boxed, bound to {size, sha256}
  if (!_pending.valid) { _err = OTA_ERR_TOKEN; return; }
  if ((uint32_t)(millis() - _pending.expires_at) < 0x80000000u) {   // expired
    clearPending();
    _err = OTA_ERR_TOKEN;
    return;
  }
  if (_pending.size != size || memcmp(_pending.sha, sha, 32) != 0 || memcmp(_pending.token, token, 4) != 0) {
    _err = OTA_ERR_TOKEN;   // stranger: leave the pending arm intact
    return;
  }
  clearPending();           // consumed by this arm

  armSession(size, sha);
}

void OtaUpdate::applyOp(const uint8_t* body, uint16_t body_len, uint8_t op) {
  switch (op) {
    case OTA_OP_COPY:
      if (body_len != 6) { abortStaging(OTA_ERR_BOUNDS); return; }
      applyCopy(le32(body), le16(body + 4));
      return;
    case OTA_OP_LITERAL: {
      if (body_len < 2) { abortStaging(OTA_ERR_BOUNDS); return; }
      uint16_t dlen = le16(body);
      if (body_len != (uint16_t)(2 + dlen)) { abortStaging(OTA_ERR_BOUNDS); return; }
      applyLiteral(body + 2, dlen);
      return;
    }
    case OTA_OP_END:
      if (body_len != 0) { abortStaging(OTA_ERR_BOUNDS); return; }
      if (_written != _size) { abortStaging(OTA_ERR_BOUNDS); return; }
      {
        uint8_t sha[32];
        if (!hashSlot(_io, /*staged=*/true, _size, sha) || memcmp(sha, _expected_sha, 32) != 0) {
          abortStaging(OTA_ERR_SHA);
          return;
        }
        if (!_io->endWrite()) { abortStaging(OTA_ERR_END); return; }
      }
      _state = State::Ready;
      return;
    default:
      abortStaging(OTA_ERR_BOUNDS);   // unknown op in the stream
      return;
  }
}

bool OtaUpdate::applyCopy(uint32_t base_off, uint16_t len) {
  if ((uint64_t)base_off + len > _io->sourceSize() || (uint64_t)_written + len > _size) {
    abortStaging(OTA_ERR_BOUNDS);
    return false;
  }
  uint8_t buf[256];
  uint32_t off = 0;
  while (off < len) {
    size_t n = (size_t)(len - off) < sizeof(buf) ? (size_t)(len - off) : sizeof(buf);
    if (!_io->readSource(base_off + off, buf, n)) { abortStaging(OTA_ERR_IO); return false; }
    if (!_io->append(buf, n)) { abortStaging(OTA_ERR_IO); return false; }
    off += (uint32_t)n;
  }
  _written += len;
  return true;
}

bool OtaUpdate::applyLiteral(const uint8_t* data, uint16_t len) {
  if ((uint64_t)_written + len > _size) { abortStaging(OTA_ERR_BOUNDS); return false; }
  if (!_io->append(data, len)) { abortStaging(OTA_ERR_IO); return false; }
  _written += len;
  return true;
}

void OtaUpdate::sendReport(const uint8_t* ret_path, uint8_t ret_path_len) {
  uint8_t p[7];
  p[0] = OTA_MAGIC0;
  p[1] = OTA_MAGIC1;
  p[2] = OTA_OP_REPORT;
  p[3] = (uint8_t)(_expect & 0xFF);
  p[4] = (uint8_t)(_expect >> 8);
  uint16_t crc = crc16_ccitt(p, 5);
  p[5] = (uint8_t)(crc & 0xFF);
  p[6] = (uint8_t)(crc >> 8);
  if (_send != nullptr) {
    _send(_send_ctx, p, sizeof(p), ret_path, ret_path_len);
  }
}

bool OtaUpdate::sendHealthProbe() {
  uint8_t p[OTA_HEALTH_LEN] = {OTA_MAGIC0, OTA_MAGIC1, OTA_OP_NOP,
                               (uint8_t)(_health_nonce & 0xFF), (uint8_t)(_health_nonce >> 8)};
  uint16_t crc = crc16_ccitt(p, OTA_HEALTH_LEN - 2);
  p[5] = (uint8_t)(crc & 0xFF);
  p[6] = (uint8_t)(crc >> 8);
  return _send != nullptr && _send(_send_ctx, p, sizeof(p), nullptr, 0);
}

// ------------------------------------------------------------------ /ota_pend

#define OTA_PEND_MAGIC   "OTAP"
// magic(4) nonce(2) crc16(2)
#define OTA_PEND_SIZE    8

bool OtaUpdate::writePend() {
  if (_fs == nullptr) return false;
  uint16_t nonce;
  uint8_t nb[2];
  if (_rng != nullptr) _rng->random(nb, 2); else { nb[0] = (uint8_t)(millis() & 0xFF); nb[1] = (uint8_t)((millis() >> 8) & 0xFF); }
  nonce = (uint16_t)nb[0] | ((uint16_t)nb[1] << 8);

  uint8_t rec[OTA_PEND_SIZE];
  memcpy(rec, OTA_PEND_MAGIC, 4);
  rec[4] = nb[0];
  rec[5] = nb[1];
  uint16_t crc = crc16_ccitt(rec, OTA_PEND_SIZE - 2);
  rec[6] = (uint8_t)(crc & 0xFF);
  rec[7] = (uint8_t)(crc >> 8);

  File f = fsOpenWrite(_fs, OTA_PEND_FILE);
  if (!f) return false;
  size_t n = f.write(rec, sizeof(rec));
  f.flush();
  f.close();
  if (n != sizeof(rec)) { _fs->remove(OTA_PEND_FILE); return false; }

  uint16_t back;
  if (!readPend(&back)) return false;
  return back == nonce;
}

bool OtaUpdate::readPend(uint16_t* nonce) {
  if (_fs == nullptr || !_fs->exists(OTA_PEND_FILE)) return false;
  File f = fsOpenRead(_fs, OTA_PEND_FILE);
  if (!f) return false;
  uint8_t rec[OTA_PEND_SIZE];
  size_t n = f.read(rec, sizeof(rec));
  f.close();
  if (n != sizeof(rec)) return false;
  if (memcmp(rec, OTA_PEND_MAGIC, 4) != 0) return false;
  uint16_t stored = (uint16_t)rec[6] | ((uint16_t)rec[7] << 8);
  if (crc16_ccitt(rec, OTA_PEND_SIZE - 2) != stored) return false;
  *nonce = (uint16_t)rec[4] | ((uint16_t)rec[5] << 8);
  return true;
}

void OtaUpdate::deletePend() {
  if (_fs != nullptr && _fs->exists(OTA_PEND_FILE)) _fs->remove(OTA_PEND_FILE);
}

void OtaUpdate::rollbackToPrevious(const char* why) {
  MESH_DEBUG_PRINTLN("ota: %s; rolling back", why);
  deletePend();                       // old image must not re-trigger a flip
  _health_pending = false;
  flipAndReboot(_io, _board);
}

// --------------------------------------------------------------------- CLI

void OtaUpdate::formatStatus(char* reply, size_t reply_size) {
  if (_io == nullptr) {
    snprintf(reply, reply_size, "running=%s sha=0000000000000000 state=unsupported size=0 off=0 err=0",
             _version);
    return;
  }
  if (!_running_sha_valid) {
    memset(_running_sha, 0, sizeof(_running_sha));
    hashSlot(_io, /*staged=*/false, _io->sourceSize(), _running_sha);
    _running_sha_valid = true;
  }
  const char* state_name = _state == State::Staging ? "staging"
      : _state == State::Ready ? "ready"
      : _state == State::Error ? "error" : "idle";
  char sha16[17];
  for (int i = 0; i < 8; i++) snprintf(sha16 + i * 2, 3, "%02x", _running_sha[i]);
  sha16[16] = 0;
  snprintf(reply, reply_size, "running=%s sha=%s state=%s size=%lu off=%lu err=%u",
           _version, sha16, state_name, (unsigned long)_size, (unsigned long)_written, (unsigned)_err);
}

bool OtaUpdate::stageCommand(const char* size_tok, const char* sha_tok, const char* ver_tok, char* reply) {
  if (_io == nullptr) {
    snprintf(reply, CLI_REPLY_MAX, "Err - OTA unsupported on this platform");
    return false;
  }
  if (_gate != nullptr && _gate->isSuspended()) {
    snprintf(reply, CLI_REPLY_MAX, "Err - battery gate active; staging refused");
    return false;
  }
  long size;
  if (!parseIntRange(size_tok, 1, 0x7FFFFFFFL, &size)) {
    snprintf(reply, CLI_REPLY_MAX, "Err - size must be 1..2147483647");
    return false;
  }
  if (sha_tok == nullptr || strlen(sha_tok) != 64 || !mesh::Utils::fromHex(_pending.sha, 32, sha_tok)) {
    snprintf(reply, CLI_REPLY_MAX, "Err - sha256 must be 64 hex chars");
    return false;
  }
  if (ver_tok == nullptr || strlen(ver_tok) == 0 || strlen(ver_tok) > 63) {
    snprintf(reply, CLI_REPLY_MAX, "Err - ver must be 1..63 bytes");
    return false;
  }
  if (_rng == nullptr) {
    snprintf(reply, CLI_REPLY_MAX, "Err - rng unavailable");
    return false;
  }

  _pending.size = (uint32_t)size;
  _rng->random(_pending.token, sizeof(_pending.token));
  _pending.expires_at = millis() + OTA_PENDING_TTL_MS;
  _pending.valid = true;
  snprintf(reply, CLI_REPLY_MAX, "OK - staging armed (size=%ld) token=%02x%02x%02x%02x",
           size, _pending.token[0], _pending.token[1], _pending.token[2], _pending.token[3]);
  return true;
}

bool OtaUpdate::applyCommand(char* reply) {
  if (_state != State::Ready) {
    snprintf(reply, CLI_REPLY_MAX, "Err - no verified staging (see ota status)");
    return false;
  }
  if (!writePend()) {
    snprintf(reply, CLI_REPLY_MAX, "Err - could not arm boot health marker");
    return false;
  }
  if (!flipAndReboot(_io, _board)) {
    snprintf(reply, CLI_REPLY_MAX, "Err - could not set boot partition");
    return false;
  }
  snprintf(reply, CLI_REPLY_MAX, "OK - applied; rebooting");
  return true;
}

bool OtaUpdate::abortCommand(char* reply) {
  resetSession();
  clearPending();
  snprintf(reply, CLI_REPLY_MAX, "OK - aborted");
  return true;
}

bool OtaUpdate::rollbackCommand(char* reply) {
  deletePend();                       // no health check pending on the other image
  if (!flipAndReboot(_io, _board)) {
    snprintf(reply, CLI_REPLY_MAX, "Err - could not set boot partition");
    return false;
  }
  snprintf(reply, CLI_REPLY_MAX, "OK - rolling back; rebooting");
  return true;
}

void otaCLI(OtaUpdate& ota, const char* sub, char* reply) {
  char scratch[MAX_PACKET_PAYLOAD + 1];
  if (!cliPrepare(sub, scratch, sizeof(scratch), reply)) return;
  char* p = scratch;
  char* cmd = nextToken(&p);

  if (cmd == nullptr) {
    ota.formatStatus(reply, CLI_REPLY_MAX);
    return;
  }
  if (strcmp(cmd, "status") == 0) {
    if (!cliNoExtra(p, reply, OTA_USAGE)) return;
    ota.formatStatus(reply, CLI_REPLY_MAX);
    return;
  }
  if (strcmp(cmd, "stage") == 0) {
    char* size_tok = nextToken(&p);
    char* sha_tok = nextToken(&p);
    char* ver_tok = nextToken(&p);
    if (size_tok == nullptr || sha_tok == nullptr || ver_tok == nullptr || nextToken(&p) != nullptr) {
      snprintf(reply, CLI_REPLY_MAX, "%s", OTA_USAGE);
      return;
    }
    ota.stageCommand(size_tok, sha_tok, ver_tok, reply);
    return;
  }
  if (strcmp(cmd, "apply") == 0) { if (!cliNoExtra(p, reply, OTA_USAGE)) return; ota.applyCommand(reply); return; }
  if (strcmp(cmd, "abort") == 0) { if (!cliNoExtra(p, reply, OTA_USAGE)) return; ota.abortCommand(reply); return; }
  if (strcmp(cmd, "rollback") == 0) { if (!cliNoExtra(p, reply, OTA_USAGE)) return; ota.rollbackCommand(reply); return; }
  snprintf(reply, CLI_REPLY_MAX, "%s", OTA_USAGE);
}
