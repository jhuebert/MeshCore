// OtaUpdate.cpp — the LoRa OTA engine for the ESP32 simple_repeater firmware.
// See OtaUpdate.h for the wire format, the arm/token contract, and the
// boot-health marker. Fork-owned. The ops ride inside an admin client's REQ
// and the OP_REPORT comes back inside the RESPONSE, so intermediate nodes
// only ever see ordinary request/response traffic.

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

// Console trace for boot verdicts. Host builds have no Serial; the native
// suite asserts the same transitions directly, so this is a no-op there.
static void otaLog(const char* line) {
#if defined(ARDUINO)
  Serial.println(line);
#else
  (void)line;
#endif
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
  if (readPend(&nonce, nullptr)) {
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

size_t OtaUpdate::buildInfoReport(uint8_t* out) {
  uint32_t features = 0;
  uint32_t slot = 0;
  if (_io != nullptr) {
    slot = _io->slotSize();
    features |= OTA_FEAT_DELTA;   // OP_COPY is implemented everywhere with flash
    features |= OTA_FEAT_APPLY;   // dual app slot (ESP32)
  }

  // OP_INFO_REPORT body: wire_ver(1) target(4) slot(4) max_literal(2) features(4)
  //                      ver[32] sha256[32]
  uint8_t p[OTA_INFO_BODY];
  memset(p, 0, sizeof(p));
  size_t i = 0;
  p[i++] = OTA_WIRE_VERSION;
  uint32_t target = ownTargetId();
  for (int k = 0; k < 4; k++) p[i++] = (uint8_t)(target >> (8 * k));
  for (int k = 0; k < 4; k++) p[i++] = (uint8_t)(slot >> (8 * k));
  p[i++] = (uint8_t)(OTA_MAX_LITERAL & 0xFF);
  p[i++] = (uint8_t)(OTA_MAX_LITERAL >> 8);
  for (int k = 0; k < 4; k++) p[i++] = (uint8_t)(features >> (8 * k));
  if (_version != nullptr) {
    size_t n = strlen(_version);
    if (n > 31) n = 31;
    memcpy(&p[OTA_INFO_RUNNING_VER_OFF], _version, n);   // NUL-padded by memset
  }
  if (_io != nullptr) {
    if (!_running_sha_valid) {
      memset(_running_sha, 0, sizeof(_running_sha));
      hashSlot(_io, /*staged=*/false, _io->sourceSize(), _running_sha);
      _running_sha_valid = true;
    }
    memcpy(&p[OTA_INFO_RUNNING_SHA_OFF], _running_sha, 32);
  }

  out[0] = OTA_MAGIC0;
  out[1] = OTA_MAGIC1;
  out[2] = OTA_OP_INFO_REPORT;
  memcpy(out + 3, p, sizeof(p));
  uint16_t crc = crc16_ccitt(out, 3 + sizeof(p));
  out[3 + sizeof(p)] = (uint8_t)(crc & 0xFF);
  out[3 + sizeof(p) + 1] = (uint8_t)(crc >> 8);
  return 3 + sizeof(p) + 2;
}

size_t OtaUpdate::onFrame(const uint8_t* frame, size_t n, uint8_t* report_out, size_t report_cap) {
  if (frame == nullptr || n < 5) return 0;   // magic(2) + op(1) + crc(2); ops carry a seq
  if (frame[0] != OTA_MAGIC0 || frame[1] != OTA_MAGIC1) return 0;
  if (crc16_ccitt(frame, n - 2) != le16(frame + n - 2)) return 0;   // bad CRC: drop

  const uint8_t op = frame[2];

  // OP_INFO is the negotiation handshake: answered in every state, no arm
  // required, tolerant of a body (for forward-compatible query extensions).
  // It does not touch flash and does not create or consume a session.
  if (op == OTA_OP_INFO) {
    if (report_out == nullptr || report_cap < OTA_INFO_REPORT_LEN) return 0;
    return buildInfoReport(report_out);
  }

  if (_io == nullptr) return 0;   // not an ESP32 (or slot lookup failed): inert

  if (op == OTA_OP_NOP || op == OTA_OP_REPORT || op == OTA_OP_INFO_REPORT) return 0;

  if (op == OTA_OP_ASK) {
    if (!stagingArmed()) return 0;
    if (n != 5) return 0;   // OP_ASK carries no body
    if (report_out == nullptr || report_cap < 7) return 0;
    sendReport(report_out);
    return 7;
  }

  const uint16_t seq = le16(frame + 3);
  const uint8_t* body = frame + 5;
  const uint16_t body_len = (uint16_t)(n - OTA_FRAME_OVERHEAD);

  switch (op) {
    case OTA_OP_START:
      if (seq != 0) return 0;          // OP_START is always seq 0
      handleStart(body, body_len);
      return 0;
    default:
      break;
  }
  if (!stagingArmed()) return 0;
  if (seq != _expect) return 0;   // strictly in-order delivery; everything else is a gap or a dup

  applyOp(body, body_len, op);
  if (_state == State::Staging) _expect++;   // Ready/Error need no watermark
  return 0;
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

void OtaUpdate::sendReport(uint8_t* out) {
  out[0] = OTA_MAGIC0;
  out[1] = OTA_MAGIC1;
  out[2] = OTA_OP_REPORT;
  out[3] = (uint8_t)(_expect & 0xFF);
  out[4] = (uint8_t)(_expect >> 8);
  uint16_t crc = crc16_ccitt(out, 5);
  out[5] = (uint8_t)(crc & 0xFF);
  out[6] = (uint8_t)(crc >> 8);
}

bool OtaUpdate::sendHealthProbe() {
  uint8_t p[OTA_HEALTH_LEN] = {OTA_MAGIC0, OTA_MAGIC1, OTA_OP_NOP,
                               (uint8_t)(_health_nonce & 0xFF), (uint8_t)(_health_nonce >> 8)};
  uint16_t crc = crc16_ccitt(p, OTA_HEALTH_LEN - 2);
  p[5] = (uint8_t)(crc & 0xFF);
  p[6] = (uint8_t)(crc >> 8);
  return _send != nullptr && _send(_send_ctx, p, sizeof(p));
}

// ------------------------------------------------------------------ /ota_pend

#define OTA_PEND_MAGIC   "OTAP"
// magic(4) nonce(2) crc16(2)  — the original record, still read: a node that
// already carries one must not lose its pending verdict.
#define OTA_PEND_SIZE_V1 8
// magic(4) attempts(1) nonce(2) crc16(2)
#define OTA_PEND_SIZE    9

bool OtaUpdate::writePend() {
  if (_fs == nullptr) return false;
  uint16_t nonce;
  uint8_t nb[2];
  if (_rng != nullptr) _rng->random(nb, 2); else { nb[0] = (uint8_t)(millis() & 0xFF); nb[1] = (uint8_t)((millis() >> 8) & 0xFF); }
  nonce = (uint16_t)nb[0] | ((uint16_t)nb[1] << 8);

  uint8_t rec[OTA_PEND_SIZE];
  memcpy(rec, OTA_PEND_MAGIC, 4);
  rec[4] = 0;               // boot attempts used so far
  rec[5] = nb[0];
  rec[6] = nb[1];
  uint16_t crc = crc16_ccitt(rec, OTA_PEND_SIZE - 2);
  rec[7] = (uint8_t)(crc & 0xFF);
  rec[8] = (uint8_t)(crc >> 8);

  File f = fsOpenWrite(_fs, OTA_PEND_FILE);
  if (!f) return false;
  size_t n = f.write(rec, sizeof(rec));
  f.flush();
  f.close();
  if (n != sizeof(rec)) { _fs->remove(OTA_PEND_FILE); return false; }

  uint16_t back;
  if (!readPend(&back, nullptr)) return false;
  return back == nonce;
}

/// Read the marker. Accepts both record sizes so a node holding a v1 marker
/// (written by the first OTA firmware) still gets its verdict; a v1 record is
/// reported with attempts = 1, i.e. "this is the boot it was written for".
bool OtaUpdate::readPend(uint16_t* nonce, uint8_t* attempts) {
  if (_fs == nullptr || !_fs->exists(OTA_PEND_FILE)) return false;
  File f = fsOpenRead(_fs, OTA_PEND_FILE);
  if (!f) return false;
  uint8_t rec[OTA_PEND_SIZE];
  size_t n = f.read(rec, sizeof(rec));
  f.close();
  if (memcmp(rec, OTA_PEND_MAGIC, 4) != 0) return false;
  if (n == OTA_PEND_SIZE_V1) {                       // legacy: magic nonce crc
    uint16_t stored = (uint16_t)rec[6] | ((uint16_t)rec[7] << 8);
    if (crc16_ccitt(rec, OTA_PEND_SIZE_V1 - 2) != stored) return false;
    if (nonce != nullptr) *nonce = (uint16_t)rec[4] | ((uint16_t)rec[5] << 8);
    if (attempts != nullptr) *attempts = 1;
    return true;
  }
  if (n != OTA_PEND_SIZE) return false;
  uint16_t stored = (uint16_t)rec[7] | ((uint16_t)rec[8] << 8);
  if (crc16_ccitt(rec, OTA_PEND_SIZE - 2) != stored) return false;
  if (nonce != nullptr) *nonce = (uint16_t)rec[5] | ((uint16_t)rec[6] << 8);
  if (attempts != nullptr) *attempts = rec[4];
  return true;
}

/// Record one more boot of the freshly-applied image. Returns the attempt
/// number, or 0 when the marker is unreadable (nothing is owed then).
uint8_t OtaUpdate::bumpPendAttempts() {
  uint16_t nonce;
  uint8_t attempts;
  if (!readPend(&nonce, &attempts)) return 0;
  attempts++;
  uint8_t rec[OTA_PEND_SIZE];
  memcpy(rec, OTA_PEND_MAGIC, 4);
  rec[4] = attempts;
  rec[5] = (uint8_t)(nonce & 0xFF);
  rec[6] = (uint8_t)(nonce >> 8);
  uint16_t crc = crc16_ccitt(rec, OTA_PEND_SIZE - 2);
  rec[7] = (uint8_t)(crc & 0xFF);
  rec[8] = (uint8_t)(crc >> 8);
  File f = fsOpenWrite(_fs, OTA_PEND_FILE);
  if (!f) {
    otaLog("OTA boot verdict: marker rewrite failed");
    return attempts;
  }
  size_t n = f.write(rec, sizeof(rec));
  f.flush();
  f.close();
  if (n != sizeof(rec)) otaLog("OTA boot verdict: marker short write");
  return attempts;
}

/// Boot-time verdict, called before the radio or the mesh are started.
///
/// The probe needs a working radio, so an image that hangs or crashes before
/// `loop()` reaches the probe can never fail it — and because the marker
/// persists, every boot would retry the same image, forever. Counting the
/// attempts here (no radio, no mesh) turns that boot loop into a rollback
/// after OTA_HEALTH_MAX_ATTEMPTS boots.
void OtaUpdate::earlyBootCheck(FILESYSTEM* fs, mesh::MainBoard* board) {
  if (fs == nullptr || board == nullptr) return;
  if (_fs == nullptr) _fs = fs;
  _board = board;
#if defined(ESP32)
  if (_io == nullptr) _io = new Esp32SlotIo();
#endif
  uint8_t attempts = bumpPendAttempts();
  if (attempts == 0) return;                     // nothing owed
  // Remember the verdict so a fatal start-up failure can act on it without
  // waiting for loop(): begin() has not run yet.
  uint16_t nonce;
  if (readPend(&nonce, nullptr)) {
    _health_pending = true;
    _health_nonce = nonce;
  }
  // Visible on the console: a boot verdict is rare, and when a fresh image is
  // being judged this is the only progress the operator can see.
  {
    char line[64];
    snprintf(line, sizeof(line), "OTA boot verdict: attempt %u of %u",
             (unsigned)attempts, (unsigned)OTA_HEALTH_MAX_ATTEMPTS);
    otaLog(line);
  }
  if (attempts > OTA_HEALTH_MAX_ATTEMPTS) {
    rollbackToPrevious("health attempts exhausted");
  }
}

/// The radio did not initialise. On a freshly-applied image that is a failed
/// verdict, so flip back now: `halt()` would strand the node until someone
/// power-cycled it OTA_HEALTH_MAX_ATTEMPTS times.
bool OtaUpdate::radioInitFailed() {
  if (!_health_pending) return false;
  rollbackToPrevious("radio init failed");
  return true;
}

void OtaUpdate::deletePend() {
  if (_fs != nullptr && _fs->exists(OTA_PEND_FILE)) _fs->remove(OTA_PEND_FILE);
}

void OtaUpdate::rollbackToPrevious(const char* why) {
  MESH_DEBUG_PRINTLN("ota: %s; rolling back", why);
  {
    char line[80];
    snprintf(line, sizeof(line), "OTA rollback: %s", why);
    otaLog(line);
  }
  deletePend();                       // old image must not re-trigger a flip
  _health_pending = false;
  if (!flipAndReboot(_io, _board)) otaLog("OTA rollback: partition flip failed");
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
