// OtaUpdate.cpp — the LoRa OTA engine for the ESP32 simple_repeater firmware.
// See OtaUpdate.h for the wire format, the op semantics, the /ota_pend boot
// health record, and the CLI contract. Personal fork feature; fork-owned.

#include "OtaUpdate.h"

#include <Arduino.h>
#include <SHA256.h>
#include <Utils.h>

#include "BatteryGate.h"
#include "CliUtil.h"

#if defined(ESP32)
#include <esp_ota_ops.h>
#endif

#include <new>

#define OTA_USAGE "Usage: ota [status|stage <size> <sha256> <ver>|apply|abort|rollback]"

static uint16_t le16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// ------------------------------------------------------------------ slot io

#if defined(ESP32)

// The only code in this file that knows about partitions. Wraps esp_ota_* and
// the Arduino FILESYSTEM-facing calls; everything else is flash-agnostic and
// host-tested against a fake.
class Esp32SlotIo : public OtaSlotIo {
  const esp_partition_t* _inactive = esp_ota_get_next_update_partition(nullptr);
  const esp_partition_t* _running = esp_ota_get_running_partition();
  esp_ota_handle_t _handle = 0;
  bool _begun = false;

public:
  Esp32SlotIo() = default;   // ctor is trivial; partitions are looked up statically

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

// SHA-256 over `size` bytes of one slot, read in 1 KB slices. The layered
// helper is shared by OP_END verification and the boot health check.
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

void OtaUpdate::begin(FILESYSTEM* fs, BatteryGate* gate, mesh::MainBoard* board, const char* firmware_version) {
  _fs = fs;
  _gate = gate;
  _board = board;
  _version = firmware_version ? firmware_version : "";

#if defined(ESP32)
  if (_io == nullptr) _io = new Esp32SlotIo();   // lives for the board's lifetime
#endif

  uint8_t state;
  uint32_t size;
  uint8_t sha[32];
  if (readPend(&state, &size, sha)) {
    if (state == OTA_PEND_PENDING) {
      // A freshly-applied image is booting: verify + probe before declaring health.
      _health_pending = true;
      _health_size = size;
      _health_nonce = ((uint32_t)sha[0] << 8) | sha[1];
      memcpy(_health_sha, sha, 32);
    } else if (state == OTA_PEND_ROLLED) {
      _rolledback = true;
    }
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

  uint8_t sha[32];
  if (_io == nullptr || !hashSlot(_io, /*staged=*/false, _health_size, sha) ||
      memcmp(sha, _health_sha, 32) != 0) {
    rollbackToPrevious("health slot digest mismatch");
    return;
  }
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
  if (le16(pkt->payload + 3) != (uint16_t)(_health_nonce & 0xFFFF)) return;
  if (ok) {
    deletePend();
    _health_pending = false;
  } else {
    rollbackToPrevious("health probe tx failed");
  }
}

void OtaUpdate::resetSession() {
  if (_io != nullptr) _io->abortWrite();
  delete[] _slots;
  _slots = nullptr;
  _state = State::Idle;
  _err = OTA_ERR_NONE;
  _size = 0;
  _written = 0;
  _expect = 1;
  _arrived = 0;
  _ret_path_len = 0;
}

void OtaUpdate::abortStaging(uint8_t err) {
  MESH_DEBUG_PRINTLN("ota: staging aborted (err=%u)", err);
  resetSession();
  _state = State::Error;
  _err = err;
}

bool OtaUpdate::armSession(uint32_t size, const uint8_t sha[32],
                            const uint8_t* ret_path, uint8_t ret_path_len) {
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
  _slots = new (std::nothrow) WindowSlot[OTA_WINDOW];
  if (_slots == nullptr) {
    _err = OTA_ERR_IO;
    return false;
  }
  if (!_io->beginWrite(size)) {
    delete[] _slots;
    _slots = nullptr;
    _err = OTA_ERR_IO;
    return false;
  }
  _size = size;
  _written = 0;
  _expect = 1;
  _arrived = 0;
  memcpy(_expected_sha, sha, 32);
  _ret_path_len = ret_path_len;
  if (ret_path_len != 0 && ret_path != nullptr) {
    memcpy(_ret_path, ret_path, (ret_path_len & 63) * ((ret_path_len >> 6) + 1));
  }
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
  const uint16_t seq = le16(payload + 3);
  const uint8_t* body = payload + 5;
  const uint16_t body_len = (uint16_t)(n - OTA_FRAME_OVERHEAD);

  if (_io == nullptr) return;   // not an ESP32 (or slot lookup failed): inert

  switch (op) {
    case OTA_OP_NOP:
    case OTA_OP_REPORT:
      return;
    case OTA_OP_START:
      if (seq != 0) return;          // OP_START is always seq 0
      handleStart(body, body_len);
      return;
    case OTA_OP_ASK:
      if (stagingArmed()) sendReport(seq);
      return;
    default:
      break;
  }
  if (!stagingArmed() || _slots == nullptr) return;

  int32_t d = (int32_t)(int16_t)(seq - _expect);   // modular: handles the u16 wrap
  if (d < 0) return;                               // already applied / future window overflow
  if (d >= OTA_WINDOW) return;                     // beyond the window: drop
  _slots[d].len = (uint16_t)(body_len + 1);
  _slots[d].data[0] = op;
  memcpy(_slots[d].data + 1, body, body_len);
  _arrived |= (uint16_t)(1u << d);
  drainWindow();
}

void OtaUpdate::handleStart(const uint8_t* body, uint16_t len) {
  if (len < OTA_OP_START_MIN || len > OTA_OP_START_MIN + MAX_PATH_SIZE) { _err = OTA_ERR_BOUNDS; return; }
  uint32_t size = le32(body);
  const uint8_t* sha = body + 4;
  uint32_t target_id = le32(body + 36);
  uint8_t ret_path_len = body[44];
  if (ret_path_len != 0) {
    if (!mesh::Packet::isValidPathLen(ret_path_len)) { _err = OTA_ERR_BOUNDS; return; }
    uint16_t path_bytes = (uint16_t)((ret_path_len & 63) * ((ret_path_len >> 6) + 1));
    if (len != OTA_OP_START_MIN + path_bytes) { _err = OTA_ERR_BOUNDS; return; }
  } else if (len != OTA_OP_START_MIN) {
    _err = OTA_ERR_BOUNDS;
    return;
  }
  if (target_id != ownTargetId()) { _err = OTA_ERR_TARGET; return; }

  // fw (body+40) is opaque to the node — carried for the app, not used here
  armSession(size, sha, body + OTA_OP_START_MIN, ret_path_len);
}

void OtaUpdate::drainWindow() {
  while (_slots != nullptr && (_arrived & 1)) {
    WindowSlot slot = _slots[0];
    applyOp(slot.data + 1, (uint16_t)(slot.len - 1), slot.data[0]);
    if (_slots == nullptr) return;   // session ended inside applyOp
    _arrived >>= 1;
    memmove(_slots, _slots + 1, (OTA_WINDOW - 1) * sizeof(WindowSlot));
    _expect++;
    if (_state != State::Staging) return;
  }
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
      delete[] _slots;
      _slots = nullptr;
      _arrived = 0;
      _state = State::Ready;
      return;
    default:
      abortStaging(OTA_ERR_BOUNDS);   // unknown op in a committed window
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

void OtaUpdate::sendReport(uint16_t ask_seq) {
  uint8_t p[12];
  p[0] = OTA_MAGIC0;
  p[1] = OTA_MAGIC1;
  p[2] = OTA_OP_REPORT;
  p[3] = (uint8_t)(ask_seq & 0xFF);
  p[4] = (uint8_t)(ask_seq >> 8);
  p[5] = (uint8_t)(_expect & 0xFF);
  p[6] = (uint8_t)(_expect >> 8);
  uint16_t gaps = (uint16_t)~_arrived;   // bit i = seq expect+i never arrived
  p[7] = (uint8_t)(gaps & 0xFF);
  p[8] = (uint8_t)(gaps >> 8);
  uint16_t crc = crc16_ccitt(p, 10);
  p[10] = (uint8_t)(crc & 0xFF);
  p[11] = (uint8_t)(crc >> 8);
  if (_send != nullptr) {
    _send(_send_ctx, p, sizeof(p), _ret_path_len != 0 ? _ret_path : nullptr, _ret_path_len);
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
#define OTA_PEND_VERSION 1
// magic(4) ver(1) state(1) size(4) sha256(32) crc16(2)
#define OTA_PEND_SIZE    44

bool OtaUpdate::writePendRecord(uint8_t state, uint32_t size, const uint8_t sha[32]) {
  if (_fs == nullptr) return false;
  uint8_t rec[OTA_PEND_SIZE];
  memcpy(rec, OTA_PEND_MAGIC, 4);
  rec[4] = OTA_PEND_VERSION;
  rec[5] = state;
  for (int i = 0; i < 4; i++) rec[6 + i] = (uint8_t)(size >> (8 * i));
  memcpy(rec + 10, sha, 32);
  uint16_t crc = crc16_ccitt(rec, OTA_PEND_SIZE - 2);
  rec[42] = (uint8_t)(crc & 0xFF);
  rec[43] = (uint8_t)(crc >> 8);

  File f = fsOpenWrite(_fs, OTA_PEND_FILE);
  if (!f) return false;
  size_t n = f.write(rec, sizeof(rec));
  f.flush();
  f.close();
  if (n != sizeof(rec)) { _fs->remove(OTA_PEND_FILE); return false; }


  // read back before it is allowed to gate anything
  uint8_t back_state;
  uint32_t back_size;
  uint8_t back_sha[32];
  if (!readPend(&back_state, &back_size, back_sha)) return false;
  return back_state == state && back_size == size && memcmp(back_sha, sha, 32) == 0;
}

bool OtaUpdate::readPend(uint8_t* state, uint32_t* size, uint8_t sha[32]) {
  if (_fs == nullptr || !_fs->exists(OTA_PEND_FILE)) return false;
  File f = fsOpenRead(_fs, OTA_PEND_FILE);
  if (!f) return false;
  uint8_t rec[OTA_PEND_SIZE];
  size_t n = f.read(rec, sizeof(rec));
  f.close();
  if (n != sizeof(rec)) return false;
  if (memcmp(rec, OTA_PEND_MAGIC, 4) != 0) return false;
  if (rec[4] != OTA_PEND_VERSION) return false;
  uint16_t stored = (uint16_t)rec[42] | ((uint16_t)rec[43] << 8);
  if (crc16_ccitt(rec, OTA_PEND_SIZE - 2) != stored) return false;
  *state = rec[5];
  *size = le32(rec + 6);
  memcpy(sha, rec + 10, 32);
  return true;
}

void OtaUpdate::deletePend() {
  if (_fs != nullptr && _fs->exists(OTA_PEND_FILE)) _fs->remove(OTA_PEND_FILE);
}

void OtaUpdate::rollbackToPrevious(const char* why) {
  MESH_DEBUG_PRINTLN("ota: %s; rolling back", why);
  writePendRecord(OTA_PEND_ROLLED, _health_size, _health_sha);   // best-effort marker
  _health_pending = false;
  flipAndReboot(_io, _board);
}

// --------------------------------------------------------------------- CLI

void OtaUpdate::formatStatus(char* reply, size_t reply_size) {
  if (_io == nullptr) {
    snprintf(reply, reply_size, "running=%s sha=0000000000000000 state=unsupported size=0 off=0 err=0 rolledback=%d",
             _version, _rolledback ? 1 : 0);
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
  snprintf(reply, reply_size, "running=%s sha=%s state=%s size=%lu off=%lu err=%u rolledback=%u",
           _version, sha16, state_name, (unsigned long)_size, (unsigned long)_written,
           (unsigned)_err, (unsigned)(_rolledback ? 1 : 0));
}

static bool parseFwVersion(const char* s, uint32_t* out) {
  if (s == nullptr || *s == 0) return false;
  if (*s == 'v') s++;
  unsigned nums[3] = {0, 0, 0};
  unsigned pre = 0;
  int idx = 0;
  unsigned acc = 0;
  int digits = 0;
  for (const char* q = s;; q++) {
    char c = *q;
    if (c >= '0' && c <= '9') {
      if (idx > 3 || digits >= 3) return false;
      acc = acc * 10 + (unsigned)(c - '0');
      digits++;
      continue;
    }
    if (c == '.' || c == '-' || c == 0) {
      if (digits == 0 || acc > 255) return false;
      if (c == '.') {
        if (idx >= 2) return false;
        nums[idx++] = acc;
      } else if (c == '-') {
        if (idx != 2) return false;   // pre-release only as MAJOR.MINOR.PATCH-pre
        nums[2] = acc;
        idx = 3;
      } else {
        if (idx == 3) pre = acc; else nums[idx] = acc;
        break;
      }
      acc = 0;
      digits = 0;
      continue;
    }
    return false;
  }
  *out = ((uint32_t)nums[0] << 24) | ((uint32_t)nums[1] << 16) | ((uint32_t)nums[2] << 8) | pre;
  return true;
}

bool OtaUpdate::stageCommand(const char* size_tok, const char* sha_tok, const char* ver_tok, char* reply) {
  if (_io == nullptr) {
    snprintf(reply, CLI_REPLY_MAX, "Err - OTA unsupported on this platform");
    return false;
  }
  if (_state != State::Idle) {
    snprintf(reply, CLI_REPLY_MAX, "Err - staging in progress; abort it first");
    return false;
  }
  long size;
  if (!parseIntRange(size_tok, 1, 0x7FFFFFFFL, &size)) {
    snprintf(reply, CLI_REPLY_MAX, "Err - size must be 1..2147483647");
    return false;
  }
  uint8_t sha[32];
  if (sha_tok == nullptr || strlen(sha_tok) != 64 || !mesh::Utils::fromHex(sha, 32, sha_tok)) {
    snprintf(reply, CLI_REPLY_MAX, "Err - sha256 must be 64 hex chars");
    return false;
  }
  uint32_t ignored_fw;
  if (!parseFwVersion(ver_tok, &ignored_fw)) {
    snprintf(reply, CLI_REPLY_MAX, "Err - ver must be MAJOR[.MINOR[.PATCH][-pre]]");
    return false;
  }
  if (!armSession((uint32_t)size, sha, nullptr, 0)) {
    if (_err == OTA_ERR_BATTERY) {
      snprintf(reply, CLI_REPLY_MAX, "Err - battery gate active; staging refused");
    } else {
      snprintf(reply, CLI_REPLY_MAX, "Err - cannot stage (slot unavailable?)");
    }
    return false;
  }
  snprintf(reply, CLI_REPLY_MAX, "OK - staging armed (size=%ld)", size);
  return true;
}

bool OtaUpdate::applyCommand(char* reply) {
  if (_state != State::Ready) {
    snprintf(reply, CLI_REPLY_MAX, "Err - no verified staging (see ota status)");
    return false;
  }
  if (!writePendRecord(OTA_PEND_PENDING, _size, _expected_sha)) {
    snprintf(reply, CLI_REPLY_MAX, "Err - could not arm boot health record");
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
  if (_rolledback) {
    deletePend();
    _rolledback = false;
  }
  snprintf(reply, CLI_REPLY_MAX, "OK - aborted");
  return true;
}

bool OtaUpdate::rollbackCommand(char* reply) {
  uint8_t zero[32] = {0};
  writePendRecord(OTA_PEND_ROLLED, 0, zero);   // best-effort marker for the old image
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
