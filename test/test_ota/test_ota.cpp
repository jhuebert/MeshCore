// test_ota.cpp — native unit tests for the LoRa OTA session engine
// (examples/simple_repeater/OtaUpdate*). Built by [env:native_ota] in
// platformio.ini; see that env for the shim and flag notes. Flash sits behind
// OtaSlotIo, so the whole state machine runs here against a RAM-backed fake;
// /ota_pend uses the in-memory NativeFS shim; the ESP32-only partition glue
// (Esp32SlotIo) is exercised on-device, not here.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <Arduino.h>      // g_mock_millis (NativeShim.h is force-included first)
#include <SHA256.h>       // test mock (deterministic, non-cryptographic)
#include <Utils.h>        // mesh::Utils::sha256 for expected digests (mock)
#include "OtaUpdate.h"
#include "BatteryGate.h"

// --------------------------------------------------------------- fakes

struct MockBoard : public mesh::MainBoard {
  uint16_t mV = 4200;
  int reboots = 0;
  uint16_t getBattMilliVolts() override { return mV; }
  const char* getManufacturerName() const override { return "mock-board"; }
  void reboot() override { reboots++; }
  uint8_t getStartupReason() const override { return 0; }
};

struct FakeSlotIo : OtaSlotIo {
  uint32_t slot_size = 0x200000;
  std::vector<uint8_t> source;   // running-slot contents
  std::vector<uint8_t> staged;   // image bytes written so far
  uint32_t image_size = 0;
  bool begin_ok = true, write_ok = true, end_ok = true, read_ok = true;
  bool aborted = false, set_boot_called = false;

  FakeSlotIo() : source(0x200000, 0) {}

  uint32_t slotSize() const override { return slot_size; }
  uint32_t sourceSize() const override { return (uint32_t)source.size(); }
  bool beginWrite(uint32_t sz) override {
    if (!begin_ok) return false;
    aborted = false;
    staged.clear();
    image_size = sz;
    return true;
  }
  bool append(const uint8_t* d, size_t n) override {
    if (!write_ok) return false;
    if ((uint64_t)staged.size() + n > image_size) return false;
    staged.insert(staged.end(), d, d + n);
    return true;
  }
  bool readSource(uint32_t off, uint8_t* out, size_t n) override {
    if (!read_ok) return false;
    if ((uint64_t)off + n > source.size()) return false;
    memcpy(out, source.data() + off, n);
    return true;
  }
  bool readStaged(uint32_t off, uint8_t* out, size_t n) override {
    if (!read_ok) return false;
    if ((uint64_t)off + n > staged.size()) return false;
    memcpy(out, staged.data() + off, n);
    return true;
  }
  bool endWrite() override {
    if (!end_ok) return false;
    return true;
  }
  void abortWrite() override {
    aborted = true;
  }
  bool setInactiveAsBoot() override {
    set_boot_called = true;
    return true;
  }
};

struct SentFrame {
  std::vector<uint8_t> payload;
  uint8_t ret_path_len = 0;
  std::vector<uint8_t> ret_path;
};

static bool captureSend(void* ctx, const uint8_t* payload, size_t len,
                        const uint8_t* ret_path, uint8_t ret_path_len) {
  auto* vec = static_cast<std::vector<SentFrame>*>(ctx);
  SentFrame f;
  f.payload.assign(payload, payload + len);
  f.ret_path_len = ret_path_len;
  if (ret_path != nullptr && ret_path_len != 0) {
    f.ret_path.assign(ret_path, ret_path + (ret_path_len & 63) * ((ret_path_len >> 6) + 1));
  }
  vec->push_back(std::move(f));
  return true;
}

static mesh::Packet makePacket(const std::vector<uint8_t>& payload) {
  mesh::Packet pkt;
  pkt.header = (PAYLOAD_TYPE_RAW_CUSTOM << PH_TYPE_SHIFT) | ROUTE_TYPE_DIRECT;
  pkt.path_len = 0;
  pkt.payload_len = (uint16_t)payload.size();
  memcpy(pkt.payload, payload.data(), payload.size());
  return pkt;
}

static std::vector<uint8_t> frame(uint8_t op, uint16_t seq, const std::vector<uint8_t>& body) {
  std::vector<uint8_t> p = {OTA_MAGIC0, OTA_MAGIC1, op, (uint8_t)(seq & 0xFF), (uint8_t)(seq >> 8)};
  p.insert(p.end(), body.begin(), body.end());
  uint16_t crc = crc16_ccitt(p.data(), p.size());
  p.push_back((uint8_t)(crc & 0xFF));
  p.push_back((uint8_t)(crc >> 8));
  return p;
}

static void mockSha256(const std::vector<uint8_t>& data, uint8_t out[32]) {
  SHA256 sha;
  sha.update(data.data(), data.size());
  sha.finalize(out, 32);
}

static uint32_t le32From(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// sha256("native_test")[0..3] LE — the value OTA_PIO_ENV builds compute
static uint32_t testTargetId() {
  const char* env = "native_test";
  uint8_t hash[4];
  mesh::Utils::sha256(hash, 4, (const uint8_t*)env, (int)strlen(env));
  return le32From(hash);
}

static std::vector<uint8_t> makeStartBody(uint32_t size, const uint8_t sha[32],
                                          uint32_t target, uint32_t fw, uint8_t ret_path_len = 0,
                                          const uint8_t* ret_path = nullptr) {
  std::vector<uint8_t> b(4 + 32 + 4 + 4 + 1);
  for (int i = 0; i < 4; i++) b[i] = (uint8_t)(size >> (8 * i));
  memcpy(b.data() + 4, sha, 32);
  for (int i = 0; i < 4; i++) {
    b[36 + i] = (uint8_t)(target >> (8 * i));
    b[40 + i] = (uint8_t)(fw >> (8 * i));
  }
  b[44] = ret_path_len;
  if (ret_path_len != 0 && ret_path != nullptr) {
    uint16_t path_bytes = (uint16_t)((ret_path_len & 63) * ((ret_path_len >> 6) + 1));
    b.insert(b.end(), ret_path, ret_path + path_bytes);
  }
  return b;
}

class OtaTest : public ::testing::Test {
protected:
  NativeFS fs;
  MockBoard board;
  BatteryGate gate;
  FakeSlotIo slot;
  OtaUpdate ota;
  std::vector<SentFrame> sent;

  void SetUp() override {
    g_mock_millis = 0;
    gate.begin(&fs);
    ota.setSlotIo(&slot);
    ota.setSender(captureSend, &sent);
    ota.begin(&fs, &gate, &board, "test-fw-1.0.0");
  }

  // small deterministic image fixture
  static std::vector<uint8_t> testImage(size_t n = 520) {
    std::vector<uint8_t> img(n);
    for (size_t i = 0; i < n; i++) img[i] = (uint8_t)((i * 37 + 11) & 0xFF);
    return img;
  }

  // arm the engine to stage `image`, filling sent[] with the OP_START echo.
  // Returns the image; expected sha is the mock sha of it.
  void arm(const std::vector<uint8_t>& image, uint32_t target = 0, uint32_t fw = 0x01020304) {
    if (target == 0) target = testTargetId();
    uint8_t sha[32];
    mockSha256(image, sha);
    auto body = makeStartBody((uint32_t)image.size(), sha, target, fw);
    auto pkt = makePacket(frame(OTA_OP_START, 0, body));
    ota.onPacket(&pkt);
  }

  void sendLiteral(uint16_t seq, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> body(2 + data.size());
    body[0] = (uint8_t)(data.size() & 0xFF);
    body[1] = (uint8_t)(data.size() >> 8);
    memcpy(body.data() + 2, data.data(), data.size());
    auto pkt = makePacket(frame(OTA_OP_LITERAL, seq, body));
    ota.onPacket(&pkt);
  }

  void sendCopy(uint16_t seq, uint32_t base_off, uint16_t len) {
    std::vector<uint8_t> body(6);
    for (int i = 0; i < 4; i++) body[i] = (uint8_t)(base_off >> (8 * i));
    body[4] = (uint8_t)(len & 0xFF);
    body[5] = (uint8_t)(len >> 8);
    auto pkt = makePacket(frame(OTA_OP_COPY, seq, body));
    ota.onPacket(&pkt);
  }

  void sendEnd(uint16_t seq) {
    auto pkt = makePacket(frame(OTA_OP_END, seq, {}));
    ota.onPacket(&pkt);
  }

  void sendAsk(uint16_t seq) {
    auto pkt = makePacket(frame(OTA_OP_ASK, seq, {}));
    ota.onPacket(&pkt);
  }

  std::string statusLine() {
    char reply[192];
    ota.formatStatus(reply, sizeof(reply));
    return std::string(reply);
  }

  // force the battery gate suspended: two low readings with a debounce gap
  void suspendBattery() {
    gate.setThresholds(3400, 3600);
    board.mV = 3000;
    gate.loop(&fs, board);   // first sample (immediate)
    g_mock_millis += 60000;  // outside the sampling interval
    gate.loop(&fs, board);   // second consecutive low reading -> suspend
  }
};

// ============================================================
// FRAME VALIDATION
// ============================================================

TEST_F(OtaTest, RejectsBadCrc) {
  auto image = testImage();
  uint8_t sha[32] = {0};
  auto pkt = makePacket(frame(OTA_OP_START, 0, makeStartBody((uint32_t)image.size(), sha, 0, 0)));
  pkt.payload[10] ^= 0x01;   // corrupt a body byte
  ota.onPacket(&pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
}

TEST_F(OtaTest, RejectsBadMagic) {
  uint8_t sha[32] = {0};
  auto body = makeStartBody(4, sha, 0, 0);
  auto p = frame(OTA_OP_START, 0, body);
  p[0] = 'X';
  auto pkt = makePacket(p);
  ota.onPacket(&pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
}

TEST_F(OtaTest, RejectsTruncatedFrame) {
  std::vector<uint8_t> p = {OTA_MAGIC0, OTA_MAGIC1, OTA_OP_START};
  auto pkt = makePacket(p);
  EXPECT_NO_FATAL_FAILURE(ota.onPacket(&pkt));
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
}

TEST_F(OtaTest, IdleNodeIgnoresOps) {
  sendLiteral(1, {1, 2, 3, 4});
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
}

// ============================================================
// OP_START — arming the contract
// ============================================================

TEST_F(OtaTest, ArmsOnValidStart) {
  auto image = testImage();
  arm(image);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Staging);
  EXPECT_NE(statusLine().find("state=staging"), std::string::npos);
}

TEST_F(OtaTest, RejectsWrongTarget) {
  auto image = testImage();
  uint8_t sha[32];
  mockSha256(image, sha);
  auto body = makeStartBody((uint32_t)image.size(), sha, 0xDEADBEEF, 0x01020304);
  auto pkt = makePacket(frame(OTA_OP_START, 0, body));
  ota.onPacket(&pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
  EXPECT_NE(statusLine().find("err=1"), std::string::npos);
}

TEST_F(OtaTest, RefusesToArmWhileBatterySuspended) {
  suspendBattery();
  auto image = testImage();
  arm(image);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
  EXPECT_NE(statusLine().find("err=2"), std::string::npos);
}

TEST_F(OtaTest, RejectsZeroSize) {
  uint8_t sha[32] = {0};
  auto body = makeStartBody(0, sha, testTargetId(), 0x01020304);
  auto pkt = makePacket(frame(OTA_OP_START, 0, body));
  ota.onPacket(&pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
  EXPECT_NE(statusLine().find("err=3"), std::string::npos);
}

TEST_F(OtaTest, RejectsOversizeImage) {
  uint8_t sha[32] = {0};
  auto body = makeStartBody(0x300000, sha, testTargetId(), 0x01020304);   // 3 MB > 2 slot
  auto pkt = makePacket(frame(OTA_OP_START, 0, body));
  ota.onPacket(&pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
  EXPECT_NE(statusLine().find("err=3"), std::string::npos);
}

TEST_F(OtaTest, RejectsSecondStartWhileArmed) {
  auto image = testImage();
  arm(image);
  arm(image, testTargetId(), 0x0A0B0C0D);   // different contract; must be ignored
  EXPECT_EQ(ota.state(), OtaUpdate::State::Staging);
}

TEST_F(OtaTest, AcceptsReturnPathVerbatim) {
  auto image = testImage();
  uint8_t sha[32];
  mockSha256(image, sha);
  const uint8_t ret_path[2] = {0xAA, 0xBB};   // one 2-byte hash, encoded 0x41
  auto body = makeStartBody((uint32_t)image.size(), sha, testTargetId(), 0x01020304, 0x41, ret_path);
  auto pkt = makePacket(frame(OTA_OP_START, 0, body));
  ota.onPacket(&pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Staging);
  sendAsk(9);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ(sent[0].ret_path_len, 0x41);
  ASSERT_EQ(sent[0].ret_path.size(), 2u);
  EXPECT_EQ(sent[0].ret_path[0], 0xAA);
  EXPECT_EQ(sent[0].ret_path[1], 0xBB);
}

TEST_F(OtaTest, RejectsInvalidReturnPathEncoding) {
  auto image = testImage();
  uint8_t sha[32];
  mockSha256(image, sha);
  auto body = makeStartBody((uint32_t)image.size(), sha, testTargetId(), 0x01020304, 0xFF, nullptr);
  auto pkt = makePacket(frame(OTA_OP_START, 0, body));
  ota.onPacket(&pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
  EXPECT_NE(statusLine().find("err=6"), std::string::npos);
}

// ============================================================
// WINDOW / SEQ DISCIPLINE
// ============================================================

TEST_F(OtaTest, AppliesLiteralsInOrder) {
  auto image = testImage(16);
  arm(image);
  std::vector<uint8_t> half1(image.begin(), image.begin() + 8);
  std::vector<uint8_t> half2(image.begin() + 8, image.end());
  sendLiteral(1, half1);
  sendLiteral(2, half2);
  EXPECT_NE(statusLine().find("off=16"), std::string::npos);
}

TEST_F(OtaTest, BuffersOutOfOrderWithinWindow) {
  auto image = testImage(16);
  arm(image);
  std::vector<uint8_t> half1(image.begin(), image.begin() + 8);
  std::vector<uint8_t> half2(image.begin() + 8, image.end());
  sendLiteral(2, half2);   // arrives first: buffered, not applied
  EXPECT_NE(statusLine().find("off=0"), std::string::npos);
  sendLiteral(1, half1);   // gap fills: both apply in order
  EXPECT_NE(statusLine().find("off=16"), std::string::npos);
}

TEST_F(OtaTest, GapReportedThenFilled) {
  auto image = testImage(24);
  arm(image);
  std::vector<uint8_t> part(image.begin(), image.begin() + 8);
  sendLiteral(1, part);
  sendLiteral(3, part);   // seq 2 missing
  sendAsk(0x1234);
  ASSERT_EQ(sent.size(), 1u);
  const auto& r = sent[0].payload;
  EXPECT_EQ(r[2], OTA_OP_REPORT);
  EXPECT_EQ(r[3] | (r[4] << 8), 0x1234);          // report echoes the ask's seq
  EXPECT_EQ(r[5] | (r[6] << 8), 2);               // next_expected
  EXPECT_EQ((r[7] | (r[8] << 8)) & 0x0001, 0x0001);   // bit0 = seq 2 never arrived
  EXPECT_EQ((r[7] | (r[8] << 8)) & 0x0002, 0x0000);   // bit1 = seq 3 arrived (buffered)
  sendLiteral(2, part);
  EXPECT_NE(statusLine().find("off=24"), std::string::npos);
}

TEST_F(OtaTest, DuplicateOpIsIgnored) {
  auto image = testImage(16);
  arm(image);
  std::vector<uint8_t> half1(image.begin(), image.begin() + 8);
  sendLiteral(1, half1);
  sendLiteral(1, half1);   // duplicate
  EXPECT_NE(statusLine().find("off=8"), std::string::npos);
}

TEST_F(OtaTest, OpBeyondWindowIsDropped) {
  auto image = testImage(16);
  arm(image);
  std::vector<uint8_t> half(image.begin(), image.begin() + 8);
  sendLiteral(17, half);   // seq 17 with expect 1: out of window
  EXPECT_NE(statusLine().find("off=0"), std::string::npos);
  sendAsk(1);
  ASSERT_EQ(sent.size(), 1u);
  EXPECT_EQ((sent[0].payload[7] | (sent[0].payload[8] << 8)) & 0x0001, 0x0001);
}

TEST_F(OtaTest, RejectsStartWithNonZeroSeq) {
  auto image = testImage();
  uint8_t sha[32];
  mockSha256(image, sha);
  auto body = makeStartBody((uint32_t)image.size(), sha, testTargetId(), 0x01020304);
  auto pkt = makePacket(frame(OTA_OP_START, 5, body));   // OP_START must be seq 0
  ota.onPacket(&pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
}

TEST_F(OtaTest, BootHealthProbeTimeoutRollsBack) {
  auto image = testImage(16);
  arm(image);
  sendLiteral(1, image);
  sendEnd(2);
  char reply[160];
  ASSERT_TRUE(ota.applyCommand(reply));

  FakeSlotIo new_slot;
  new_slot.source = slot.staged;
  MockBoard board2;
  OtaUpdate fresh;
  fresh.setSlotIo(&new_slot);
  fresh.begin(&fs, &gate, &board2, "test-fw-2.0.0");
  std::vector<SentFrame> fresh_sent;
  fresh.setSender(captureSend, &fresh_sent);
  fresh.loop();                       // sha OK -> probe dispatched (not yet answered)
  ASSERT_EQ(fresh_sent.size(), 1u);
  g_mock_millis += 20000;             // health deadline (15 s) expires with no TX result
  fresh.loop();
  EXPECT_EQ(board2.reboots, 1);
}

TEST_F(OtaTest, EndWhenFlashReadFailsScrapsStaging) {
  auto image = testImage(16);
  arm(image);
  sendLiteral(1, image);
  slot.read_ok = false;   // flash read failure at verify time
  sendEnd(2);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Error);
  EXPECT_NE(statusLine().find("err=4"), std::string::npos);
  EXPECT_TRUE(slot.aborted);
}

TEST_F(OtaTest, CopyWhenSourceReadFailsAborts) {
  auto image = testImage(16);
  arm(image);
  slot.read_ok = false;
  sendCopy(1, 0, 16);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Error);
  EXPECT_NE(statusLine().find("err=5"), std::string::npos);
}

TEST_F(OtaTest, StartWhenSlotBeginFailsRefuses) {
  slot.begin_ok = false;
  auto image = testImage();
  arm(image);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
  EXPECT_NE(statusLine().find("err=5"), std::string::npos);
}

TEST_F(OtaTest, LiteralWhenFlashWriteFailsAborts) {
  auto image = testImage(16);
  arm(image);
  slot.write_ok = false;
  sendLiteral(1, std::vector<uint8_t>(4, 0));
  EXPECT_EQ(ota.state(), OtaUpdate::State::Error);
  EXPECT_NE(statusLine().find("err=5"), std::string::npos);
}

TEST_F(OtaTest, EndWhenEndWriteFailsIsNotReady) {
  auto image = testImage(4);
  arm(image);
  sendLiteral(1, image);
  slot.end_ok = false;   // esp_ota_end() reports failure
  sendEnd(2);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Error);
  EXPECT_NE(statusLine().find("err=9"), std::string::npos);
}

TEST_F(OtaTest, StageCliRefusesWhileBatterySuspended) {
  suspendBattery();
  char reply[160];
  std::string image(8, 'x');
  uint8_t sha[32] = {0};
  char sha_hex[65];
  for (int i = 0; i < 32; i++) snprintf(sha_hex + i * 2, 3, "%02x", sha[i]);
  otaCLI(ota, (std::string("stage 8 ") + sha_hex + " 1.2.3").c_str(), reply);
  EXPECT_NE(std::string(reply).find("battery gate"), std::string::npos);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
}

TEST_F(OtaTest, AskWhenIdleGetsNoReply) {
  sendAsk(1);
  EXPECT_TRUE(sent.empty());
}

// ============================================================
// OP_COPY / OP_LITERAL — staging the image
// ============================================================

TEST_F(OtaTest, CopyOpStagesBytesFromSource) {
  // image = src[0:150] + marker(4) + src[300:306]
  std::vector<uint8_t> src(0x2000);
  for (size_t i = 0; i < src.size(); i++) src[i] = (uint8_t)((i * 13 + 5) & 0xFF);
  slot.source = src;
  std::vector<uint8_t> image;
  image.insert(image.end(), src.begin(), src.begin() + 150);
  image.insert(image.end(), {0xDE, 0xAD, 0xBE, 0xEF});
  image.insert(image.end(), src.begin() + 300, src.begin() + 306);

  arm(image);
  sendCopy(1, 0, 150);
  sendLiteral(2, {0xDE, 0xAD, 0xBE, 0xEF});
  sendCopy(3, 300, 6);
  sendEnd(4);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Ready);
  EXPECT_EQ(slot.staged, image);
}

TEST_F(OtaTest, CopyOutOfBoundsIsRejected) {
  auto image = testImage(8);
  arm(image);
  sendCopy(1, (uint32_t)slot.source.size() - 2, 4);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Error);
  EXPECT_NE(statusLine().find("err=6"), std::string::npos);
}

TEST_F(OtaTest, LiteralOverflowIsRejected) {
  auto image = testImage(8);
  arm(image);
  sendLiteral(1, std::vector<uint8_t>(9, 0xAA));   // image only holds 8
  EXPECT_EQ(ota.state(), OtaUpdate::State::Error);
}

TEST_F(OtaTest, LiteralWithTrailingJunkIsRejected) {
  auto image = testImage(4);
  arm(image);
  std::vector<uint8_t> body(2 + 4 + 1, 0);   // len says 4, buffer has 5
  body[0] = 4;
  auto pkt = makePacket(frame(OTA_OP_LITERAL, 1, body));
  ota.onPacket(&pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Error);
}

// ============================================================
// OP_END — verify then ready
// ============================================================

TEST_F(OtaTest, EndWithWrongShaIsRejected) {
  auto image = testImage(16);
  arm(image);
  // write one byte too many claims; apply wrong bytes instead
  // (easiest: arm for `image` but write a different image)
  sendLiteral(1, std::vector<uint8_t>(16, 0xFF));
  sendEnd(2);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Error);
  EXPECT_NE(statusLine().find("err=4"), std::string::npos);
  EXPECT_TRUE(slot.aborted);
}

TEST_F(OtaTest, EndBeforeImageCompleteIsRejected) {
  auto image = testImage(16);
  arm(image);
  sendLiteral(1, std::vector<uint8_t>(8, 0));
  sendEnd(2);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Error);
}

TEST_F(OtaTest, FullImageEndToEndReady) {
  auto image = testImage(24);
  arm(image);
  std::vector<uint8_t> half(image.begin(), image.begin() + 12);
  std::vector<uint8_t> half2(image.begin() + 12, image.end());
  sendLiteral(1, half);
  sendLiteral(2, half2);
  sendEnd(3);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Ready);
  EXPECT_EQ(slot.staged, image);
}

// ============================================================
// APPLY / ROLLBACK — the boot health record
// ============================================================

TEST_F(OtaTest, ApplyWhenNotReadyEnrefuses) {
  char reply[160];
  EXPECT_FALSE(ota.applyCommand(reply));
  EXPECT_EQ(board.reboots, 0);
}

TEST_F(OtaTest, ApplyWritesPendingRecordAndReboots) {
  auto image = testImage(16);
  arm(image);
  sendLiteral(1, image);
  sendEnd(2);
  ASSERT_EQ(ota.state(), OtaUpdate::State::Ready);

  char reply[160];
  EXPECT_TRUE(ota.applyCommand(reply));
  EXPECT_EQ(board.reboots, 1);
  EXPECT_TRUE(slot.set_boot_called);
  EXPECT_TRUE(fs.exists(OTA_PEND_FILE));
}

TEST_F(OtaTest, BootHealthPassClearsPendingRecord) {
  // freshly-applied boot: write the pending record via apply, then boot a
  // fresh engine instance over the same filesystem + slot state
  auto image = testImage(16);
  arm(image);
  sendLiteral(1, image);
  sendEnd(2);
  char reply[160];
  ASSERT_TRUE(ota.applyCommand(reply));

  FakeSlotIo new_slot;
  new_slot.source = slot.staged;   // the new firmware IS the staged slot now
  OtaUpdate fresh;
  fresh.setSlotIo(&new_slot);
  fresh.begin(&fs, &gate, &board, "test-fw-2.0.0");
  std::vector<SentFrame> fresh_sent;
  fresh.setSender(captureSend, &fresh_sent);
  fresh.loop();   // verify + probe

  ASSERT_EQ(fresh_sent.size(), 1u);
  EXPECT_EQ(fresh_sent[0].payload[2], OTA_OP_NOP);
  mesh::Packet health = makePacket(fresh_sent[0].payload);
  fresh.onTxResult(&health, true);
  EXPECT_FALSE(fs.exists(OTA_PEND_FILE));
}

TEST_F(OtaTest, BootHealthShaMismatchRollsBack) {
  auto image = testImage(16);
  arm(image);
  sendLiteral(1, image);
  sendEnd(2);
  char reply[160];
  ASSERT_TRUE(ota.applyCommand(reply));

  FakeSlotIo new_slot;
  new_slot.source = slot.staged;
  new_slot.source[3] ^= 0x40;   // flash corruption against the verified image
  MockBoard board2;
  OtaUpdate fresh;
  fresh.setSlotIo(&new_slot);
  fresh.begin(&fs, &gate, &board2, "test-fw-2.0.0");
  fresh.loop();
  EXPECT_EQ(board2.reboots, 1);
  EXPECT_TRUE(new_slot.set_boot_called);
}

TEST_F(OtaTest, BootHealthProbeFailureRollsBack) {
  auto image = testImage(16);
  arm(image);
  sendLiteral(1, image);
  sendEnd(2);
  char reply[160];
  ASSERT_TRUE(ota.applyCommand(reply));

  FakeSlotIo new_slot;
  new_slot.source = slot.staged;
  MockBoard board2;
  OtaUpdate fresh;
  fresh.setSlotIo(&new_slot);
  fresh.setSender(captureSend, &sent);
  fresh.begin(&fs, &gate, &board2, "test-fw-2.0.0");
  fresh.loop();
  ASSERT_EQ(sent.size(), 1u);
  mesh::Packet health = makePacket(sent[0].payload);
  fresh.onTxResult(&health, false);   // TX timeout
  EXPECT_EQ(board2.reboots, 1);
}

TEST_F(OtaTest, RolledbackMarkerSurfacedAndAcknowledged) {
  // seed a ROLLED record the way a real rollback writes it (apply failed the
  // boot health check), then boot the old image over the same fs and check
  // that `ota status` surfaces the rollback exactly once
  auto image = testImage(8);
  arm(image);
  sendLiteral(1, image);
  sendEnd(2);
  char reply[160];
  ASSERT_TRUE(ota.applyCommand(reply));
  FakeSlotIo new_slot;
  new_slot.source = slot.staged;
  new_slot.source[0] ^= 0x01;   // corrupt the freshly-applied "image"
  MockBoard board2;
  OtaUpdate fresh;
  fresh.setSlotIo(&new_slot);
  fresh.begin(&fs, &gate, &board2, "test-fw-2.0.0");
  fresh.loop();                   // digest mismatch -> ROLLED + flip + reboot
  EXPECT_TRUE(fs.exists(OTA_PEND_FILE));

  // the board reboots into the old image; a new engine instance reads the marker
  OtaUpdate old_image;
  old_image.setSlotIo(&new_slot);
  old_image.begin(&fs, &gate, &board2, "test-fw-1.0.0");
  char r[192];
  old_image.formatStatus(r, sizeof(r));
  EXPECT_NE(std::string(r).find("rolledback=1"), std::string::npos);
  EXPECT_TRUE(old_image.abortCommand(r));
  old_image.formatStatus(r, sizeof(r));
  EXPECT_NE(std::string(r).find("rolledback=0"), std::string::npos);
}

TEST_F(OtaTest, RollbackFromIdleFlipsPartitionAndReboots) {
  char reply[160];
  EXPECT_TRUE(ota.rollbackCommand(reply));
  EXPECT_EQ(board.reboots, 1);
  EXPECT_TRUE(slot.set_boot_called);
}

// ============================================================
// CLI SURFACE
// ============================================================

TEST_F(OtaTest, AbortCommandClearsSession) {
  auto image = testImage(16);
  arm(image);
  char reply[160];
  EXPECT_TRUE(ota.abortCommand(reply));
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
}

TEST_F(OtaTest, StageCommandArmsContract) {
  auto image = testImage(8);
  uint8_t sha[32];
  mockSha256(image, sha);
  char sha_hex[65];
  for (int i = 0; i < 32; i++) snprintf(sha_hex + i * 2, 3, "%02x", sha[i]);
  std::string cmd = std::string("8 ") + sha_hex + " 1.2.3";
  char reply[160];
  EXPECT_TRUE(ota.stageCommand("8", sha_hex, "1.2.3", reply));
  EXPECT_EQ(ota.state(), OtaUpdate::State::Staging);
  EXPECT_TRUE(ota.abortCommand(reply));
  EXPECT_FALSE(ota.stageCommand("x", sha_hex, "1.2.3", reply));
  EXPECT_FALSE(ota.stageCommand("8", "abcd", "1.2.3", reply));
  EXPECT_FALSE(ota.stageCommand("8", sha_hex, "1.2.x", reply));
}

TEST_F(OtaTest, CliVerbRouting) {
  char reply[192];
  otaCLI(ota, "", reply);
  EXPECT_NE(std::string(reply).find("running=test-fw-1.0.0"), std::string::npos);
  otaCLI(ota, "bogus", reply);
  EXPECT_NE(std::string(reply).find("Usage: ota"), std::string::npos);
  otaCLI(ota, "status", reply);
  EXPECT_NE(std::string(reply).find("state=idle"), std::string::npos);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
}

TEST_F(OtaTest, StatusLineIsBoundedAndComplete) {
  char reply[192];
  reply[0] = 0;
  ota.formatStatus(reply, 160);
  EXPECT_LT(strlen(reply), 157u);
  EXPECT_NE(strstr(reply, "running=test-fw-1.0.0"), nullptr);
  EXPECT_NE(strstr(reply, "sha="), nullptr);
  EXPECT_NE(strstr(reply, "state="), nullptr);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
