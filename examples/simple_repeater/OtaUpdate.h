// OtaUpdate.h — LoRa OTA (over-the-air firmware update) for the ESP32
// simple_repeater firmware (plans/2026-11-15-lora-ota-fork-firmware.md).
// Personal fork feature; all fork-owned code lives in this file pair plus
// minimal hook lines in MyMesh.{h,cpp} and the `ota` CLI branch in
// handleCommand().
//
// The repeater is dumb; the phone app is smart. The app picks the target,
// computes the delta (or full image), paces the transfer, and interprets
// `ota status`; this node reassembles an op stream into its *inactive* app
// slot, verifies the SHA-256 of what actually landed in flash, and only
// then accepts an apply. The node accepts nothing from a stranger:
//
//   1. Arming requires one operator action (serial or any authenticated
//      remote admin on this node): `ota stage <size> <sha256-hex> <ver>`.
//      It registers a single pending arm {size, sha256, token} in RAM,
//      valid for 120 s, and replies with the token.
//   2. OP_START is accepted only while a pending arm exists, unexpired,
//      whose token AND {size, sha256} both match — a fully accepted arm
//      consumes it. The armed contract binds the token to one exact image,
//      so even an overheard token arms nothing but the genuine release.
//   3. `ota apply` boots the verified image. A freshly applied image runs a
//      boot health check (radio init + one zero-hop packet TX) and flips
//      back automatically on failure.
//
// The CLI exchange carrying the token is encrypted end-to-end (same TXT_MSG
// path as every admin command); OP_START itself is cleartext by nature, and
// that is safe by (2) — see the plan's threat model. DIRECT packets only,
// never flooded. SHA-256 gate on the staged slot is the only integrity
// decision; the op stream itself is trustless once armed.
//
// Wire format (frozen in M0):
//
//   ch0   'O' 0x4F
//   ch1   'T' 0x54
//   ch2   op
//   ch3   seq (u16, little endian) — op packets only: OP_START/OP_COPY/
//             OP_LITERAL/OP_END. OP_ASK, OP_REPORT and OP_NOP carry no seq.
//   ch4.. opcode-specific body
//   last.. last-1
//   crc16 (u16, LE) — CRC-16/CCITT-FALSE over everything before it
//
//   OP_START  0x01  size:u32 sha256[32] target:u32 token:u32
//   OP_COPY   0x02  base_off:u32 len:u16          copy running-slot bytes into the staged image
//   OP_LITERAL 0x03 len:u16 data[len]             write data into the staged image
//   OP_END    0x04                              finish; SHA-256 the slot; if OK, ready
//   OP_ASK    0x05  ret_path_len:u8 [ret_path]    request an OP_REPORT (after a window)
//   OP_REPORT 0x06  next_expected:u16           (repeater → app)
//   OP_NOP    0x00                              boot health probe; never answered
//
// Ops are strictly in order: an op applies only when seq == next_expected;
// anything else is dropped. DIRECT routing delivers in order per path, so
// this costs nothing on the wire and no reassembly buffer is needed. The
// app's recovery from a lost op is "re-send from next_expected".
//
// ret_path_len / ret_path in OP_ASK carry the return itinerary for the
// OP_REPORT being requested, in the repeater→app travel order (encoded
// exactly like a packet's path_len byte). DIRECT paths are consumed hop by
// hop as they travel, so the final hop cannot learn the reverse itinerary —
// without it OP_REPORTs cannot cross a multi-hop path. The path travels
// with each ask and is used once; nothing is stored. Empty (0) = zero hop.
//
// /ota_pend is an 8-byte marker (magic "OTAP", a random nonce, CRC-16): its mere presence means an apply just booted and the boot health
// check owes the board a verdict. Success deletes the marker; failure flips
// the boot partition back to the previous slot and reboots. The marker is
// deleted on rollback too, so the old image never re-triggers a flip.
//
// CLI: "ota ..." commands, handled by otaCLI() (see OtaUpdate.cpp). `ota
// status` is the app's progress feed; the line format is frozen in M0:
//
//   running=<ver> sha=<16hex> state=<idle|staging|ready|error> size=<n> off=<n> err=<code>
//
// - sha is the running slot's SHA-256, truncated to its first 8 bytes
// - size/off are the staged contract's size and the write offset
// - err is one of the OTA_ERR_* codes (0 = OK); state=error sticks until
//   `ota abort` or a new OP_START
// - state=unsupported on builds with no second app slot and on non-ESP32

#ifndef _OTA_UPDATE_H
#define _OTA_UPDATE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <Mesh.h>                    // mesh::Packet, mesh::MainBoard, mesh::RNG
#include <helpers/IdentityStore.h>   // FILESYSTEM typedef
#include <Packet.h>                  // PAYLOAD_TYPE_RAW_CUSTOM
#include "PersistUtil.h"             // crc16_ccitt (the wire CRC)

// --- wire format ------------------------------------------------------------
#define OTA_OP_NOP     0x00
#define OTA_OP_START   0x01
#define OTA_OP_COPY    0x02
#define OTA_OP_LITERAL 0x03
#define OTA_OP_END     0x04
#define OTA_OP_ASK     0x05
#define OTA_OP_REPORT  0x06

#define OTA_MAGIC0     0x4F          // 'O'
#define OTA_MAGIC1     0x54          // 'T'

// OTA frames carrying an op: magic(2) + op(1) + seq(2) + body + crc16(2).
// ASK/REPORT/NOP carry no seq.
#define OTA_FRAME_OVERHEAD (2 + 1 + 2 + 2)
#define OTA_OP_START_MIN   (4 + 32 + 4 + 4)   // size + sha256 + target + token
#define OTA_HEALTH_LEN     7         // NOP(3) nonce(2) crc(2)
#define OTA_PENDING_TTL_MS 120000    // a pending arm lives two minutes
#define OTA_PEND_FILE      "/ota_pend"

// error codes reported as err= in `ota status` (0 = OK)
#define OTA_ERR_NONE       0
#define OTA_ERR_TARGET     1        // wrong target_id for this node
#define OTA_ERR_BATTERY    2        // battery gate suspended: staging refused
#define OTA_ERR_SLOT       3        // size 0, too big, or no second slot
#define OTA_ERR_SHA        4        // OP_END digest mismatch: staging scrapped
#define OTA_ERR_IO         5        // flash read/write/begin/end failed
#define OTA_ERR_BOUNDS     6        // op out of bounds or past end of image
#define OTA_ERR_TOKEN      7        // OP_START with no/incompatible pending arm
#define OTA_ERR_END        9        // esp_ota_end() failed after a clean verify

// true iff the packet is a well-formed OTA frame: RAW_CUSTOM payload, OTA
// magic, valid CRC. Used by the relay path (a fork filter rule must never
// drop an update in transit — and only frames the node would itself accept
// are exempt) and by the local receive path, on every platform.
inline bool otaIsOtaPacket(const mesh::Packet* pkt) {
  if (pkt == nullptr) return false;
  if (pkt->getPayloadType() != PAYLOAD_TYPE_RAW_CUSTOM) return false;
  const size_t n = pkt->payload_len;
  if (n < 5) return false;   // magic(2) + op(1) + crc(2); ops carry a seq, ASK/REPORT/NOP do not
  if (pkt->payload[0] != OTA_MAGIC0 || pkt->payload[1] != OTA_MAGIC1) return false;
  const uint16_t stored = (uint16_t)pkt->payload[n - 2] | ((uint16_t)pkt->payload[n - 1] << 8);
  return crc16_ccitt(pkt->payload, n - 2) == stored;
}

// The staging slot engine's only view of flash, so the session machine is
// host-testable. The ESP32 implementation wraps esp_ota_*; host tests inject
// a RAM-backed fake.
class OtaSlotIo {
public:
  virtual ~OtaSlotIo() {}
  virtual uint32_t slotSize() const = 0;            // inactive slot size, bytes
  virtual uint32_t sourceSize() const = 0;          // running slot size, bytes
  virtual bool beginWrite(uint32_t image_size) = 0; // esp_ota_begin()
  virtual bool append(const uint8_t* data, size_t len) = 0;   // esp_ota_write()
  virtual bool readSource(uint32_t off, uint8_t* out, size_t len) = 0;
  virtual bool readStaged(uint32_t off, uint8_t* out, size_t len) = 0;
  virtual bool endWrite() = 0;                      // esp_ota_end()
  virtual void abortWrite() = 0;                    // esp_ota_abort()
  // Point the bootloader at the inactive slot (esp_ota_set_boot_partition()).
  // The engine calls this for apply (flip forward) and rollback (flip back).
  virtual bool setInactiveAsBoot() = 0;
};

typedef bool (*OtaSendFn)(void* ctx, const uint8_t* payload, size_t len,
                          const uint8_t* ret_path, uint8_t ret_path_len);

class BatteryGate;

class OtaUpdate {
public:
  enum class State : uint8_t { Idle, Staging, Ready, Error };

  OtaUpdate();
  ~OtaUpdate();

  // Wire the engine into the node: filesystem, the battery gate (staging is
  // refused while it is suspended), the board (apply/rollback reboot via
  // board->reboot()), and the RNG (arm tokens). `firmware_version` is
  // FIRMWARE_VERSION (shown in `ota status`). The sender dispatches
  // replies: zero-hop, or DIRECT along the return path from the armed
  // OP_START.
  void begin(FILESYSTEM* fs, BatteryGate* gate, mesh::MainBoard* board, const char* firmware_version, mesh::RNG* rng);
  void setSender(OtaSendFn fn, void* ctx) { _send = fn; _send_ctx = ctx; }

  // Receive entry: RAW_CUSTOM payloads with the OTA magic are validated and
  // applied here; anything else is inert.
  void onPacket(const mesh::Packet* pkt);

  // Boot health check (only engages right after an apply reboot) plus the
  // rollback deadline. Cheap once settled.
  void loop();

  // Feed TX completion for the health probe (from logTx / logTxFail).
  void onTxResult(const mesh::Packet* pkt, bool ok);

  // CLI handler for everything after the "ota" verb (see otaCLI()).
  void formatStatus(char* reply, size_t reply_size);
  bool stageCommand(const char* size_tok, const char* sha_tok, const char* ver_tok, char* reply);
  bool applyCommand(char* reply);
  bool abortCommand(char* reply);
  bool rollbackCommand(char* reply);

  State state() const { return _state; }

  // Test-only: inject a RAM-backed slot instead of the ESP32 partitions.
  void setSlotIo(OtaSlotIo* io) { _io = io; }

private:
  // The one operator-authorized pre-condition for an OP_START: created only
  // by `ota stage`, single slot (re-arming overwrites), single use, expires
  // quickly. A token without the matching {size, sha256} arms nothing.
  struct PendingArm {
    bool valid = false;
    uint32_t size = 0;
    uint8_t sha[32] = {0};
    uint8_t token[4] = {0};
    uint32_t expires_at = 0;   // millis() deadline; wrap-safe compare
  };

  static constexpr uint32_t kHealthTimeoutMs = 15000;

  FILESYSTEM* _fs = nullptr;
  BatteryGate* _gate = nullptr;
  mesh::MainBoard* _board = nullptr;
  const char* _version = "";
  mesh::RNG* _rng = nullptr;
  OtaSlotIo* _io = nullptr;
  OtaSendFn _send = nullptr;
  void* _send_ctx = nullptr;

  State _state = State::Idle;
  uint8_t _err = OTA_ERR_NONE;
  PendingArm _pending;
  uint16_t _expect = 1;                     // next seq to apply (OP_START = 0)
  uint32_t _size = 0, _written = 0;
  uint8_t _expected_sha[32] = {0};

  bool _running_sha_valid = false;
  uint8_t _running_sha[32] = {0};

  bool _health_pending = false;             // freshly-applied image booted
  bool _health_sent = false;                // health probe already dispatched
  bool _health_checked = false;             // boot pump settled
  uint32_t _health_deadline = 0;
  uint16_t _health_nonce = 0;               // sha-free marker nonce

  bool stagingArmed() const { return _state == State::Staging || _state == State::Ready; }
  void resetSession();                      // back to Idle
  void clearPending() { _pending.valid = false; }
  void abortStaging(uint8_t err);           // scrap the staged slot on failure
  bool armSession(uint32_t size, const uint8_t sha[32]);
  void handleStart(const uint8_t* body, uint16_t body_len);
  void applyOp(const uint8_t* body, uint16_t body_len, uint8_t op);
  bool applyCopy(uint32_t base_off, uint16_t len);
  bool applyLiteral(const uint8_t* data, uint16_t len);
  uint32_t ownTargetId() const;
  void sendReport(const uint8_t* ret_path, uint8_t ret_path_len);
  bool sendHealthProbe();
  void rollbackToPrevious(const char* why);
  bool writePend();
  bool readPend(uint16_t* nonce);
  void deletePend();
};

// CLI verb handler: everything after "ota" (see MyMesh::handleCommand).
void otaCLI(OtaUpdate& ota, const char* sub, char* reply);

#endif // _OTA_UPDATE_H
