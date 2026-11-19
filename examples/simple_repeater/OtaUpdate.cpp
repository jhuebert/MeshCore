// OtaUpdate.cpp — the LoRa OTA engine for the ESP32 simple_repeater firmware.
// See OtaUpdate.h for the wire format, the arm/token contract, and the
// boot-health marker. Fork-owned. The ops ride inside an admin client's REQ
// and the OP_REPORT comes back inside the RESPONSE, so intermediate nodes
// only ever see ordinary request/response traffic.

#include "OtaUpdate.h"

#include <Arduino.h>
#include <stdlib.h>
#include <SHA256.h>

// Streaming raw-DEFLATE decompressor. ESP32 uses the miniz implementation in
// ROM (zero flash cost); native tests use the host's zlib. Both consume the
// same raw DEFLATE bytes produced by the app.
#if defined(ESP32)
#include <esp_rom/miniz.h>
struct ZInflater {
  tinfl_decompressor d;
  uint8_t* scratch;
  bool done = false;
};
#else
#include <zlib.h>
struct ZInflater {
  z_stream s;
  uint8_t* scratch;
  bool started = false;
  bool done = false;
};
#endif

typedef bool (*ZEmit)(void* ctx, const uint8_t* data, size_t len);

static ZInflater* zInit() {
  auto* z = new ZInflater();
  if (z == nullptr) return nullptr;
  // A DEFLATE window is up to 32 KB; both decoders need that much scratch.
  z->scratch = (uint8_t*)malloc(32768);
  if (z->scratch == nullptr) { delete z; return nullptr; }
#if defined(ESP32)
  tinfl_init(&z->d);
#else
  memset(&z->s, 0, sizeof(z->s));
  if (inflateInit2(&z->s, -15) != Z_OK) {
    free(z->scratch);
    delete z;
    return nullptr;
  }
  z->started = true;
#endif
  return z;
}

static bool zFeed(ZInflater* z, const uint8_t* in, size_t in_len,
                  uint8_t* scratch, size_t scratch_cap, bool more,
                  bool* done, ZEmit emit, void* ctx) {
#if defined(ESP32)
  size_t in_left = in_len;
  uint8_t* const base = scratch;
  const uint32_t flags = more ? TINFL_FLAG_HAS_MORE_INPUT : 0;
  while (in_left > 0 && !z->done) {
    size_t out_left = scratch_cap;
    size_t in_before = in_left;
    tinfl_status st = tinfl_decompress(&z->d, in + (in_len - in_left), &in_left,
        base, base + scratch_cap, &out_left, flags);
    if (st < TINFL_STATUS_DONE) return false;
    const size_t produced = scratch_cap - out_left;
    if (produced > 0 && !emit(ctx, scratch, produced)) return false;
    if (st == TINFL_STATUS_DONE) { z->done = true; *done = true; break; }
    if (in_left == in_before && produced == 0) break;
  }
  return true;
#else
  z->s.next_in = const_cast<Bytef*>(in);
  z->s.avail_in = in_len;
  while (!z->done) {
    z->s.next_out = scratch;
    z->s.avail_out = scratch_cap;
    const int rc = inflate(&z->s, Z_NO_FLUSH);
    if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) return false;
    const size_t produced = scratch_cap - z->s.avail_out;
    if (produced > 0 && !emit(ctx, scratch, produced)) return false;
    if (rc == Z_STREAM_END) { z->done = true; *done = true; break; }
    if (z->s.avail_in == 0) break;
    if (produced == 0) break;
  }
  return true;
#endif
}

static void zFree(ZInflater* z) {
  if (z == nullptr) return;
#if !defined(ESP32)
  if (z->started) inflateEnd(&z->s);
#endif
  free(z->scratch);
  delete z;
}

#include <Utils.h>

#include "BatteryGate.h"
#include "CliUtil.h"
#include "PersistUtil.h"             // crc16_ccitt (the wire CRC)

#if defined(ESP32)
#include <esp_ota_ops.h>
#endif

#define OTA_USAGE "Usage: ota [status|stage <size> <sha256> <ver>|apply|abort|rollback]"

static uint16_t le16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint8_t* putLe16(uint8_t* p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  return p + 2;
}
static uint8_t* putLe32(uint8_t* p, uint32_t v) {
  for (int k = 0; k < 4; k++) p[k] = (uint8_t)(v >> (8 * k));
  return p + 4;
}

// Close an outgoing frame whose payload (magic..body) is already in `out`:
// appends the CRC-16 and returns the total frame length. Every frame this
// engine sends ends exactly this way.
static size_t sealFrame(uint8_t* out, size_t payload_len) {
  uint16_t crc = crc16_ccitt(out, payload_len);
  out[payload_len] = (uint8_t)(crc & 0xFF);
  out[payload_len + 1] = (uint8_t)(crc >> 8);
  return payload_len + 2;
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
// OP_END verification (staged) and hashRunningImage() (running).
static bool hashSlot(OtaSlotIo* io, bool staged, uint32_t start, uint32_t size, uint8_t out[32]) {
  SHA256 sha;
  uint8_t buf[1024];
  uint32_t off = start;
  const uint32_t end = start + size;
  while (off < end) {
    size_t n = end - off < sizeof(buf) ? end - off : sizeof(buf);
    if (staged ? !io->readStaged(off, buf, n) : !io->readSource(off, buf, n)) return false;
    sha.update(buf, n);
    off += n;
  }
  sha.finalize(out, 32);
  return true;
}

// One slot read, staged or running.
static bool slotRead(OtaSlotIo* io, bool staged, uint32_t off, uint8_t* out, size_t len) {
  return staged ? io->readStaged(off, out, len) : io->readSource(off, out, len);
}

// Length of the app image in the *running* slot: the ESP image header
// (24 bytes: magic, segment count, ..., chip id, hash_appended) followed by
// each segment header + payload, a checksum byte and 16-byte padding. This is
// exactly what an update wrote — the .bin length — not the erase-filled
// partition. Returns 0 when the bytes are not an ESP app image.
static uint32_t runningImageLength(OtaSlotIo* io) {
  uint8_t h[24];
  if (!slotRead(io, false, 0, h, sizeof(h))) return 0;
  if (h[0] != 0xE9 || h[1] < 1 || h[1] > 16) return 0;   // magic, segment count
  uint32_t off = sizeof(h);
  for (uint8_t i = 0; i < h[1]; i++) {
    uint8_t seg[8];
    if (!slotRead(io, false, off, seg, sizeof(seg))) return 0;
    uint32_t len = (uint32_t)seg[4] | ((uint32_t)seg[5] << 8) |
                   ((uint32_t)seg[6] << 16) | ((uint32_t)seg[7] << 24);
    off += 8 + len;
    off = (off + 3) & ~3u;                               // payloads are 4-byte aligned
    if (off >= io->sourceSize()) return 0;               // garbage: refuse
  }
  off += 1;                                              // trailing checksum byte
  return (off + 15) & ~15u;                              // images are padded to 16
}

// SHA-256 of the running app image, over exactly the bytes the update wrote.
// esptool appends a SHA-256 of the image to every .bin by default; it is
// included when it verifies against the computed hash, so this equals the hash
// the app computes over its own image file (the delta base match). Returns
// false when the slot does not hold a recognisable image.
static bool hashRunningImage(OtaSlotIo* io, uint8_t out[32], uint32_t* hashed_len = nullptr) {
  const uint32_t len = runningImageLength(io);
  if (len == 0) return false;
  if (!hashSlot(io, /*staged=*/false, 0, len, out)) return false;
  uint8_t digest[32];
  if (slotRead(io, false, len, digest, sizeof(digest)) && memcmp(digest, out, 32) == 0) {
    if (!hashSlot(io, /*staged=*/false, 0, len + sizeof(digest), out)) return false;
    if (hashed_len != nullptr) *hashed_len = len + sizeof(digest);
    return true;
  }
  if (hashed_len != nullptr) *hashed_len = len;
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

  adoptPendVerdict();
}

// The marker means this boot of a freshly-applied image owes the board a
// verdict. Adopted by both entry points (earlyBootCheck before the radio
// starts, begin() for the paths that skip it).
bool OtaUpdate::adoptPendVerdict() {
  uint16_t nonce;
  if (!readPend(&nonce, nullptr)) return false;
  _health_pending = true;
  _health_nonce = nonce;
  return true;
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
  if (pkt->payload[0] != OTA_MAGIC0 || pkt->payload[1] != OTA_MAGIC1 ||
      pkt->payload[2] != OTA_OP_NOP) {
    return;
  }
  if (le16(pkt->payload + 3) != _health_nonce) return;
  if (ok) {
    deletePend();
    _health_pending = false;
  } else {
    rollbackToPrevious("health probe tx failed");
  }
}

void OtaUpdate::resetSession() {
  closeInflater();
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

void OtaUpdate::armSession(uint32_t size, const uint8_t sha[32]) {
  if (_io == nullptr) { _err = OTA_ERR_SLOT; return; }
  if (_gate != nullptr && _gate->isSuspended()) { _err = OTA_ERR_BATTERY; return; }
  if (size == 0 || size > _io->slotSize()) { _err = OTA_ERR_SLOT; return; }
  if (!_io->beginWrite(size)) { _err = OTA_ERR_IO; return; }
  _size = size;
  _written = 0;
  _expect = 1;
  memcpy(_expected_sha, sha, 32);
  _state = State::Staging;
  _err = OTA_ERR_NONE;
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
    features |= OTA_FEAT_BLOCK_HASH;   // read-only running-image hash oracle
  }

  // OP_INFO_REPORT body: wire_ver(1) target(4) slot(4) max_literal(2) features(4)
  //                      ver[32] sha256[32]
  uint8_t p[OTA_INFO_BODY];
  memset(p, 0, sizeof(p));
  uint8_t* q = p;
  *q++ = OTA_WIRE_VERSION;
  q = putLe32(q, ownTargetId());
  q = putLe32(q, slot);
  q = putLe16(q, OTA_MAX_LITERAL);
  putLe32(q, features);
  memcpy(&p[OTA_INFO_RUNNING_VER_OFF], _version, strlen(_version) > 31 ? 31 : strlen(_version));
  if (_io != nullptr) {
    ensureRunningSha();
    memcpy(&p[OTA_INFO_RUNNING_SHA_OFF], _running_sha, 32);
  }

  out[0] = OTA_MAGIC0;
  out[1] = OTA_MAGIC1;
  out[2] = OTA_OP_INFO_REPORT;
  memcpy(out + 3, p, sizeof(p));
  return sealFrame(out, 3 + sizeof(p));
}

uint32_t OtaUpdate::ensureRunningLength() {
  if (_running_len_valid) return _running_len;
  _running_len_valid = true;
  // The digest of the hashed bytes is the length the app sees in its .bin.
  // ensureRunningSha() computes both in one pass over flash.
  ensureRunningSha();
  return _running_len;
}

size_t OtaUpdate::buildBlockHashReport(const uint8_t* body, uint16_t body_len, uint8_t* out) {
  if (body == nullptr || body_len < 1 || out == nullptr) return 0;
  const uint8_t count = body[0];
  const uint32_t running_len = ensureRunningLength();
  uint8_t p[OTA_HASH_RANGE_BODY(OTA_HASH_MAX_RANGES)];
  memset(p, 0, sizeof(p));
  putLe32(p, running_len);
  p[4] = count;

  if (count == 0) {
    // Metadata query: fixed segment table (six slots, unused ones zero) plus
    // the whole-image hash. One round trip gives the app everything it needs
    // to align segments and detect an identical image.
    if (body_len != 1) return 0;
    uint8_t seg_count = 0;
    if (_io != nullptr && running_len > 0) {
      uint8_t h[24];
      if (slotRead(_io, false, 0, h, sizeof(h)) && h[0] == 0xE9 && h[1] >= 1 && h[1] <= 16) {
        uint32_t off = sizeof(h);
        for (uint8_t i = 0; i < h[1] && seg_count < OTA_HASH_MAX_SEGS; i++) {
          uint8_t seg[8];
          if (!slotRead(_io, false, off, seg, sizeof(seg))) break;
          const uint32_t len = le32(seg + 4);
          const uint32_t payload_off = off + 8;
          if ((uint64_t)payload_off + len > running_len) break;
          uint8_t* s = &p[6 + seg_count * 8];
          putLe32(s, payload_off);
          putLe32(s + 4, len);
          seg_count++;
          off = payload_off + len;
          off = (off + 3) & ~3u;
          if (off >= running_len) break;
        }
      }
    }
    p[5] = seg_count;             // p[4] stays count == 0 (metadata marker)
    ensureRunningSha();
    memcpy(&p[6 + OTA_HASH_MAX_SEGS * 8], _running_sha, OTA_HASH_LEN);
    out[0] = OTA_MAGIC0;
    out[1] = OTA_MAGIC1;
    out[2] = OTA_OP_BLOCK_HASH_REPORT;
    memcpy(out + 3, p, OTA_HASH_META_BODY);
    return sealFrame(out, 3 + OTA_HASH_META_BODY);
  }

  if (count > OTA_HASH_MAX_RANGES || body_len != (uint16_t)(1 + 8 * count)) return 0;
  for (uint8_t i = 0; i < count; i++) {
    const uint8_t* r = body + 1 + i * 8;
    const uint32_t off = le32(r);
    const uint32_t len = le32(r + 4);
    uint8_t* dst = &p[5 + i * (4 + 1 + OTA_HASH_LEN)];
    putLe32(dst, off);
    uint8_t status = 0;
    if (_io == nullptr || running_len == 0) {
      status = 3;                         // no readable image
    } else if ((off & (OTA_HASH_ALIGN - 1)) != 0 ||
               (len != 0 && ((len & (OTA_HASH_ALIGN - 1)) != 0 ||
                              len > OTA_HASH_MAX_RANGE))) {
      status = 3;                         // alignment / length rejected
    } else {
      uint32_t n = len;
      if (len == 0 && off <= running_len) n = running_len - off;
      if (off > running_len || n > running_len - off) {
        status = 1;                       // out of bounds
      } else if (!hashSlot(_io, false, off, n, dst + 5)) {
        status = 2;                       // flash read failed
      }
    }
    dst[4] = status;
  }
  out[0] = OTA_MAGIC0;
  out[1] = OTA_MAGIC1;
  out[2] = OTA_OP_BLOCK_HASH_REPORT;
  memcpy(out + 3, p, OTA_HASH_RANGE_BODY(count));
  return sealFrame(out, 3 + OTA_HASH_RANGE_BODY(count));
}

size_t OtaUpdate::buildFindReport(const uint8_t* body, uint16_t body_len, uint8_t* out) {
  if (body == nullptr || body_len < 13 || _io == nullptr) return 0;
  const uint32_t len = le32(body);
  const uint32_t start = le32(body + 4);
  const uint32_t end = le32(body + 8);
  const uint8_t count = body[12];
  const uint32_t running_len = ensureRunningLength();
  if (count == 0 || count > OTA_FIND_MAX_BLOCKS) return 0;
  if (body_len < (uint16_t)(13 + 8 * count)) return 0;
  if (len == 0 || len % OTA_HASH_ALIGN != 0 || len > OTA_HASH_MAX_RANGE ||
      start % OTA_HASH_ALIGN != 0 || end % OTA_HASH_ALIGN != 0 ||
      start > end || end > running_len) return 0;

  uint64_t want[OTA_FIND_MAX_BLOCKS];
  uint8_t hit_count[OTA_FIND_MAX_BLOCKS];
  uint32_t hits[OTA_FIND_MAX_BLOCKS][OTA_FIND_HITS_PER];
  for (uint8_t i = 0; i < count; i++) {
    const uint8_t* p = body + 13 + i * 8;
    want[i] = (uint64_t)le32(p) | ((uint64_t)le32(p + 4) << 32);
    hit_count[i] = 0;
    memset(hits[i], 0, sizeof(hits[i]));
  }

  // Same polynomial as the app's delta matcher. Slide once across the window
  // and compare every fingerprint at each offset; this keeps a 12-block query
  // to one flash scan.
  static const uint64_t kPrime = 0x00000100000001B3ull;
  uint8_t* buf = (uint8_t*)malloc(len);
  if (buf == nullptr) return 0;
  if (!slotRead(_io, false, start, buf, len)) { free(buf); return 0; }
  uint64_t h = 0;
  for (uint32_t i = 0; i < len; i++) h = h * kPrime + buf[i];
  uint64_t pow = 1;
  for (uint32_t i = 1; i < len; i++) pow *= kPrime;   // P^(len-1), mod 2^64

  uint8_t remaining = count;
  for (uint32_t off = start; off + len <= end && remaining > 0;
       off += OTA_HASH_ALIGN) {
    for (uint8_t i = 0; i < count; i++) {
      if (h != want[i] || hit_count[i] >= OTA_FIND_HITS_PER) continue;
      hits[i][hit_count[i]++] = off;
      if (hit_count[i] == OTA_FIND_HITS_PER) remaining--;
    }
    if (remaining == 0 || off + OTA_HASH_ALIGN + len > end) break;
    uint8_t in[OTA_HASH_ALIGN];
    if (!slotRead(_io, false, off + len, in, sizeof(in))) break;
    for (uint8_t k = 0; k < OTA_HASH_ALIGN; k++) {
      h = (h - (uint64_t)buf[k] * pow) * kPrime + in[k];
    }
    memmove(buf, buf + OTA_HASH_ALIGN, len - OTA_HASH_ALIGN);
    memcpy(buf + len - OTA_HASH_ALIGN, in, OTA_HASH_ALIGN);
  }
  free(buf);

  uint8_t p[1 + OTA_FIND_MAX_BLOCKS * (1 + OTA_FIND_HITS_PER * 4)];
  memset(p, 0, sizeof(p));
  size_t used = 0;
  p[used++] = count;
  for (uint8_t i = 0; i < count; i++) {
    p[used++] = hit_count[i];
    for (uint8_t j = 0; j < hit_count[i]; j++) {
      putLe32(p + used, hits[i][j]);
      used += 4;
    }
  }
  out[0] = OTA_MAGIC0;
  out[1] = OTA_MAGIC1;
  out[2] = OTA_OP_BLOCK_FIND_REPORT;
  memcpy(out + 3, p, used);
  return sealFrame(out, 3 + used);
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

  // Read-only diff oracle: answered in every state, like OP_INFO, and never
  // touches the session or flash writes. Body is count + ranges (or just the
  // count byte for the metadata query).
  if (op == OTA_OP_BLOCK_HASH) {
    if (report_out == nullptr || report_cap < OTA_HASH_REPORT_MAX) return 0;
    return buildBlockHashReport(frame + 3, (uint16_t)(n - 5), report_out);
  }
  if (op == OTA_OP_BLOCK_FIND) {
    if (report_out == nullptr || report_cap < OTA_FIND_REPORT_MAX) return 0;
    return buildFindReport(frame + 3, (uint16_t)(n - 5), report_out);
  }

  if (op == OTA_OP_NOP || op == OTA_OP_REPORT || op == OTA_OP_INFO_REPORT) return 0;

  if (op == OTA_OP_ASK) {
    if (!stagingArmed()) return 0;
    if (n != 5) return 0;   // OP_ASK carries no body
    if (report_out == nullptr || report_cap < 7) return 0;
    sendReport(report_out);
    return 7;
  }

  // An op frame carries a seq word; anything shorter cannot be one and is
  // dropped rather than read past its end.
  if (n < OTA_FRAME_OVERHEAD) return 0;
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
  // Only a live staging session takes ops: a verified (Ready) image is never
  // written to again, so a stray op or a retransmitted OP_END is a no-op
  // rather than something that could scrap a finished transfer.
  if (_state != State::Staging) return 0;
  if (seq != _expect) return 0;   // strictly in-order delivery; everything else is a gap or a dup

  applyOp(body, body_len, op);
  if (_state != State::Error) _expect++;   // Ready (OP_END) and Staging both advance
  return 0;
}

void OtaUpdate::handleStart(const uint8_t* body, uint16_t len) {
  // A busy session keeps its place: a stray OP_START never disturbs a
  // transfer that is in progress or already verified.
  if (_state == State::Staging || _state == State::Ready) return;
  // From here the session is Idle or Error; a refusal records why and leaves
  // the state alone (the operator sees `err=` until the next successful arm).
  if (len != OTA_OP_START_MIN) { _err = OTA_ERR_BOUNDS; return; }
  uint32_t size = le32(body);
  const uint8_t* sha = body + 4;
  uint32_t target_id = le32(body + 36);
  const uint8_t* token = body + 40;
  if (target_id != ownTargetId()) { _err = OTA_ERR_TARGET; return; }

  // The armed contract decides: single-use, time-boxed, bound to {size, sha256}
  if (!_pending.valid) { _err = OTA_ERR_TOKEN; return; }
  if ((uint32_t)(millis() - _pending.expires_at) < 0x80000000u) {   // expired
    clearPending();
    _err = OTA_ERR_TOKEN;
    return;
  }
  if (_pending.size != size || memcmp(_pending.sha, sha, 32) != 0 ||
      memcmp(_pending.token, token, 4) != 0) {
    _err = OTA_ERR_TOKEN;   // stranger: leave the pending arm intact
    return;
  }
  clearPending();           // consumed by this arm
  resetSession();           // a failed (Error) session is replaced; Idle is a no-op
  armSession(size, sha);
}

void OtaUpdate::applyOp(const uint8_t* body, uint16_t body_len, uint8_t op) {
  switch (op) {
    case OTA_OP_COPY:
      if (body_len != 6) { abortStaging(OTA_ERR_BOUNDS); return; }
      applyCopy(le32(body), le16(body + 4));
      return;
    case OTA_OP_LITERAL: {
      if (_inflater != nullptr) { abortStaging(OTA_ERR_COMPRESS); return; }
      if (body_len < 2) { abortStaging(OTA_ERR_BOUNDS); return; }
      uint16_t dlen = le16(body);
      if (body_len != (uint16_t)(2 + dlen)) { abortStaging(OTA_ERR_BOUNDS); return; }
      applyLiteral(body + 2, dlen);
      return;
    }
    case OTA_OP_LITERAL_Z:
      applyLiteralZ(body, body_len);
      return;
    case OTA_OP_END:
      if (_inflater != nullptr) { abortStaging(OTA_ERR_COMPRESS); return; }
      if (body_len != 0) { abortStaging(OTA_ERR_BOUNDS); return; }
      if (_written != _size) { abortStaging(OTA_ERR_BOUNDS); return; }
      {
        uint8_t sha[32];
        if (!hashSlot(_io, /*staged=*/true, 0, _size, sha) || memcmp(sha, _expected_sha, 32) != 0) {
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

void OtaUpdate::applyCopy(uint32_t base_off, uint16_t len) {
  if ((uint64_t)base_off + len > _io->sourceSize() || (uint64_t)_written + len > _size) {
    abortStaging(OTA_ERR_BOUNDS);
    return;
  }
  uint8_t buf[256];
  uint32_t off = 0;
  while (off < len) {
    size_t n = (size_t)(len - off) < sizeof(buf) ? (size_t)(len - off) : sizeof(buf);
    if (!_io->readSource(base_off + off, buf, n)) { abortStaging(OTA_ERR_IO); return; }
    if (!_io->append(buf, n)) { abortStaging(OTA_ERR_IO); return; }
    off += (uint32_t)n;
  }
  _written += len;
}

void OtaUpdate::applyLiteral(const uint8_t* data, uint16_t len) {
  if ((uint64_t)_written + len > _size) { abortStaging(OTA_ERR_BOUNDS); return; }
  if (!_io->append(data, len)) { abortStaging(OTA_ERR_IO); return; }
  _written += len;
}

void OtaUpdate::closeInflater() {
  zFree(static_cast<ZInflater*>(_inflater));
  _inflater = nullptr;
  _zin_comp_total = 0;
  _zin_orig_total = 0;
  _zin_comp_left = 0;
}

bool OtaUpdate::zEmit(void* ctx, const uint8_t* data, size_t len) {
  auto* self = static_cast<OtaUpdate*>(ctx);
  if ((uint64_t)self->_written + len > self->_size) return false;
  if (!self->_io->append(data, len)) return false;
  self->_written += len;
  return true;
}

void OtaUpdate::applyLiteralZ(const uint8_t* body, uint16_t body_len) {
  if (body_len < 8) { abortStaging(OTA_ERR_BOUNDS); return; }
  const uint32_t orig_len = le32(body);
  const uint16_t comp_total = le16(body + 4);
  const uint8_t flags = body[6];
  const uint16_t dlen = body_len - 7;
  if (dlen == 0 || dlen > OTA_LZ_MAX_DATA) {
    abortStaging(OTA_ERR_BOUNDS);
    return;
  }
  if ((flags & OTA_LZ_START) != 0 && (uint64_t)_written + orig_len > _size) {
    abortStaging(OTA_ERR_BOUNDS);
    return;
  }

  if (flags & OTA_LZ_START) {
    closeInflater();
    _inflater = zInit();
    if (_inflater == nullptr) { abortStaging(OTA_ERR_COMPRESS); return; }
    _zin_comp_total = comp_total;
    _zin_orig_total = orig_len;
    _zin_comp_left = comp_total;
    _zin_stream_start = _written;
  } else if (_inflater == nullptr || comp_total != _zin_comp_total ||
             orig_len != _zin_orig_total) {
    abortStaging(OTA_ERR_COMPRESS);
    return;
  }
  if (dlen > _zin_comp_left) { abortStaging(OTA_ERR_BOUNDS); return; }

  auto* z = static_cast<ZInflater*>(_inflater);
  bool done = z->done;
  if (!zFeed(z, body + 7, dlen, z->scratch, 32768,
             (flags & OTA_LZ_END) == 0 && dlen < _zin_comp_left,
             &done, zEmit, this)) {
    abortStaging(OTA_ERR_COMPRESS);
    return;
  }
  _zin_comp_left -= dlen;
  if ((flags & OTA_LZ_END) != 0) {
    if (!done || _zin_comp_left != 0 ||
        _written - _zin_stream_start != _zin_orig_total) {
      abortStaging(OTA_ERR_COMPRESS);
      return;
    }
    closeInflater();
  }
}

void OtaUpdate::sendReport(uint8_t* out) {
  out[0] = OTA_MAGIC0;
  out[1] = OTA_MAGIC1;
  out[2] = OTA_OP_REPORT;
  putLe16(out + 3, _expect);
  sealFrame(out, 5);
}

bool OtaUpdate::sendHealthProbe() {
  uint8_t p[OTA_HEALTH_LEN] = {OTA_MAGIC0, OTA_MAGIC1, OTA_OP_NOP};
  putLe16(p + 3, _health_nonce);
  sealFrame(p, OTA_HEALTH_LEN - 2);
  return _send != nullptr && _send(_send_ctx, p, sizeof(p));
}

// ------------------------------------------------------------------ /ota_pend

#define OTA_PEND_MAGIC   "OTAP"
// magic(4) attempts(1) nonce(2) crc16(2)
#define OTA_PEND_SIZE    9

bool OtaUpdate::writePend() {
  if (_fs == nullptr || _rng == nullptr) return false;   // no nonce, no marker
  uint8_t nb[2];
  _rng->random(nb, 2);
  uint16_t nonce = (uint16_t)nb[0] | ((uint16_t)nb[1] << 8);
  if (!writePendRecord(0 /* attempts */, nonce)) return false;
  // Read the marker back before the reboot: a write that silently landed
  // nothing is the one failure that would strand a fresh image with no
  // verdict owed.
  uint16_t back;
  return readPend(&back, nullptr) && back == nonce;
}

// Write the whole marker record (magic, attempts, nonce, CRC). A short write
// removes it: a torn marker is not a verdict.
bool OtaUpdate::writePendRecord(uint8_t attempts, uint16_t nonce) {
  uint8_t rec[OTA_PEND_SIZE];
  memcpy(rec, OTA_PEND_MAGIC, 4);
  rec[4] = attempts;
  rec[5] = (uint8_t)(nonce & 0xFF);
  rec[6] = (uint8_t)(nonce >> 8);
  uint16_t crc = crc16_ccitt(rec, OTA_PEND_SIZE - 2);
  rec[7] = (uint8_t)(crc & 0xFF);
  rec[8] = (uint8_t)(crc >> 8);

  File f = fsOpenWrite(_fs, OTA_PEND_FILE);
  if (!f) return false;
  size_t n = f.write(rec, sizeof(rec));
  f.flush();
  f.close();
  if (n != sizeof(rec)) {
    _fs->remove(OTA_PEND_FILE);
    return false;
  }
  return true;
}

bool OtaUpdate::readPend(uint16_t* nonce, uint8_t* attempts) {
  if (_fs == nullptr || !_fs->exists(OTA_PEND_FILE)) return false;
  File f = fsOpenRead(_fs, OTA_PEND_FILE);
  if (!f) return false;
  uint8_t rec[OTA_PEND_SIZE];
  const size_t n = f.read(rec, sizeof(rec));
  f.close();
  if (n != OTA_PEND_SIZE) return false;
  if (memcmp(rec, OTA_PEND_MAGIC, 4) != 0) return false;
  const uint16_t stored = (uint16_t)rec[7] | ((uint16_t)rec[8] << 8);
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
  if (!writePendRecord(attempts, nonce)) otaLog("OTA boot verdict: marker rewrite failed");
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
  adoptPendVerdict();
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

void OtaUpdate::ensureRunningSha() {
  if (_running_sha_valid) return;
  _running_sha_valid = true;
  memset(_running_sha, 0, sizeof(_running_sha));
  if (_io != nullptr) hashRunningImage(_io, _running_sha, &_running_len);   // zeros: unknown
}

void OtaUpdate::formatStatus(char* reply, size_t reply_size) {
  if (_io == nullptr) {
    snprintf(reply, reply_size, "running=%s sha=0000000000000000 state=unsupported size=0 off=0 err=0",
             _version);
    return;
  }
  ensureRunningSha();
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
  if ((uint32_t)size > _io->slotSize()) {
    // Refuse here, where the operator can read why: an arm the slot can never
    // hold would otherwise fail later as a silent OP_START refusal.
    snprintf(reply, CLI_REPLY_MAX, "Err - size exceeds the %lu-byte slot",
             (unsigned long)_io->slotSize());
    return false;
  }
  uint8_t sha[32];
  if (sha_tok == nullptr || strlen(sha_tok) != 64 || !mesh::Utils::fromHex(sha, 32, sha_tok)) {
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
  memcpy(_pending.sha, sha, sizeof(sha));
  _rng->random(_pending.token, sizeof(_pending.token));
  _pending.expires_at = millis() + OTA_PENDING_TTL_MS;
  _pending.valid = true;
  // Arming a new image is the start of a new attempt: drop a failed session so
  // the operator (and the app) never has to `ota abort` an old error away. A
  // live transfer is left untouched.
  if (_state == State::Error) resetSession();
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
    deletePend();   // nothing booted: the marker would only owe a phantom verdict
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
  if (strcmp(cmd, "apply") == 0) {
    if (cliNoExtra(p, reply, OTA_USAGE)) ota.applyCommand(reply);
    return;
  }
  if (strcmp(cmd, "abort") == 0) {
    if (cliNoExtra(p, reply, OTA_USAGE)) ota.abortCommand(reply);
    return;
  }
  if (strcmp(cmd, "rollback") == 0) {
    if (cliNoExtra(p, reply, OTA_USAGE)) ota.rollbackCommand(reply);
    return;
  }
  snprintf(reply, CLI_REPLY_MAX, "%s", OTA_USAGE);
}
