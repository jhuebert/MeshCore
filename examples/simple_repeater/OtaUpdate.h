// OtaUpdate.h — LoRa OTA (over-the-air firmware update) for the ESP32
// simple_repeater firmware. Personal fork feature; all fork-owned code lives
// in this file pair plus minimal hook lines in MyMesh.{h,cpp} and the `ota`
// CLI branch in handleCommand().
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
//      valid for 120 s, and replies with the token. The version token is the
//      operator's declaration of what is being staged: it is validated and
//      kept out of the trust decision (the SHA-256 already pins the exact
//      bytes), so it never has to be parsed on the node.
//   2. OP_START is accepted only while a pending arm exists, unexpired,
//      whose token AND {size, sha256} both match — a fully accepted arm
//      consumes it. The armed contract binds the token to one exact image,
//      so even an overheard token arms nothing but the genuine release.
//      OP_START is also how a failed session is replaced: arming again (or
//      `ota stage`) resets an Error session, so a retry never needs `ota
//      abort` first. A Staging/Ready session is never disturbed.
//   3. `ota apply` boots the verified image. A freshly applied image runs a
//      boot health check (radio init + one zero-hop packet TX) and flips
//      back automatically on failure.
//
// The CLI exchange carrying the token and every op ride in encrypted
// admin REQ/RESPONSE datagrams (same trust path as every admin command);
// relays forward ciphertext. SHA-256 gate on the staged slot is the only
// integrity decision; the op stream itself is trustless once armed.
//
// Wire format: carried inside an admin client's REQ, answered by a RESPONSE,
// so intermediate nodes only ever forward ordinary mesh traffic.
//
//   REQ plaintext:    [tag(4)][REQ_TYPE_OTA (1)][frame_len(1)][OTA frame]
//   RESPONSE plaintext (only for OP_ASK / OP_INFO):  [tag(4)][report frame]
// The plaintext is padded by the crypto layer to a 16-byte block, so the
// OTA frame inside carries its own length — the padding after it is ignored.
//
//   OTA frame:
//     ch0   'O' 0x4F
//     ch1   'T' 0x54
//     ch2   op
//     ch3   seq (u16, little endian) — op packets only: OP_START/OP_COPY/
//               OP_LITERAL/OP_END. OP_ASK, OP_INFO, OP_REPORT,
//               OP_INFO_REPORT and OP_NOP carry no seq.
//     ch4.. opcode-specific body
//     last.. last-1
//     crc16 (u16, LE) — CRC-16/CCITT-FALSE over everything before it
//
//   OP_NOP        0x00                              boot health probe; never answered
//   OP_START      0x01  size:u32 sha256[32] target:u32 token:u32
//   OP_COPY       0x02  base_off:u32 len:u16        copy running-slot bytes into the staged image
//   OP_LITERAL    0x03  len:u16 data[len]           write data into the staged image
//   OP_END        0x04                              finish; SHA-256 the slot; if OK, ready
//   OP_ASK        0x05                              request an OP_REPORT (after a window)
//   OP_REPORT     0x06  next_expected:u16           (repeater → app, inside RESPONSE)
//   OP_INFO       0x07                              query node capabilities (any state)
//   OP_INFO_REPORT 0x08 wire_ver:u8 target:u32 slot:u32 max_literal:u16 features:u32
//                      ver[32] sha256[32]           (repeater → app, inside RESPONSE)
//   OP_BLOCK_HASH 0x09 count:u8 (offset:u32 len:u32)[count]
//                                                  hash running-image ranges (any state)
//   OP_BLOCK_HASH_REPORT 0x0A running_size:u32 ...  (repeater → app, inside RESPONSE)
//                          count=0: seg_count:u8 (off:u32 len:u32)[segs] sha256[32]
//                          count>0: count:u8 (off:u32 status:u8 sha256[32])[count]
//   OP_BLOCK_FIND 0x0B len:u32 start:u32 end:u32 count:u8 rolling64[count]:u64
//                                                  find up to 12 blocks in one
//                                                  window of the running image
//                                                  (4-byte aligned offsets).
//     rolling64 is the same polynomial hash the app's delta matcher uses:
//       h = 0; for each byte: h = h*0x100000001B3 + byte   (mod 2^64)
//     The node slides it once over the window, comparing all fingerprints.
//   OP_BLOCK_FIND_REPORT 0x0C (hit_count:u8 offset:u32[hit_count])[count]
//                              (at most OTA_FIND_HITS_PER offsets per block)
//   OP_LITERAL_Z   0x0D orig_len:u32 comp_len:u16 flags:u8 data[comp_len]
//                              raw DEFLATE stream (no gzip wrapper), split
//                              across ops; flags bit0 starts a stream, bit1
//                              ends it, bits 2..4 select the codec (0 = raw
//                              DEFLATE). Decompressed bytes are appended to the
//                              staged image like OP_LITERAL.
//   OP_STAGED_HASH 0x0E count:u8 (offset:u32 len:u32)[count]
//                              read-only hash of the staged slot's written
//                              bytes (any state). Same request/report layout
//                              as OP_BLOCK_HASH; count=0 hashes the staged
//                              prefix and reports its length in the size field.
//   OP_STAGED_HASH_REPORT 0x0F   same layout as OP_BLOCK_HASH_REPORT
//   OP_STATUS      0x10          structured session/status/capability query
//   OP_STATUS_REPORT 0x11        identity block + session id + progress +
//                              codec mask + a length-prefixed TLV extension
//                              block. Answered in every state, like OP_INFO.
//   OP_PING        0x12 payload[1..16]  echo the payload back over the same
//                              REQ/RESPONSE path (any state, read-only)
//   OP_PING_REPORT 0x13 payload[...]
//
// Stability contract (this is what lets the format live for years):
//   - Unknown ops and unknown OP_INFO bodies are dropped silently.
//   - A stray unknown op *inside a staged stream* aborts staging — the node
//     will never apply a half-understood update.
//   - OP_INFO is always answered, in every state (idle/staging/ready/error,
//     and on builds with no second app slot, with slot_size = 0).
//   - OP_INFO_REPORT is the negotiation handshake: it is how an app detects
//     the wire version, the board's target id (before arming), the usable
//     slot size, the largest accepted OP_LITERAL, and the feature mask
//     (bit0 = OP_COPY delta, bit1 = apply/rollback), plus the RUNNING
//     image's firmware version (32 bytes, NUL-padded) and its SHA-256 —
//     taken over exactly the bytes the update wrote (image length from the
//     ESP image header, appended digest included), so it equals the hash of
//     the app's own .bin and is what the app uses to pick a delta base by
//     exact-image match. All zeros means "cannot tell".
//     New features are only ever added as new bits or new report fields
//     appended at the end. OP_BLOCK_HASH is answered in every state and never
//     writes flash or changes the session; it is a read-only diff oracle. The
//     app still SHA-256s the *new* image at OP_END, so a wrong diff can waste
//     airtime but can never boot a wrong image.
//   - OP_INFO_REPORT is frozen byte-for-byte: an app that expects exactly 79
//     body bytes must keep working. Everything added after this firmware ships
//     goes into OP_STATUS_REPORT instead, whose trailing TLV block is the
//     forward-compatibility point ([tag:u8][len:u8][value...], ext_len=0 today).
//     OP_STATUS_REPORT repeats the identity block at the same offsets, so an
//     app can share one decoder between the two reports.
//   - The session id in OP_STATUS_REPORT is minted at OP_START and zeroed on
//     reset. An app that survives a phone restart can tell from it whether the
//     repeater also restarted: same id = the staged bytes are still there.
//   - New ops get new op codes; existing ops never change meaning, field
//     order or size. The CRC algorithm, the magic bytes and the SEQ rules
//     are frozen. A new op is always optional: nodes that do not implement it
//     drop it silently, and apps must fall back when a feature bit is clear.
//   - The `frame_len` byte in the REQ says the exact OTA frame length that
//     follows; the node requires frame_len <= payload_len - 2 and ignores
//     any trailing padding.
// Any hard break of these rules is a NEW protocol (new REQ code), never a
// silent redefinition of these ones.
//
// The tag the companion prepends to every request keeps each REQ's packet hash
// unique, so a re-sent op is never swallowed by the duplicate filter.
// The inner CRC stays on the OP_* frames: it lets the engine reject a
// mangled frame before touching flash, without trusting the carrier.
//
// Ops are strictly in order: an op applies only when seq == next_expected;
// anything else is dropped. DIRECT routing delivers in order per path, so
// this costs nothing on the wire and no reassembly buffer is needed. The
// app's recovery from a lost op is "re-send from next_expected".
//
// The report frame comes back through the stock REQ reply routing (the client's
// stored out_path, or a flood when none is known), exactly like a status
// response — the ask/info carries no return itinerary and nothing is stored.
//
// /ota_pend is a 9-byte marker (magic "OTAP", boots-so-far, a random nonce,
// CRC-16): its mere presence means an apply just booted and the boot health
// check owes the board a verdict. Success deletes the marker; failure flips
// the boot partition back to the previous slot and reboots. The marker is
// deleted on rollback too, so the old image never re-triggers a flip.
//
// CLI: "ota ..." commands, handled by otaCLI() (see OtaUpdate.cpp). `ota
// status` is the app's progress feed; the line format is frozen:
//
//   running=<ver> sha=<16hex> state=<idle|staging|ready|error> size=<n> off=<n> err=<code>
//
// - sha is the running image's SHA-256 (same value OP_INFO_REPORT carries),
//   truncated to its first 8 bytes
// - size/off are the staged contract's size and the write offset
// - err is one of the OTA_ERR_* codes (0 = OK); state=error sticks until
//   `ota abort`, `ota stage` or a new OP_START
// - state=unsupported on builds with no second app slot and on non-ESP32

#ifndef _OTA_UPDATE_H
#define _OTA_UPDATE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include <Mesh.h>                    // mesh::Packet, mesh::MainBoard, mesh::RNG
#include <helpers/IdentityStore.h>   // FILESYSTEM typedef

// --- wire format ------------------------------------------------------------
#define OTA_OP_NOP        0x00
#define OTA_OP_START      0x01
#define OTA_OP_COPY       0x02
#define OTA_OP_LITERAL    0x03
#define OTA_OP_END        0x04
#define OTA_OP_ASK        0x05
#define OTA_OP_REPORT     0x06
#define OTA_OP_INFO       0x07
#define OTA_OP_INFO_REPORT 0x08
#define OTA_OP_BLOCK_HASH 0x09
#define OTA_OP_BLOCK_HASH_REPORT 0x0A
#define OTA_OP_BLOCK_FIND 0x0B
#define OTA_OP_BLOCK_FIND_REPORT 0x0C
#define OTA_OP_LITERAL_Z 0x0D
#define OTA_OP_STAGED_HASH 0x0E
#define OTA_OP_STAGED_HASH_REPORT 0x0F
#define OTA_OP_STATUS     0x10
#define OTA_OP_STATUS_REPORT 0x11
#define OTA_OP_PING       0x12
#define OTA_OP_PING_REPORT 0x13
#define OTA_LZ_MAX_DATA  120           // ≤ the companion's 176-byte frame
#define OTA_LZ_START     0x01
#define OTA_LZ_END       0x02
// Bits 2..4 of OP_LITERAL_Z.flags carry a codec id; 0 is raw DEFLATE, which is
// the only codec this firmware implements. Other ids are refused at staging.
#define OTA_LZ_CODEC_SHIFT 2
#define OTA_LZ_CODEC_MASK  0x1C

#define OTA_MAGIC0     0x4F          // 'O'
#define OTA_MAGIC1     0x54          // 'T'

// The frame-format version OP_INFO_REPORT reports. New reports keep all
// previously-defined fields in the same positions.
#define OTA_WIRE_VERSION   1

// feature bits reported in OP_INFO_REPORT.features and OP_STATUS_REPORT
// (rest reserved, must be 0 so an old app ignores what it does not know)
#define OTA_FEAT_DELTA     0x00000001   // OP_COPY is implemented
#define OTA_FEAT_APPLY     0x00000002   // apply/rollback is implemented (dual app slot)
#define OTA_FEAT_BLOCK_HASH 0x00000004  // OP_BLOCK_HASH is implemented
#define OTA_FEAT_STAGED_HASH 0x00000008  // OP_STAGED_HASH is implemented
#define OTA_FEAT_STATUS    0x00000010   // OP_STATUS_REPORT is implemented
#define OTA_FEAT_PING      0x00000020   // OP_PING is implemented
#define OTA_FEAT_ONLINE_FETCH 0x00000040 // local staging from a release download

// Online OTA is observer-only firmware. The local staging API itself is
// Arduino-free and host-tested; the advertised bit follows the real build.
#if defined(ESP32) && defined(WITH_MQTT_OBSERVER)
#define OTA_HAS_ONLINE_FETCH 1
#endif

// One reply string for the whole CLI, shared by the verbs that exist in this
// build. Non-observer builds must not advertise the online verbs.
#ifdef OTA_HAS_ONLINE_FETCH
#define OTA_USAGE "Usage: ota [status|net|fetch <ver|sha|start>|stage <size> <sha256> <ver>|apply|abort|rollback]"
#else
#define OTA_USAGE "Usage: ota [status|stage <size> <sha256> <ver>|apply|abort|rollback]"
#endif

// OP_STATUS_REPORT.state values (same order as OtaUpdate::State).
#define OTA_STATE_IDLE        0
#define OTA_STATE_STAGING     1
#define OTA_STATE_READY       2
#define OTA_STATE_ERROR       3
#define OTA_STATE_UNSUPPORTED 4
// Bit in OP_STATUS_REPORT.codecs: raw DEFLATE via OP_LITERAL_Z.
#define OTA_CODEC_DEFLATE  0x01

// OP_INFO_REPORT body size (before the OTA frame envelope: magic + op + crc)
#define OTA_INFO_BODY      79           // wire_ver(1) + target(4) + slot(4) + max_literal(2) + features(4) + ver[32] + running sha256[32]
#define OTA_INFO_RUNNING_VER_OFF   15   // body offset of the 32-byte running version string
#define OTA_INFO_RUNNING_SHA_OFF   47   // body offset of the 32-byte running SHA-256
#define OTA_INFO_REPORT_LEN (3 + OTA_INFO_BODY + 2)
// The report body is written field by field, so the fixed offsets above are
// what keeps the frozen layout and the writer in step.
static_assert(OTA_INFO_RUNNING_VER_OFF == 1 + 4 + 4 + 2 + 4, "OP_INFO_REPORT field order");
static_assert(OTA_INFO_RUNNING_SHA_OFF == OTA_INFO_RUNNING_VER_OFF + 32, "OP_INFO_REPORT field order");
static_assert(OTA_INFO_BODY == OTA_INFO_RUNNING_SHA_OFF + 32, "OP_INFO_REPORT field order");
// OP_STATUS_REPORT: the frozen identity block above, then session/progress
// fields and a reserved TLV block. Written as fixed offsets so a new field is
// an append, never an insertion into the identity block.
#define OTA_STATUS_SESSION_OFF   (OTA_INFO_BODY)
#define OTA_STATUS_STATE_OFF     (OTA_STATUS_SESSION_OFF + 4)
#define OTA_STATUS_ERR_OFF       (OTA_STATUS_STATE_OFF + 1)
#define OTA_STATUS_SIZE_OFF      (OTA_STATUS_ERR_OFF + 1)
#define OTA_STATUS_WRITTEN_OFF   (OTA_STATUS_SIZE_OFF + 4)
#define OTA_STATUS_EXPECT_OFF    (OTA_STATUS_WRITTEN_OFF + 4)
#define OTA_STATUS_CODECS_OFF    (OTA_STATUS_EXPECT_OFF + 2)
#define OTA_STATUS_EXT_LEN_OFF   (OTA_STATUS_CODECS_OFF + 1)
#define OTA_STATUS_BODY_FIXED    (OTA_STATUS_EXT_LEN_OFF + 2)
#define OTA_STATUS_MAX_EXT       32
#define OTA_STATUS_REPORT_LEN    (3 + OTA_STATUS_BODY_FIXED + 2)
#define OTA_STATUS_REPORT_MAX    (3 + OTA_STATUS_BODY_FIXED + OTA_STATUS_MAX_EXT + 2)
static_assert(OTA_STATUS_BODY_FIXED == 98, "OP_STATUS_REPORT field order");
// OP_PING echoes at most this many bytes; its report is the payload + envelope.
#define OTA_PING_MAX  16
#define OTA_PING_REPORT_MAX (3 + OTA_PING_MAX + 2)
// OP_BLOCK_HASH: one request may carry up to OTA_HASH_MAX_RANGES ranges; the
// report echoes up to that many hashes. count=0 is the metadata query: it
// returns the running image's size, its ESP segment table and its whole-image
// hash so the app can align segments and detect "already up to date" in one
// round trip. Every range report also carries running_size.
#define OTA_HASH_LEN          32
#define OTA_HASH_MAX_RANGES   4
#define OTA_HASH_MAX_SEGS     6
#define OTA_HASH_MAX_RANGE    32768   // 32 x 1 KB flash reads per range
#define OTA_HASH_ALIGN        4
#define OTA_FIND_MAX_BLOCKS   12
#define OTA_FIND_HITS_PER     2
#define OTA_FIND_REPORT_MAX   (3 + 1 + OTA_FIND_MAX_BLOCKS * (1 + OTA_FIND_HITS_PER * 4) + 2)
#define OTA_HASH_META_BODY    (4 + 1 + 1 + OTA_HASH_MAX_SEGS * 8 + OTA_HASH_LEN)
#define OTA_HASH_RANGE_BODY(n) (4 + 1 + (n) * (4 + 1 + OTA_HASH_LEN))
#define OTA_HASH_META_LEN     (3 + OTA_HASH_META_BODY + 2)
#define OTA_HASH_RANGE_LEN(n) (3 + OTA_HASH_RANGE_BODY(n) + 2)
#define OTA_HASH_REPORT_MAX   (OTA_HASH_META_LEN > OTA_HASH_RANGE_LEN(OTA_HASH_MAX_RANGES) \
                               ? OTA_HASH_META_LEN : OTA_HASH_RANGE_LEN(OTA_HASH_MAX_RANGES))
// largest report frame the engine ever returns
#define OTA_REPORT_BASE ((OTA_INFO_REPORT_LEN > OTA_HASH_REPORT_MAX) ? OTA_INFO_REPORT_LEN : OTA_HASH_REPORT_MAX)
#define OTA_REPORT_MAX ((OTA_REPORT_BASE > OTA_STATUS_REPORT_MAX) ? OTA_REPORT_BASE : OTA_STATUS_REPORT_MAX)

// OTA frames carrying an op: magic(2) + op(1) + seq(2) + body + crc16(2).
// ASK/REPORT/INFO*/NOP carry no seq.
#define OTA_FRAME_OVERHEAD (2 + 1 + 2 + 2)
// Two more bytes ride in front of the frame in the REQ (`REQ_TYPE_OTA` and
// the frame-length byte), so this is the largest OP_LITERAL that can reach
// the engine inside one MAX_PACKET_PAYLOAD datagram — the value OP_INFO_REPORT
// advertises as `max_literal`.
#define OTA_MAX_LITERAL    (MAX_PACKET_PAYLOAD - 2 - OTA_FRAME_OVERHEAD - 2)
#define OTA_OP_START_MIN   (4 + 32 + 4 + 4)   // size + sha256 + target + token
#define OTA_HEALTH_LEN     7         // NOP(3) nonce(2) crc(2)
#define OTA_HEALTH_MAX_ATTEMPTS 3    // boots of a new image before it is rejected
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
#define OTA_ERR_END        8        // esp_ota_end() failed after a clean verify
#define OTA_ERR_COMPRESS   9        // compressed literal stream failed


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

// Zero-hop transport for the boot health probe (PAYLOAD_TYPE_RAW_CUSTOM,
// OP_NOP). The transfer itself never uses it — ops travel inside REQ/RESPONSE.
typedef bool (*OtaSendFn)(void* ctx, const uint8_t* payload, size_t len);

class BatteryGate;

class OtaUpdate {
public:
  enum class State : uint8_t { Idle, Staging, Ready, Error };

  OtaUpdate();
  ~OtaUpdate();

  // Wire the engine into the node: filesystem, the battery gate (staging is
  // refused while it is suspended), the board (apply/rollback reboot via
  // board->reboot()), and the RNG (arm tokens). `firmware_version` is
  // FIRMWARE_VERSION (shown in `ota status`). The sender dispatches the
  // boot health probe: zero-hop only.
  void begin(FILESYSTEM* fs, BatteryGate* gate, mesh::MainBoard* board, const char* firmware_version, mesh::RNG* rng);
  void setSender(OtaSendFn fn, void* ctx) { _send = fn; _send_ctx = ctx; }

  // Receive entry: feed one OTA frame (as carried inside an admin client's
  // REQ, payload after REQ_TYPE_OTA). Applies the op; when the frame was an
  // OP_ASK, writes the OP_REPORT frame into `report_out` and returns its
  // length (7). Anything else — unknown magic/CRC, unsolicited ops, ASK while
  // idle — returns 0 and is inert. The caller (handleRequest) puts the report
  // into the RESPONSE the stock reply router delivers.
  size_t onFrame(const uint8_t* frame, size_t len, uint8_t* report_out, size_t report_cap);

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

  // Local staging for the online download. It feeds the same session and the
  // same SHA-256 gate as a LoRa transfer, so apply and rollback are shared.
  bool beginLocalStage(uint32_t size, const uint8_t sha[32], const char* ver, char* reply);
  bool appendLocalStage(const uint8_t* data, size_t len);
  bool finishLocalStage(char* reply);
  void cancelLocalStage();
  void noteOnlineError(uint8_t err, char* reply);
  bool localStageActive() const { return _local_source; }
  bool batterySuspended() const;
  uint8_t errorCode() const { return _err; }
  uint32_t slotSize() const { return _io != nullptr ? _io->slotSize() : 0; }

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
  // Random per-arm id reported by OP_STATUS: lets an app tell a phone restart
  // (same session still staged) from a repeater restart (staged bytes orphaned).
  uint32_t _session_id = 0;
  uint32_t _size = 0, _written = 0;
  uint8_t _expected_sha[32] = {0};

  // Opaque streaming inflater for OP_LITERAL_Z (defined in OtaUpdate.cpp, so
  // the header stays free of zlib/miniz includes).
  void* _inflater = nullptr;
  uint32_t _zin_comp_total = 0;         // full stream size, echoed by each op
  uint32_t _zin_orig_total = 0;
  uint32_t _zin_stream_start = 0;       // staged offset when the stream began
  uint32_t _zin_comp_left = 0;          // compressed bytes still expected
  uint8_t _running_sha[32] = {0};
  bool _running_sha_valid = false;
  uint32_t _running_len = 0;                // image length (header + digest)
  bool _running_len_valid = false;

  bool _local_source = false;               // this session came from an online download

  bool _health_pending = false;             // freshly-applied image booted
  bool _health_sent = false;                // health probe already dispatched
  bool _health_checked = false;             // boot pump settled
  uint32_t _health_deadline = 0;
  uint16_t _health_nonce = 0;               // sha-free marker nonce

  bool stagingArmed() const { return _state == State::Staging || _state == State::Ready; }
  // Compute _running_sha once per boot, on first use. Zeros when the platform
  // cannot read the running image back.
  void ensureRunningSha();
  // runningImageLength(), cached for the read-only hash oracle.
  uint32_t ensureRunningLength();
  // Build the OP_INFO_REPORT frame (magic..CRC) into `out`. Works in every
  // state — it is the negotiation handshake an app performs before arming.
  size_t buildInfoReport(uint8_t* out);
  // The frozen 79-byte identity block shared by INFO_REPORT and STATUS_REPORT.
  void writeIdentity(uint8_t* p, uint32_t feature_bits);
  // Feature bits advertised by both reports (zero on platforms with no slot).
  uint32_t features() const;
  // Structured status/capability report. Also answered in every state.
  size_t buildStatusReport(uint8_t* out);
  // Build a running-image or staged-slot hash report with one shared encoder;
  // the two ops differ only in which slot they read and which op they answer.
  // Read-only: never touches the session state or flash writes.
  size_t buildHashReport(bool staged, uint8_t report_op,
                         const uint8_t* body, uint16_t body_len, uint8_t* out);
  size_t buildBlockHashReport(const uint8_t* body, uint16_t body_len, uint8_t* out);
  size_t buildStagedHashReport(const uint8_t* body, uint16_t body_len, uint8_t* out);
  // Locate 4-byte-aligned blocks of the running image by their 64-bit rolling
  // fingerprints. This recovers matches after an edit shifts every byte.
  size_t buildFindReport(const uint8_t* body, uint16_t body_len, uint8_t* out);
public:
  /// Boot-time verdict before the radio or mesh start: count this boot of a
  /// freshly-applied image and roll back once the attempts run out. Covers the
  /// images the probe cannot: ones that hang or crash before `loop()`.
  void earlyBootCheck(FILESYSTEM* fs, mesh::MainBoard* board);

  /// A freshly-applied image whose radio will not start is not healthy: roll
  /// back now instead of halting. Returns true when a rollback was triggered.
  bool radioInitFailed();

private:
  void resetSession();                      // back to Idle
  void clearPending() { _pending.valid = false; }
  void abortStaging(uint8_t err);           // scrap the staged slot on failure
  void armSession(uint32_t size, const uint8_t sha[32]);
  bool checkStageRequest(uint32_t size, const uint8_t sha[32], const char* ver,
                         char* reply, uint8_t* err_out = nullptr);
  void handleStart(const uint8_t* body, uint16_t body_len);
  void applyOp(const uint8_t* body, uint16_t body_len, uint8_t op);
  void applyCopy(uint32_t base_off, uint16_t len);
  void applyLiteral(const uint8_t* data, uint16_t len);
  void applyLiteralZ(const uint8_t* body, uint16_t body_len);
  void closeInflater();                 // free the streaming decompressor
  static bool zEmit(void* ctx, const uint8_t* data, size_t len);
  uint32_t ownTargetId() const;
  void sendReport(uint8_t* out);
  bool sendHealthProbe();
  void rollbackToPrevious(const char* why);
  bool writePend();
  bool writePendRecord(uint8_t attempts, uint16_t nonce);
  bool readPend(uint16_t* nonce, uint8_t* attempts);
  bool adoptPendVerdict();
  uint8_t bumpPendAttempts();
  void deletePend();
};

// CLI verb handler: everything after "ota" (see MyMesh::handleCommand).
void otaCLI(OtaUpdate& ota, const char* sub, char* reply);

#endif // _OTA_UPDATE_H
