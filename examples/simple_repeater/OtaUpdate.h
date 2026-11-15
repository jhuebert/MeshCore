// OtaUpdate.h — LoRa OTA (over-the-air firmware update) for the ESP32
// simple_repeater firmware, from the lora-ota plan
// (plans/2026-11-15-lora-ota-fork-firmware.md). Personal fork feature; all
// fork-owned code lives in this file pair plus minimal hook lines in
// MyMesh.{h,cpp} and the `ota` CLI branch in handleCommand(). No changes to
// MeshCore core sources.
//
// The repeater is dumb; the phone app is smart. The app picks the target,
// computes the delta (or full image), paces the transfer and interprets
// `ota status`; this node only reassembles an op stream into its *inactive*
// app slot, verifies the SHA-256 of what actually landed in flash, and —
// only then — offers to flip the boot partition. Nothing here floods a
// single byte: every OTA packet is ROUTE_TYPE_DIRECT carrying
// PAYLOAD_TYPE_RAW_CUSTOM, zero-hop or along an explicit --via path, and
// DIRECT packets cannot flood.
//
// Wire format (frozen in M0):
//
//   ch0   'O' 0x4F
//   ch1   'T' 0x54
//   ch2   op
//   ch3   seq (u16, little endian) — OP_START is 0; each later packet +1
//   ch4.. opcode-specific body
//   last.. last-1
//   crc16 (u16, LE) — CRC-16/CCITT-FALSE over everything before it
//
//   OP_START  0x01  size:u32 sha256[32] target:u32 fw:u32 ret_path_len:u8 [ret_path]
//             size        total byte length of the image to stage
//             sha256      expected digest of the staged slot at OP_END
//             target      sha256(pio_env)[0..3] LE; mismatch = wrong node, reject
//             fw          MAJOR<<24 | MINOR<<16 | PATCH<<8 | pre (opaque here)
//             ret_path_len / ret_path   return itinerary in the repeater→app
//             travel order, encoded exactly like a packet's path_len byte
//             (count in the low 6 bits, hash-size-1 in the high 2). Empty
//             (0) = the app is zero-hop and OP_REPORTs go back zero-hop.
//             DIRECT paths are consumed hop by hop as they travel, so the
//             final hop can never learn the reverse itinerary — without it
//             OP_REPORT cannot cross a multi-hop path. The app knows the
//             full route both ways and hands the return leg over; the
//             repeater uses it verbatim for every reply in the session.
//
//   OP_COPY   0x02  base_off:u32 len:u16
//             copy running-slot bytes [base_off, base_off+len) into the
//             inactive slot at the current write offset
//   OP_LITERAL 0x03 len:u16 data[len]
//             write data into the inactive slot at the current write offset
//   OP_END    0x04                finish; SHA-256 the staged slot; if OK, ready
//   OP_ASK    0x05                request an OP_REPORT (after a window)
//   OP_REPORT 0x06  next_expected:u16 gaps_mask:u16   (repeater → app only)
//             gaps_mask bit i = op at (next_expected + i) never arrived
//   OP_NOP    0x00                health probe; never answered
//
// Ops are strictly ordered: an op is applied only when its seq equals
// next_expected; anything else is buffered (up to a 16-op window) or, if it
// falls outside the window, dropped. Unchanged bytes are never re-transmitted
// — a delta is OP_COPY + OP_LITERAL against the running slot; a full image is
// OP_LITERALs against nothing. Verify-then-apply is the only path to a
// reboot into new firmware: on any mismatch the staging is scrapped and the
// previous firmware keeps running.
//
// /ota_pend is a tiny record (magic "OTAP", version, state, size, sha256,
// CRC-16) written before an apply reboot and consumed by the boot health
// check: a freshly-applied image re-verifies the SHA-256 of the slot it is
// running from, then sends one zero-hop health packet as proof the radio
// path works. Success clears the record; failure flips the boot partition
// back to the previous slot and reboots. The record is kept in state
// ROLLED after a rollback so the old image can surface rolledback=1 in
// `ota status` until `ota abort` acknowledges it.
//
// CLI: "ota ..." commands, handled by otaCLI() (see OtaUpdate.cpp). `ota
// status` is the app's progress feed; the line format is frozen in M0:
//
//   running=<ver> sha=<16hex> state=<idle|staging|ready|error> size=<n> off=<n> err=<code> rolledback=<0|1>
//
// - sha is the running slot's SHA-256, truncated to its first 8 bytes
// - size/off are the staged contract's size and the write offset (0 when idle)
// - err is one of the OTA_ERR_* codes (0 = OK); state=error sticks until an
//   explicit `ota abort` or a new `ota stage`/OP_START
// - rolledback=1 appears on the previous firmware after an automatic
//   rollback; `ota abort` acknowledges it. state=unsupported on builds with
//   no second app slot (and on non-ESP32), where every other key still prints.

#ifndef _OTA_UPDATE_H
#define _OTA_UPDATE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <Mesh.h>                    // mesh::Packet, mesh::MainBoard
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

// every OTA frame: magic(2) + op(1) + seq(2) + body + crc16(2)
#define OTA_FRAME_OVERHEAD (2 + 1 + 2 + 2)
#define OTA_WINDOW         16        // ops buffered in flight, relative to expect
#define OTA_OP_START_MIN   (4 + 32 + 4 + 4 + 1)        // without ret_path
#define OTA_HEALTH_LEN     7        // NOP(3) seq(2) crc(2)

// error codes reported as err= in `ota status` (0 = OK)
#define OTA_ERR_NONE       0
#define OTA_ERR_TARGET     1        // wrong target_id for this node
#define OTA_ERR_BATTERY    2        // battery gate suspended: staging refused
#define OTA_ERR_SLOT       3        // size 0, too big, or no second slot
#define OTA_ERR_SHA        4        // OP_END digest mismatch: staging scrapped
#define OTA_ERR_IO         5        // flash read/write/begin/end failed
#define OTA_ERR_BOUNDS     6        // op out of bounds or past end of image
#define OTA_ERR_END        9        // esp_ota_end() failed after a clean verify

// /ota_pend states
#define OTA_PEND_FILE      "/ota_pend"
#define OTA_PEND_PENDING   1        // freshly applied; boot health check pending
#define OTA_PEND_ROLLED    2        // rolled back; visible as rolledback=1 until acked

// true iff the packet is a well-formed OTA frame: RAW_CUSTOM payload, OTA
// magic, valid CRC. Used by the relay path (a fork filter rule must never
// drop an update in transit — and only frames the node would itself accept
// are exempt) and by the local receive path, on every platform.
inline bool otaIsOtaPacket(const mesh::Packet* pkt) {
  if (pkt == nullptr) return false;
  if (pkt->getPayloadType() != PAYLOAD_TYPE_RAW_CUSTOM) return false;
  const size_t n = pkt->payload_len;
  if (n < OTA_FRAME_OVERHEAD) return false;
  if (pkt->payload[0] != OTA_MAGIC0 || pkt->payload[1] != OTA_MAGIC1) return false;
  const uint16_t stored = (uint16_t)pkt->payload[n - 2] | ((uint16_t)pkt->payload[n - 1] << 8);
  return crc16_ccitt(pkt->payload, n - 2) == stored;
}

// The staging slot engine's only view of flash, so the session machine is
// host-testable. The ESP32 implementation (Esp32SlotIo, OtaUpdate.cpp) wraps
// esp_ota_*; host tests inject a RAM-backed fake.
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
  // refused while it is suspended), and the board (apply/rollback reboot via
  // board->reboot()). `firmware_version` is FIRMWARE_VERSION (shown in
  // `ota status`). The sender dispatches replies: zero-hop, or DIRECT along
  // the return path from the armed OP_START.
  void begin(FILESYSTEM* fs, BatteryGate* gate, mesh::MainBoard* board, const char* firmware_version);
  void setSender(OtaSendFn fn, void* ctx) { _send = fn; _send_ctx = ctx; }

  // Receive entry: RAW_CUSTOM payloads with the OTA magic are validated and
  // applied here; anything else is inert.
  void onPacket(const mesh::Packet* pkt);

  // Boot health check pump (first loop passes after a freshly-applied boot)
  // plus the rollback deadline. Cheap once settled.
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
  struct WindowSlot { uint16_t len; uint8_t data[MAX_PACKET_PAYLOAD]; };

  static constexpr uint32_t kHealthTimeoutMs = 15000;

  FILESYSTEM* _fs = nullptr;
  BatteryGate* _gate = nullptr;
  mesh::MainBoard* _board = nullptr;
  const char* _version = "";
  OtaSlotIo* _io = nullptr;
  OtaSendFn _send = nullptr;
  void* _send_ctx = nullptr;

  State _state = State::Idle;
  uint8_t _err = OTA_ERR_NONE;
  WindowSlot* _slots = nullptr;             // heap, sized OTA_WINDOW, armed only
  uint16_t _expect = 1;                     // next seq to apply (OP_START = 0)
  uint16_t _arrived = 0;                    // window bitmap, relative to _expect
  uint32_t _size = 0, _written = 0;
  uint8_t _expected_sha[32] = {0};
  uint8_t _ret_path_len = 0, _ret_path[MAX_PATH_SIZE] = {0};

  bool _running_sha_valid = false;
  uint8_t _running_sha[32] = {0};

  bool _health_pending = false;             // freshly-applied image booted
  bool _health_sent = false;                // health probe already dispatched
  bool _health_checked = false;             // boot pump settled
  uint32_t _health_deadline = 0;
  uint32_t _health_size = 0, _health_nonce = 0;
  uint8_t _health_sha[32] = {0};
  bool _rolledback = false;                 // ROLLED record seen at boot

  bool stagingArmed() const { return _state == State::Staging || _state == State::Ready; }
  void resetSession();                      // back to Idle, window buffer freed
  void abortStaging(uint8_t err);           // scrap the staged slot on failure
  bool armSession(uint32_t size, const uint8_t sha[32], const uint8_t* ret_path, uint8_t ret_path_len);
  void handleStart(const uint8_t* body, uint16_t body_len);
  void applyOp(const uint8_t* body, uint16_t body_len, uint8_t op);
  bool applyCopy(uint32_t base_off, uint16_t len);
  bool applyLiteral(const uint8_t* data, uint16_t len);
  void drainWindow();
  uint32_t ownTargetId() const;
  void sendReport(uint16_t ask_seq);
  bool sendHealthProbe();
  void rollbackToPrevious(const char* why);
  bool writePendRecord(uint8_t state, uint32_t size, const uint8_t sha[32]);
  bool readPend(uint8_t* state, uint32_t* size, uint8_t sha[32]);
  void deletePend();
};

// CLI verb handler: everything after "ota" (see MyMesh::handleCommand).
void otaCLI(OtaUpdate& ota, const char* sub, char* reply);

#endif // _OTA_UPDATE_H
