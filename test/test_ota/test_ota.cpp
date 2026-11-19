// test_ota.cpp — native unit tests for the LoRa OTA session engine
// (examples/simple_repeater/OtaUpdate*). Built by [env:native_ota] in
// platformio.ini; see that env for the shim and flag notes. Flash sits behind
// OtaSlotIo, so the whole engine runs here against a RAM-backed fake; /ota_pend
// uses the in-memory NativeFS shim; the ESP32-only partition glue
// (Esp32SlotIo) is exercised on-device, not here.

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include <zlib.h>

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

// Deterministic byte source for arm tokens (the tests read tokens back out of
// the CLI reply, they never predict them).
struct MockRng : mesh::RNG {
  uint8_t counter = 0;
  void random(uint8_t* dest, size_t sz) override {
    for (size_t i = 0; i < sz; i++) dest[i] = (uint8_t)(0x41 + counter++ * 37 + 11);
  }
};

struct FakeSlotIo : OtaSlotIo {
  uint32_t slot_size = 0x200000;
  std::vector<uint8_t> source;   // running-slot contents
  std::vector<uint8_t> staged;   // image bytes written so far
  uint32_t image_size = 0;
  bool begin_ok = true, write_ok = true, end_ok = true, read_ok = true;
  bool set_boot_ok = true;
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
    return set_boot_ok;
  }
};

struct SentFrame {
  std::vector<uint8_t> payload;
};

static bool captureSend(void* ctx, const uint8_t* payload, size_t len) {
  auto* vec = static_cast<std::vector<SentFrame>*>(ctx);
  SentFrame f;
  f.payload.assign(payload, payload + len);
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

static std::vector<uint8_t> askFrame() {
  std::vector<uint8_t> p = {OTA_MAGIC0, OTA_MAGIC1, OTA_OP_ASK};
  uint16_t crc = crc16_ccitt(p.data(), p.size());
  p.push_back((uint8_t)(crc & 0xFF));
  p.push_back((uint8_t)(crc >> 8));
  return p;
}

static std::vector<uint8_t> bodyFrame(uint8_t op, const std::vector<uint8_t>& body) {
  std::vector<uint8_t> p = {OTA_MAGIC0, OTA_MAGIC1, op};
  p.insert(p.end(), body.begin(), body.end());
  uint16_t crc = crc16_ccitt(p.data(), p.size());
  p.push_back((uint8_t)(crc & 0xFF));
  p.push_back((uint8_t)(crc >> 8));
  return p;
}

static std::vector<uint8_t> infoFrame() { return bodyFrame(OTA_OP_INFO, {}); }

static void mockSha256(const std::vector<uint8_t>& data, uint8_t out[32]) {
  SHA256 sha;
  sha.update(data.data(), data.size());
  sha.finalize(out, 32);
}

static uint32_t le32From(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t testTargetId() {
  const char* env = "native_test";
  uint8_t hash[4];
  mesh::Utils::sha256(hash, 4, (const uint8_t*)env, (int)strlen(env));
  return le32From(hash);
}

// OP_START body: size sha256 target token
static std::vector<uint8_t> makeStartBody(uint32_t size, const uint8_t sha[32],
                                          uint32_t target, const uint8_t token[4]) {
  std::vector<uint8_t> b(4 + 32 + 4 + 4);
  for (int i = 0; i < 4; i++) b[i] = (uint8_t)(size >> (8 * i));
  memcpy(b.data() + 4, sha, 32);
  for (int i = 0; i < 4; i++) b[36 + i] = (uint8_t)(target >> (8 * i));
  memcpy(b.data() + 40, token, 4);
  return b;
}

// token as the raw 4 bytes the CLI printed (hex pairs in order) — exactly
// what handleStart() memcmp's against the pending arm
static std::vector<uint8_t> armTokenOf(const std::string& reply) {
  size_t i = reply.find("token=");
  std::vector<uint8_t> token(4);
  for (int k = 0; k < 4; k++) {
    char pair[3] = {reply[i + 6 + k * 2], reply[i + 6 + k * 2 + 1], 0};
    token[k] = (uint8_t)strtoul(pair, nullptr, 16);
  }
  return token;
}

class OtaTest : public ::testing::Test {
protected:
  NativeFS fs;
  MockBoard board;
  MockRng rng;
  BatteryGate gate;
  FakeSlotIo slot;
  OtaUpdate ota;
  std::vector<SentFrame> sent;

  void SetUp() override {
    g_mock_millis = 0;
    gate.begin(&fs);
    ota.setSlotIo(&slot);
    ota.setSender(captureSend, &sent);
    ota.begin(&fs, &gate, &board, "test-fw-1.0.0", &rng);
  }

  // Feed one frame to the engine, exactly as handleRequest() does from an
  // admin client's REQ. Returns the OP_REPORT/INFO_REPORT frame the op asks
  // for (or an empty vector).
  std::vector<uint8_t> feed(const std::vector<uint8_t>& f) {
    std::vector<uint8_t> report(OTA_REPORT_MAX, 0);
    size_t n = ota.onFrame(f.data(), f.size(), report.data(), report.size());
    report.resize(n);
    return report;
  }

  static std::vector<uint8_t> testImage(size_t n = 520) {
    std::vector<uint8_t> img(n);
    for (size_t i = 0; i < n; i++) img[i] = (uint8_t)((i * 37 + 11) & 0xFF);
    return img;
  }

  // Run the CLI arm (as an operator would, over an authenticated channel),
  // returning the reply with the token.
  std::string stage(const std::vector<uint8_t>& image, const char* ver = "1.2.3") {
    uint8_t sha[32];
    mockSha256(image, sha);
    char sha_hex[65];
    for (int i = 0; i < 32; i++) snprintf(sha_hex + i * 2, 3, "%02x", sha[i]);
    char size_str[24];
    snprintf(size_str, sizeof size_str, "%zu", image.size());
    char reply[160];
    ota.stageCommand(size_str, sha_hex, ver, reply);
    return std::string(reply);
  }

  // Full arm flow: CLI stage, then OP_START with the returned token.
  void arm(const std::vector<uint8_t>& image, uint32_t target = 0, uint8_t token_flip = 0) {
    if (target == 0) target = testTargetId();
    uint8_t sha[32];
    mockSha256(image, sha);
    std::string reply = stage(image);
    auto token = armTokenOf(reply);
    token[0] ^= token_flip;   // flip a byte to model a stolen/damaged token
    auto body = makeStartBody((uint32_t)image.size(), sha, target, token.data());
    auto pkt = frame(OTA_OP_START, 0, body);
    feed(pkt);
  }

  void sendLiteral(uint16_t seq, const std::vector<uint8_t>& data) {
    std::vector<uint8_t> body(2 + data.size());
    body[0] = (uint8_t)(data.size() & 0xFF);
    body[1] = (uint8_t)(data.size() >> 8);
    memcpy(body.data() + 2, data.data(), data.size());
    auto pkt = frame(OTA_OP_LITERAL, seq, body);
    feed(pkt);
  }

  void sendCopy(uint16_t seq, uint32_t base_off, uint16_t len) {
    std::vector<uint8_t> body(6);
    for (int i = 0; i < 4; i++) body[i] = (uint8_t)(base_off >> (8 * i));
    body[4] = (uint8_t)(len & 0xFF);
    body[5] = (uint8_t)(len >> 8);
    auto pkt = frame(OTA_OP_COPY, seq, body);
    feed(pkt);
  }

  void sendEnd(uint16_t seq) {
    auto pkt = frame(OTA_OP_END, seq, {});
    feed(pkt);
  }

  std::vector<uint8_t> sendAsk() {
    return feed(askFrame());
  }

  std::string statusLine() {
    char reply[192];
    ota.formatStatus(reply, sizeof(reply));
    return std::string(reply);
  }

  // Stage and apply [image] as the operator would: the image lands in
  // slot.staged and the boot-health marker is left behind for the next boot.
  // One literal covers it (the test images are tiny).
  void applyImage(const std::vector<uint8_t>& image) {
    arm(image);
    sendLiteral(1, image);
    sendEnd(2);
    ASSERT_EQ(ota.state(), OtaUpdate::State::Ready);
    char reply[160];
    ASSERT_TRUE(ota.applyCommand(reply));
  }

  // The reboot after an apply: a fresh engine over the same filesystem, whose
  // running slot is exactly what the update wrote.
  void bootFresh(OtaUpdate& fresh, FakeSlotIo& io, MockBoard& b) {
    io.source = slot.staged;
    fresh.setSlotIo(&io);
    fresh.begin(&fs, &gate, &b, "test-fw-2.0.0", &rng);
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
  uint8_t sha[32] = {0};
  const uint8_t token[4] = {1, 2, 3, 4};
  auto pkt = frame(OTA_OP_START, 0, makeStartBody(4, sha, testTargetId(), token));
  pkt[10] ^= 0x01;   // corrupt a body byte
  feed(pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
}

TEST_F(OtaTest, RejectsBadMagic) {
  uint8_t sha[32] = {0};
  const uint8_t token[4] = {1, 2, 3, 4};
  auto p = frame(OTA_OP_START, 0, makeStartBody(4, sha, 0, token));
  p[0] = 'X';
  auto pkt = p;
  feed(pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
}

TEST_F(OtaTest, RejectsTruncatedFrame) {
  std::vector<uint8_t> p = {OTA_MAGIC0, OTA_MAGIC1, OTA_OP_START};
  auto pkt = p;
  EXPECT_NO_FATAL_FAILURE(feed(pkt));
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
}

TEST_F(OtaTest, RejectsWrongTarget) {
  auto image = testImage();
  arm(image, 0xDEADBEEF);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
  EXPECT_NE(statusLine().find("err=1"), std::string::npos);
}

TEST_F(OtaTest, IdleNodeIgnoresOps) {
  sendLiteral(1, {1, 2, 3, 4});
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
}

// ============================================================
// ARM TOKEN — the operator contract gate
// ============================================================

TEST_F(OtaTest, ArmWithoutPendingRejected) {
  auto image = testImage();
  uint8_t sha[32];
  mockSha256(image, sha);
  const uint8_t token[4] = {0x12, 0x34, 0x56, 0x78};   // never armed by an operator
  auto body = makeStartBody((uint32_t)image.size(), sha, testTargetId(), token);
  auto pkt = frame(OTA_OP_START, 0, body);   // no `ota stage` first
  feed(pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
  EXPECT_NE(statusLine().find("err=7"), std::string::npos);
}

TEST_F(OtaTest, ArmWithWrongTokenRejectedPendingUntouched) {
  auto image = testImage();
  arm(image, 0, /*token_flip=*/0x01);   // flip a token byte
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
  EXPECT_NE(statusLine().find("err=7"), std::string::npos);
  arm(image);   // pending arm survived the bad attempt
  EXPECT_EQ(ota.state(), OtaUpdate::State::Staging);
}

TEST_F(OtaTest, OverheardTokenCannotSubstituteImage) {
  auto genuine = testImage();
  auto attacker = testImage(512);
  attacker[0] ^= 0xFF;
  std::string reply = stage(genuine);         // operator arms ONLY for `genuine`
  auto token = armTokenOf(reply);
  uint8_t asha[32];
  mockSha256(attacker, asha);
  auto body = makeStartBody((uint32_t)attacker.size(), asha, testTargetId(), token.data());
  auto pkt = frame(OTA_OP_START, 0, body);   // attacker with the overheard token
  feed(pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
  EXPECT_NE(statusLine().find("err=7"), std::string::npos);
  arm(genuine);   // genuine flow still works on the same pending arm
  EXPECT_EQ(ota.state(), OtaUpdate::State::Staging);
}

TEST_F(OtaTest, ArmConsumesPendingArmStaleTokenIsUnusable) {
  auto image = testImage();
  uint8_t sha[32];
  mockSha256(image, sha);
  std::string reply = stage(image);   // one pending arm
  auto token = armTokenOf(reply);
  auto pkt = frame(OTA_OP_START, 0,
      makeStartBody((uint32_t)image.size(), sha, testTargetId(), token.data()));
  feed(pkt);                  // consumed by this arm
  ASSERT_EQ(ota.state(), OtaUpdate::State::Staging);

  // an exact replay is refused (busy); and the stale arm is not usable after an abort either
  pkt = frame(OTA_OP_START, 0,
      makeStartBody((uint32_t)image.size(), sha, testTargetId(), token.data()));
  feed(pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Staging);

  char r[160];
  ota.abortCommand(r);
  pkt = frame(OTA_OP_START, 0,
      makeStartBody((uint32_t)image.size(), sha, testTargetId(), token.data()));
  feed(pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);   // stale token arms nothing
  EXPECT_NE(statusLine().find("err=7"), std::string::npos);
}

TEST_F(OtaTest, PendingArmExpires) {
  auto image = testImage();
  uint8_t sha[32];
  mockSha256(image, sha);
  std::string reply = stage(image);
  auto token = armTokenOf(reply);
  g_mock_millis += OTA_PENDING_TTL_MS + 1;
  auto body = makeStartBody((uint32_t)image.size(), sha, testTargetId(), token.data());
  auto pkt = frame(OTA_OP_START, 0, body);
  feed(pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
  EXPECT_NE(statusLine().find("err=7"), std::string::npos);
}

TEST_F(OtaTest, SecondStageReplacesFirstToken) {
  auto genuine = testImage(16);
  std::string reply_a = stage(genuine);
  auto image2 = testImage(24);
  std::string reply_b = stage(image2);
  auto token_a = armTokenOf(reply_a);
  uint8_t sha2[32];
  mockSha256(image2, sha2);
  auto body = makeStartBody((uint32_t)image2.size(), sha2, testTargetId(), token_a.data());
  auto pkt = frame(OTA_OP_START, 0, body);   // old token, new contract
  feed(pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
  EXPECT_NE(statusLine().find("err=7"), std::string::npos);
  // new token works
  auto token_b = armTokenOf(reply_b);
  body = makeStartBody((uint32_t)image2.size(), sha2, testTargetId(), token_b.data());
  pkt = frame(OTA_OP_START, 0, body);
  feed(pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Staging);
}

TEST_F(OtaTest, AbortClearsPendingArm) {
  auto image = testImage();
  uint8_t sha[32];
  mockSha256(image, sha);
  std::string reply = stage(image);
  auto token = armTokenOf(reply);
  char r[160];
  EXPECT_TRUE(ota.abortCommand(r));
  auto body = makeStartBody((uint32_t)image.size(), sha, testTargetId(), token.data());
  auto pkt = frame(OTA_OP_START, 0, body);
  feed(pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
  EXPECT_NE(statusLine().find("err=7"), std::string::npos);
}

TEST_F(OtaTest, ArmWhileBatterySuspendedRefused) {
  auto image = testImage();
  uint8_t sha[32];
  mockSha256(image, sha);
  std::string reply = stage(image);   // accepted while the battery is fine
  auto token = armTokenOf(reply);
  suspendBattery();                   // gate engages before the app's OP_START arrives
  auto body = makeStartBody((uint32_t)image.size(), sha, testTargetId(), token.data());
  auto pkt = frame(OTA_OP_START, 0, body);
  feed(pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
  EXPECT_NE(statusLine().find("err=2"), std::string::npos);
}

TEST_F(OtaTest, ArmWithNullRngRefused) {
  // a board without an RNG can be attacked the same way as any other
  // uninitialised path: refuse rather than arm
  FakeSlotIo naked_slot;
  MockBoard naked_board;
  BatteryGate naked_gate;
  naked_gate.begin(&fs);
  OtaUpdate naked;   // outlives the slot io / board / gate (reverse declaration)
  naked.setSlotIo(&naked_slot);
  naked.begin(&fs, &naked_gate, &naked_board, "test-fw-1.0.0", nullptr);
  char reply[160];
  auto image = testImage();
  uint8_t sha[32];
  mockSha256(image, sha);
  char sha_hex[65];
  for (int i = 0; i < 32; i++) snprintf(sha_hex + i * 2, 3, "%02x", sha[i]);
  EXPECT_FALSE(naked.stageCommand("8", sha_hex, "1.2.3", reply));
  EXPECT_TRUE(strstr(reply, "rng") != nullptr);
}

TEST_F(OtaTest, StageCliRejectsZeroSize) {
  char reply[160];
  uint8_t sha[32] = {0};
  char sha_hex[65];
  for (int i = 0; i < 32; i++) snprintf(sha_hex + i * 2, 3, "%02x", sha[i]);
  EXPECT_FALSE(ota.stageCommand("0", sha_hex, "1.2.3", reply));
  EXPECT_NE(std::string(reply).find("Err - size"), std::string::npos);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
}

TEST_F(OtaTest, StageCliRefusesAnImageTheSlotCannotHold) {
  char reply[160];
  uint8_t sha[32] = {0};
  char sha_hex[65];
  for (int i = 0; i < 32; i++) snprintf(sha_hex + i * 2, 3, "%02x", sha[i]);
  char size_str[24];
  snprintf(size_str, sizeof size_str, "%lu", (unsigned long)slot.slot_size + 1);
  EXPECT_FALSE(ota.stageCommand(size_str, sha_hex, "1.2.3", reply));
  EXPECT_NE(std::string(reply).find("exceeds"), std::string::npos);
}

// The session is the last word on whether the slot can hold the image: an
// armed size that no longer fits is refused at OP_START (err=3, slot).
TEST_F(OtaTest, StartRefusesASizeTheSlotCannotHold) {
  auto image = testImage(64);
  auto token = armTokenOf(stage(image));
  slot.slot_size = 32;                     // the slot cannot hold 64 bytes
  uint8_t sha[32];
  mockSha256(image, sha);
  auto body = makeStartBody((uint32_t)image.size(), sha, testTargetId(), token.data());
  feed(frame(OTA_OP_START, 0, body));
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
  EXPECT_NE(statusLine().find("err=3"), std::string::npos);
}

TEST_F(OtaTest, RejectsStartWithNonZeroSeq) {
  auto image = testImage();
  uint8_t sha[32];
  mockSha256(image, sha);
  std::string reply = stage(image);
  auto token = armTokenOf(reply);
  auto body = makeStartBody((uint32_t)image.size(), sha, testTargetId(), token.data());
  auto pkt = frame(OTA_OP_START, 5, body);
  feed(pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
}

TEST_F(OtaTest, RejectsSecondStartWhileArmed) {
  auto image = testImage();
  arm(image);
  arm(image);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Staging);
}

TEST_F(OtaTest, AskGetsReportResponse) {
  auto image = testImage();
  arm(image);
  ASSERT_EQ(ota.state(), OtaUpdate::State::Staging);
  auto report = sendAsk();
  ASSERT_EQ(report.size(), 7u);
  EXPECT_EQ(report[0], OTA_MAGIC0);
  EXPECT_EQ(report[1], OTA_MAGIC1);
  EXPECT_EQ(report[2], OTA_OP_REPORT);
  EXPECT_EQ(report[3] | (report[4] << 8), 1);               // next_expected
}

TEST_F(OtaTest, AskWithStrayBodyGetsNoReply) {
  auto image = testImage();
  arm(image);
  ASSERT_EQ(ota.state(), OtaUpdate::State::Staging);
  // OP_ASK carries no body; a stray byte is not an ask.
  std::vector<uint8_t> p = {OTA_MAGIC0, OTA_MAGIC1, OTA_OP_ASK, 0x00};
  uint16_t crc = crc16_ccitt(p.data(), p.size());
  p.push_back((uint8_t)(crc & 0xFF));
  p.push_back((uint8_t)(crc >> 8));
  auto report = feed(p);
  EXPECT_TRUE(report.empty());
}

// ============================================================
// OP_INFO — the negotiation handshake (answered in any state)
// ============================================================

TEST_F(OtaTest, InfoReportAnswersWhenIdle) {
  auto report = feed(infoFrame());
  ASSERT_EQ(report.size(), OTA_INFO_REPORT_LEN);
  EXPECT_EQ(report[0], OTA_MAGIC0);
  EXPECT_EQ(report[1], OTA_MAGIC1);
  EXPECT_EQ(report[2], OTA_OP_INFO_REPORT);
  // frame CRC is valid
  uint16_t stored = (uint16_t)report[report.size() - 2] | ((uint16_t)report[report.size() - 1] << 8);
  EXPECT_EQ(crc16_ccitt(report.data(), report.size() - 2), stored);
  const uint8_t* body = report.data() + 3;
  EXPECT_EQ(body[0], OTA_WIRE_VERSION);
  EXPECT_EQ(le32From(body + 1), testTargetId());
  EXPECT_EQ(le32From(body + 5), 0x200000u);                       // FakeSlotIo slot size
  EXPECT_EQ(body[9] | (body[10] << 8), (uint16_t)OTA_MAX_LITERAL);
  EXPECT_EQ(le32From(body + 11),
            (uint32_t)(OTA_FEAT_DELTA | OTA_FEAT_APPLY | OTA_FEAT_BLOCK_HASH));
  // The fake slot holds no ESP image, so the running sha is reported as
  // unknown (all zeros) rather than as a hash of erase fill.
  for (size_t k = 0; k < 32; k++) EXPECT_EQ(body[OTA_INFO_RUNNING_SHA_OFF + k], 0u);
  EXPECT_STREQ((const char*)(body + OTA_INFO_RUNNING_VER_OFF), "test-fw-1.0.0");
}

// A synthetic ESP app image: 24-byte header (magic, segment count, chip id,
// hash_appended), one 400-byte segment, the checksum byte, padding to 16 and,
// when `with_digest`, the appended SHA-256 that esptool writes by default.
static std::vector<uint8_t> syntheticImage(bool with_digest, uint8_t fill = 0x5A) {
  const size_t seg_len = 400;
  std::vector<uint8_t> img(24 + 8 + seg_len, 0);
  img[0] = 0xE9;
  img[1] = 1;
  img[0x0c] = 9;                          // chip id: ESP32-S3
  img[0x17] = with_digest ? 1 : 0;         // hash_appended
  img[24 + 4] = (uint8_t)(seg_len & 0xFF);
  img[24 + 5] = (uint8_t)((seg_len >> 8) & 0xFF);
  img[24 + 6] = (uint8_t)((seg_len >> 16) & 0xFF);
  for (size_t k = 0; k < seg_len; k++) img[24 + 8 + k] = (uint8_t)(fill + k);
  img.resize((img.size() + 1 + 15) & ~(size_t)15);   // + checksum, padded to 16
  if (with_digest) {
    uint8_t digest[32];
    mockSha256(img, digest);
    img.insert(img.end(), digest, digest + 32);
  }
  return img;
}

TEST_F(OtaTest, InfoReportHashesTheRunningImageBytes) {
  auto image = syntheticImage(/*with_digest=*/true);
  std::copy(image.begin(), image.end(), slot.source.begin());
  auto report = feed(infoFrame());
  ASSERT_EQ(report.size(), OTA_INFO_REPORT_LEN);
  uint8_t expect[32];
  mockSha256(image, expect);   // the whole .bin, digest included
  EXPECT_EQ(memcmp(report.data() + 3 + OTA_INFO_RUNNING_SHA_OFF, expect, 32), 0);
}

TEST_F(OtaTest, InfoReportHashesImageWithoutAppendedDigest) {
  auto image = syntheticImage(/*with_digest=*/false);
  std::copy(image.begin(), image.end(), slot.source.begin());
  auto report = feed(infoFrame());
  ASSERT_EQ(report.size(), OTA_INFO_REPORT_LEN);
  uint8_t expect[32];
  mockSha256(image, expect);   // ends at the padded checksum
  EXPECT_EQ(memcmp(report.data() + 3 + OTA_INFO_RUNNING_SHA_OFF, expect, 32), 0);
}

// ============================================================
// OP_BLOCK_HASH — the read-only diff oracle
// ============================================================

static std::vector<uint8_t> hashFrame(
    const std::vector<std::pair<uint32_t, uint32_t>>& ranges) {
  std::vector<uint8_t> body;
  body.push_back((uint8_t)ranges.size());
  for (const auto& r : ranges) {
    for (int i = 0; i < 4; i++) body.push_back((uint8_t)(r.first >> (8 * i)));
    for (int i = 0; i < 4; i++) body.push_back((uint8_t)(r.second >> (8 * i)));
  }
  return bodyFrame(OTA_OP_BLOCK_HASH, body);
}

static uint8_t hashStatus(const std::vector<uint8_t>& report, size_t i) {
  return report[3 + 5 + i * (4 + 1 + OTA_HASH_LEN) + 4];
}

static const uint8_t* hashDigest(const std::vector<uint8_t>& report, size_t i) {
  return report.data() + 3 + 5 + i * (4 + 1 + OTA_HASH_LEN) + 5;
}

static std::vector<uint8_t> rawDeflate(const std::vector<uint8_t>& data) {
  z_stream z;
  memset(&z, 0, sizeof(z));
  deflateInit2(&z, 9, Z_DEFLATED, -15, 9, Z_DEFAULT_STRATEGY);
  z.next_in = const_cast<Bytef*>(data.data());
  z.avail_in = data.size();
  std::vector<uint8_t> out(data.size() + 1024);
  z.next_out = out.data();
  z.avail_out = out.size();
  deflate(&z, Z_FINISH);
  size_t n = out.size() - z.avail_out;
  deflateEnd(&z);
  out.resize(n);
  return out;
}

static std::vector<uint8_t> literalZBody(uint32_t orig_len, uint16_t comp_total,
                                         uint8_t flags,
                                         const std::vector<uint8_t>& chunk) {
  std::vector<uint8_t> body;
  for (int i = 0; i < 4; i++) body.push_back((uint8_t)(orig_len >> (8 * i)));
  body.push_back((uint8_t)(comp_total & 0xFF));
  body.push_back((uint8_t)(comp_total >> 8));
  body.push_back(flags);
  body.insert(body.end(), chunk.begin(), chunk.end());
  return body;
}

TEST_F(OtaTest, CompressedLiteralOpsRebuildTheImage) {
  // A pseudo-random image whose deflate stream is larger than one op frame.
  std::vector<uint8_t> image(8192);
  uint32_t x = 0x12345678;
  for (size_t i = 0; i < image.size(); i++) {
    x = x * 1664525u + 1013904223u;
    image[i] = (uint8_t)(x >> 24);
  }
  auto comp = rawDeflate(image);
  ASSERT_GT(comp.size(), (size_t)OTA_LZ_MAX_DATA);

  uint16_t seq = 1;
  size_t at = 0;
  while (at < comp.size()) {
    size_t n = comp.size() - at;
    if (n > OTA_LZ_MAX_DATA) n = OTA_LZ_MAX_DATA;
    uint8_t flags = (at == 0 ? OTA_LZ_START : 0) |
                    (at + n == comp.size() ? OTA_LZ_END : 0);
    std::vector<uint8_t> chunk(comp.begin() + at, comp.begin() + at + n);
    feed(frame(OTA_OP_LITERAL_Z, seq++,
               literalZBody(image.size(), comp.size(), flags, chunk)));
    at += n;
  }
  // The ops above were sent without an arm; verify the framing is inert, then
  // run the same stream inside a real staged session.
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);

  arm(image);
  seq = 1;
  at = 0;
  while (at < comp.size()) {
    size_t n = comp.size() - at;
    if (n > OTA_LZ_MAX_DATA) n = OTA_LZ_MAX_DATA;
    uint8_t flags = (at == 0 ? OTA_LZ_START : 0) |
                    (at + n == comp.size() ? OTA_LZ_END : 0);
    std::vector<uint8_t> chunk(comp.begin() + at, comp.begin() + at + n);
    feed(frame(OTA_OP_LITERAL_Z, seq++,
               literalZBody(image.size(), comp.size(), flags, chunk)));
    at += n;
  }
  sendEnd(seq);
  ASSERT_EQ(ota.state(), OtaUpdate::State::Ready);
  EXPECT_EQ(slot.staged, image);
}

TEST_F(OtaTest, CompressedLiteralStreamWithMissingEndFailsStaging) {
  auto image = syntheticImage(/*with_digest=*/true);
  auto comp = rawDeflate(image);
  arm(image);
  std::vector<uint8_t> chunk(comp.begin(), comp.end());
  feed(frame(OTA_OP_LITERAL_Z, 1,
             literalZBody(image.size(), comp.size(), OTA_LZ_START, chunk)));
  // No END flag and then OP_END: the unfinished stream must be refused.
  sendEnd(2);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Error);
}

TEST_F(OtaTest, BlockHashMetadataReportsSizeSegmentsAndWholeHash) {
  auto image = syntheticImage(/*with_digest=*/true);
  std::copy(image.begin(), image.end(), slot.source.begin());
  auto report = feed(hashFrame({}));
  ASSERT_EQ(report.size(), OTA_HASH_META_LEN);
  EXPECT_EQ(report[2], OTA_OP_BLOCK_HASH_REPORT);
  const uint8_t* body = report.data() + 3;
  EXPECT_EQ(le32From(body), (uint32_t)image.size());
  EXPECT_EQ(body[4], 0u);                       // metadata marker
  ASSERT_EQ(body[5], 1u);                       // one program segment
  EXPECT_EQ(le32From(body + 6), 32u);           // payload starts after the header
  EXPECT_EQ(le32From(body + 10), 400u);         // its length
  uint8_t expect[32];
  mockSha256(image, expect);
  EXPECT_EQ(memcmp(body + 6 + OTA_HASH_MAX_SEGS * 8, expect, 32), 0);
}

TEST_F(OtaTest, BlockHashHashesRequestedRanges) {
  auto image = syntheticImage(/*with_digest=*/true);
  std::copy(image.begin(), image.end(), slot.source.begin());
  // Four hash calculations in one call — the whole point of the batch.
  auto report = feed(hashFrame({{0, 16}, {32, 400}, {0, 0}, {448, 32}}));
  ASSERT_EQ(report.size(), OTA_HASH_RANGE_LEN(4));
  for (int i = 0; i < 4; i++) {
    EXPECT_EQ(hashStatus(report, i), 0u) << "range " << i;
  }
  const size_t lengths[4] = {16, 400, image.size(), 32};
  const size_t offsets[4] = {0, 32, 0, 448};
  for (int i = 0; i < 4; i++) {
    uint8_t expect[32];
    mockSha256(std::vector<uint8_t>(image.begin() + offsets[i],
                                    image.begin() + offsets[i] + lengths[i]), expect);
    EXPECT_EQ(memcmp(hashDigest(report, i), expect, 32), 0) << "range " << i;
  }
}

TEST_F(OtaTest, BlockHashRejectsAliasedAndOversizedRanges) {
  auto image = syntheticImage(/*with_digest=*/true);
  std::copy(image.begin(), image.end(), slot.source.begin());
  auto report = feed(hashFrame({{1, 16},        // misaligned
                                {0, image.size() + 16},  // out of bounds
                                {32, OTA_HASH_MAX_RANGE + 4},  // too long
                                {image.size(), 0}}));    // empty tail: hashed
  ASSERT_EQ(report.size(), OTA_HASH_RANGE_LEN(4));
  EXPECT_EQ(hashStatus(report, 0), 3u);
  EXPECT_EQ(hashStatus(report, 1), 1u);
  EXPECT_EQ(hashStatus(report, 2), 3u);
  EXPECT_EQ(hashStatus(report, 3), 0u);
}

TEST_F(OtaTest, BlockHashIsReadOnlyInEverySessionState) {
  auto image = syntheticImage(/*with_digest=*/true);
  std::copy(image.begin(), image.end(), slot.source.begin());
  auto metadata = hashFrame({});
  ASSERT_EQ(feed(metadata).size(), OTA_HASH_META_LEN);   // Idle
  ASSERT_EQ(ota.state(), OtaUpdate::State::Idle);

  arm(image);
  ASSERT_EQ(feed(metadata).size(), OTA_HASH_META_LEN);   // Staging
  EXPECT_EQ(ota.state(), OtaUpdate::State::Staging);
  EXPECT_EQ(slot.staged.size(), 0u);                     // no writes by the oracle

  sendLiteral(1, image);
  sendEnd(2);
  ASSERT_EQ(ota.state(), OtaUpdate::State::Ready);
  ASSERT_EQ(feed(metadata).size(), OTA_HASH_META_LEN);   // Ready
  EXPECT_EQ(ota.state(), OtaUpdate::State::Ready);
}

TEST_F(OtaTest, BlockHashWorksAfterAFailedStaging) {
  auto image = syntheticImage(/*with_digest=*/true);
  std::copy(image.begin(), image.end(), slot.source.begin());
  arm(image);
  sendCopy(1, 0xFFFFFF00u, 16);                  // out of bounds → Error
  ASSERT_EQ(ota.state(), OtaUpdate::State::Error);
  ASSERT_EQ(feed(hashFrame({{0, 16}})).size(), OTA_HASH_RANGE_LEN(1));
  EXPECT_EQ(ota.state(), OtaUpdate::State::Error);
}

static uint64_t rollingTest(const std::vector<uint8_t>& d, size_t at, size_t len) {
  uint64_t h = 0;
  for (size_t i = 0; i < len; i++) h = h * 0x00000100000001B3ull + d[at + i];
  return h;
}

static std::vector<uint8_t> findFrame(uint32_t len, uint32_t start, uint32_t end,
                                      const std::vector<uint64_t>& wants) {
  std::vector<uint8_t> body;
  for (int i = 0; i < 4; i++) body.push_back((uint8_t)(len >> (8 * i)));
  for (int i = 0; i < 4; i++) body.push_back((uint8_t)(start >> (8 * i)));
  for (int i = 0; i < 4; i++) body.push_back((uint8_t)(end >> (8 * i)));
  body.push_back((uint8_t)wants.size());
  for (auto w : wants) {
    for (int i = 0; i < 8; i++) body.push_back((uint8_t)(w >> (8 * i)));
  }
  return bodyFrame(OTA_OP_BLOCK_FIND, body);
}

static std::vector<int> findHits(const std::vector<uint8_t>& report, size_t i) {
  size_t at = 4;
  for (size_t k = 0; k < i; k++) {
    uint8_t n = report[at++];
    at += n * 4;
  }
  std::vector<int> out;
  uint8_t n = report[at++];
  for (uint8_t k = 0; k < n; k++) {
    out.push_back((int)le32From(report.data() + at));
    at += 4;
  }
  return out;
}

TEST_F(OtaTest, BlockFindLocatesShiftedBlocksInOneScan) {
  auto image = syntheticImage(/*with_digest=*/true);
  std::copy(image.begin(), image.end(), slot.source.begin());
  const uint64_t want = rollingTest(image, 32, 16);
  const uint64_t bogus = want ^ 0x12345678ull;
  auto report = feed(findFrame(16, 0, (uint32_t)image.size(),
                               {bogus, want, want + 1}));
  ASSERT_FALSE(report.empty());
  ASSERT_EQ(report[2], OTA_OP_BLOCK_FIND_REPORT);
  EXPECT_TRUE(findHits(report, 0).empty());
  auto hits = findHits(report, 1);
  ASSERT_FALSE(hits.empty());
  EXPECT_TRUE(std::find(hits.begin(), hits.end(), 32) != hits.end());
  EXPECT_TRUE(findHits(report, 2).empty());
}

TEST_F(OtaTest, BlockFindRejectsBadRangesWithoutDisturbingStaging) {
  auto image = syntheticImage(/*with_digest=*/true);
  std::copy(image.begin(), image.end(), slot.source.begin());
  arm(image);
  const uint64_t want = rollingTest(image, 32, 16);
  // Misaligned start, end past the image, and zero length all produce no reply.
  EXPECT_TRUE(feed(findFrame(16, 1, (uint32_t)image.size(), {want})).empty());
  EXPECT_TRUE(feed(findFrame(16, 0, (uint32_t)image.size() + 4, {want})).empty());
  EXPECT_TRUE(feed(findFrame(0, 0, (uint32_t)image.size(), {want})).empty());
  EXPECT_EQ(ota.state(), OtaUpdate::State::Staging);
}

TEST_F(OtaTest, BlockFindSpanningManyReadChunksFindsEveryFingerprint) {
  // The bulk read-ahead refills every 1 KB; make the scan cross dozens of
  // chunks so an off-by-one in the refill path cannot hide in a small image.
  const uint32_t seg_len = 64 * 1024;
  std::vector<uint8_t> img(24 + 8 + seg_len, 0);
  img[0] = 0xE9;
  img[1] = 1;
  img[0x0c] = 9;
  img[0x17] = 1;                            // hash_appended
  img[24 + 4] = (uint8_t)(seg_len & 0xFF);
  img[24 + 5] = (uint8_t)((seg_len >> 8) & 0xFF);
  img[24 + 6] = (uint8_t)((seg_len >> 16) & 0xFF);
  uint32_t rng = 0x12345678;
  for (uint32_t k = 0; k < seg_len; k++) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    img[24 + 8 + k] = (uint8_t)rng;
  }
  img.resize((img.size() + 1 + 15) & ~(size_t)15);
  uint8_t digest[32];
  mockSha256(img, digest);
  img.insert(img.end(), digest, digest + 32);
  std::copy(img.begin(), img.end(), slot.source.begin());

  const uint32_t offsets[] = {0, 4096, 12288, 32768, 57344};
  std::vector<uint64_t> wants;
  for (auto o : offsets) wants.push_back(rollingTest(img, o, 32));
  auto report = feed(findFrame(32, 0, (uint32_t)img.size(), wants));
  ASSERT_FALSE(report.empty());
  ASSERT_EQ(report[2], OTA_OP_BLOCK_FIND_REPORT);
  for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
    auto hits = findHits(report, i);
    EXPECT_TRUE(std::find(hits.begin(), hits.end(), (int)offsets[i]) != hits.end())
        << "fingerprint at " << offsets[i] << " not found";
  }
}

TEST_F(OtaTest, BlockHashWithoutAnImageReportsZeroSize) {
  // FakeSlotIo starts as erase fill, which is not an ESP app image.
  auto report = feed(hashFrame({}));
  ASSERT_EQ(report.size(), OTA_HASH_META_LEN);
  EXPECT_EQ(le32From(report.data() + 3), 0u);
  EXPECT_EQ(report[3 + 5], 0u);                  // no segments
  auto range = feed(hashFrame({{0, 16}}));
  ASSERT_EQ(range.size(), OTA_HASH_RANGE_LEN(1));
  EXPECT_EQ(hashStatus(range, 0), 3u);
}

TEST_F(OtaTest, StatusShaMatchesTheInfoReport) {
  auto image = syntheticImage(/*with_digest=*/true);
  std::copy(image.begin(), image.end(), slot.source.begin());
  auto report = feed(infoFrame());
  ASSERT_EQ(report.size(), OTA_INFO_REPORT_LEN);
  char expect_sha16[17];
  for (int k = 0; k < 8; k++)
    snprintf(expect_sha16 + k * 2, 3, "%02x", report[3 + OTA_INFO_RUNNING_SHA_OFF + k]);
  char reply[160];
  ota.formatStatus(reply, sizeof(reply));
  EXPECT_NE(strstr(reply, (std::string("sha=") + expect_sha16).c_str()), nullptr);
}

TEST_F(OtaTest, StatusShaIsZeroWhenTheSlotHoldsNoImage) {
  char reply[160];
  ota.formatStatus(reply, sizeof(reply));
  EXPECT_NE(strstr(reply, "sha=0000000000000000"), nullptr);
}

TEST_F(OtaTest, InfoReportAdvertisesTheReachableMaxLiteral) {
  // The largest OP_LITERAL that fits one MAX_PACKET_PAYLOAD request: the REQ
  // envelope (type + frame length), the op frame overhead and the literal's
  // own length prefix all come out of the datagram.
  EXPECT_EQ((size_t)OTA_MAX_LITERAL + 2 + OTA_FRAME_OVERHEAD + 2, (size_t)MAX_PACKET_PAYLOAD);
  auto report = feed(infoFrame());
  ASSERT_EQ(report.size(), OTA_INFO_REPORT_LEN);
  EXPECT_EQ(report[3 + 9] | (report[3 + 10] << 8), (uint16_t)OTA_MAX_LITERAL);
}

TEST_F(OtaTest, InfoReportAnswersDuringStaging) {
  auto image = testImage();
  arm(image);
  ASSERT_EQ(ota.state(), OtaUpdate::State::Staging);
  auto report = feed(infoFrame());
  ASSERT_EQ(report.size(), OTA_INFO_REPORT_LEN);
  EXPECT_EQ(report[2], OTA_OP_INFO_REPORT);
}

TEST_F(OtaTest, InfoReportAnswersAfterStagingError) {
  auto image = testImage(16);
  arm(image);
  sendLiteral(1, std::vector<uint8_t>(16, 0xFF));   // wrong bytes for the contract
  sendEnd(2);                                       // sha mismatch → err=4
  ASSERT_EQ(ota.state(), OtaUpdate::State::Error);
  auto report = feed(infoFrame());
  ASSERT_EQ(report.size(), OTA_INFO_REPORT_LEN);
  EXPECT_EQ(report[2], OTA_OP_INFO_REPORT);
}

TEST_F(OtaTest, InfoReportToleratesStrayBody) {
  auto report = feed(bodyFrame(OTA_OP_INFO, {0xDE, 0xAD, 0xBE, 0xEF}));
  ASSERT_EQ(report.size(), OTA_INFO_REPORT_LEN);
  EXPECT_EQ(report[2], OTA_OP_INFO_REPORT);
}

TEST_F(OtaTest, InfoReportOnUnwiredEngineAnswersWithZeros) {
  OtaUpdate fresh;   // never wired: no fs, slot io, rng, or firmware version
  std::vector<uint8_t> f = infoFrame();
  std::vector<uint8_t> report(OTA_REPORT_MAX, 0);
  size_t n = fresh.onFrame(f.data(), f.size(), report.data(), report.size());
  report.resize(n);
  ASSERT_EQ(report.size(), OTA_INFO_REPORT_LEN);
  const uint8_t* body = report.data() + 3;
  EXPECT_EQ(body[0], OTA_WIRE_VERSION);
  EXPECT_EQ(le32From(body + 1), testTargetId());   // OTA_PIO_ENV is fixed at compile time
  EXPECT_EQ(le32From(body + 5), 0u);    // no slot size without a slot io
  EXPECT_EQ(le32From(body + 11), 0u);   // no features without flash
  for (size_t i = OTA_INFO_RUNNING_VER_OFF; i < OTA_INFO_BODY; i++) {
    EXPECT_EQ(body[i], 0u);             // running version and sha are zero
  }
}

// ============================================================
// IN-ORDER OP APPLICATION
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

TEST_F(OtaTest, OutOfOrderOpsAreDropped) {
  auto image = testImage(16);
  arm(image);
  std::vector<uint8_t> half2(image.begin() + 8, image.end());
  sendLiteral(2, half2);   // gap: seq 1 never arrived; drop
  EXPECT_NE(statusLine().find("off=0"), std::string::npos);
  std::vector<uint8_t> half1(image.begin(), image.begin() + 8);
  sendLiteral(1, half1);
  EXPECT_NE(statusLine().find("off=8"), std::string::npos);
}

TEST_F(OtaTest, GapReportedThenResentFromGap) {
  auto image = testImage(32);
  arm(image);
  std::vector<uint8_t> part(image.begin(), image.begin() + 8);
  sendLiteral(1, part);
  sendLiteral(4, part);   // dropped: seq 2 and 3 never arrived
  auto report = sendAsk();
  ASSERT_EQ(report.size(), 7u);
  EXPECT_EQ(report[2], OTA_OP_REPORT);
  EXPECT_EQ(report[3] | (report[4] << 8), 2);               // next_expected
  sendLiteral(2, part);
  sendLiteral(3, part);
  sendLiteral(4, part);
  EXPECT_NE(statusLine().find("off=32"), std::string::npos);
}

TEST_F(OtaTest, DuplicateOpIsIgnored) {
  auto image = testImage(16);
  arm(image);
  std::vector<uint8_t> half1(image.begin(), image.begin() + 8);
  sendLiteral(1, half1);
  sendLiteral(1, half1);   // duplicate
  EXPECT_NE(statusLine().find("off=8"), std::string::npos);
}

TEST_F(OtaTest, AskWhenIdleGetsNoReply) {
  auto report = sendAsk();
  EXPECT_TRUE(report.empty());
}

// ============================================================
// OP_COPY / OP_LITERAL — staging the image
// ============================================================

TEST_F(OtaTest, CopyOpStagesBytesFromSource) {
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
  auto pkt = frame(OTA_OP_LITERAL, 1, body);
  feed(pkt);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Error);
}

// ============================================================
// OP_END — verify then ready
// ============================================================

TEST_F(OtaTest, EndWithWrongShaIsRejected) {
  auto image = testImage(16);
  arm(image);
  sendLiteral(1, std::vector<uint8_t>(16, 0xFF));   // wrong content for the armed contract
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
  EXPECT_NE(statusLine().find("err=8"), std::string::npos);
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
// APPLY / ROLLBACK — the boot health marker
// ============================================================

// A retransmitted OP_END (whose reply was lost on the way back) must leave a
// verified session ready — not re-end it and scrap it.
TEST_F(OtaTest, DuplicateEndKeepsSessionReady) {
  auto image = testImage(16);
  arm(image);
  sendLiteral(1, image);
  sendEnd(2);
  ASSERT_EQ(ota.state(), OtaUpdate::State::Ready);
  // The report the app resumes from must already say "nothing left to send",
  // otherwise its recovery would resend OP_END and scrap the session.
  auto report = sendAsk();
  ASSERT_EQ(report.size(), 7u);
  EXPECT_EQ(report[3] | (report[4] << 8), 3);  // END was seq 2
  sendEnd(2);                                  // the app's retry of the same op
  EXPECT_EQ(ota.state(), OtaUpdate::State::Ready);
  EXPECT_FALSE(slot.aborted);
  char reply[160];
  EXPECT_TRUE(ota.applyCommand(reply));        // still applyable
}

// Nothing may write to a verified image: a stray op after OP_END is a no-op.
TEST_F(OtaTest, OpsAfterEndDoNotTouchTheVerifiedImage) {
  auto image = testImage(16);
  arm(image);
  sendLiteral(1, image);
  sendEnd(2);
  ASSERT_EQ(ota.state(), OtaUpdate::State::Ready);
  sendLiteral(3, std::vector<uint8_t>(4, 0xAA));   // next seq, after END
  EXPECT_EQ(ota.state(), OtaUpdate::State::Ready);
  EXPECT_EQ(slot.staged, image);
  EXPECT_FALSE(slot.aborted);
}

// A frame too short to carry a seq is dropped, not read past its end and not
// treated as a malformed op that scraps the transfer.
TEST_F(OtaTest, ShortOpFrameIsDroppedWithoutScrapping) {
  auto image = testImage(16);
  arm(image);
  std::vector<uint8_t> short_frame = {OTA_MAGIC0, OTA_MAGIC1, OTA_OP_END};
  uint16_t crc = crc16_ccitt(short_frame.data(), short_frame.size());
  short_frame.push_back((uint8_t)(crc & 0xFF));
  short_frame.push_back((uint8_t)(crc >> 8));
  feed(short_frame);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Staging);
  EXPECT_EQ(slot.staged.size(), 0u);
}

// A failed transfer must not need `ota abort` before the next attempt: a fresh
// arm replaces it, and `ota stage` alone clears the stale error.
TEST_F(OtaTest, ArmAfterAFailedSessionReplacesIt) {
  auto image = testImage(16);
  arm(image);
  sendLiteral(1, std::vector<uint8_t>(16, 0xFF));   // wrong bytes
  sendEnd(2);                                       // sha mismatch -> Error
  ASSERT_EQ(ota.state(), OtaUpdate::State::Error);

  arm(image);                                       // re-arm + OP_START
  EXPECT_EQ(ota.state(), OtaUpdate::State::Staging);
  EXPECT_NE(statusLine().find("err=0"), std::string::npos);
  sendLiteral(1, image);
  sendEnd(2);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Ready);
}

TEST_F(OtaTest, StageCommandClearsAFailedSession) {
  auto image = testImage(16);
  arm(image);
  sendLiteral(1, std::vector<uint8_t>(16, 0xFF));
  sendEnd(2);
  ASSERT_EQ(ota.state(), OtaUpdate::State::Error);
  stage(image);                                     // operator re-arms
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
}

// A failed boot-partition flip must not leave a marker behind: it would owe a
// verdict on the next boot for an image that never booted.
TEST_F(OtaTest, ApplyUnsetsTheMarkerWhenTheFlipFails) {
  auto image = testImage(16);
  arm(image);
  sendLiteral(1, image);
  sendEnd(2);
  ASSERT_EQ(ota.state(), OtaUpdate::State::Ready);
  slot.set_boot_ok = false;

  char reply[160];
  EXPECT_FALSE(ota.applyCommand(reply));
  EXPECT_FALSE(fs.exists(OTA_PEND_FILE));
  EXPECT_EQ(board.reboots, 0);
}

TEST_F(OtaTest, ApplyWhenNotReadyIsRefused) {
  char reply[160];
  EXPECT_FALSE(ota.applyCommand(reply));
  EXPECT_EQ(board.reboots, 0);
}

TEST_F(OtaTest, ApplyWritesPendingMarkerAndReboots) {
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

TEST_F(OtaTest, BootHealthPassClearsMarker) {
  // freshly-applied boot: apply, then boot a fresh engine over the same fs
  applyImage(testImage(16));

  FakeSlotIo new_slot;
  OtaUpdate fresh;
  bootFresh(fresh, new_slot, board);
  std::vector<SentFrame> fresh_sent;
  fresh.setSender(captureSend, &fresh_sent);
  fresh.loop();   // verify + probe

  ASSERT_EQ(fresh_sent.size(), 1u);
  EXPECT_EQ(fresh_sent[0].payload[2], OTA_OP_NOP);
  mesh::Packet health = makePacket(fresh_sent[0].payload);
  fresh.onTxResult(&health, true);
  EXPECT_FALSE(fs.exists(OTA_PEND_FILE));
}

TEST_F(OtaTest, BootHealthProbeFailureRollsBack) {
  applyImage(testImage(16));

  FakeSlotIo new_slot;
  MockBoard board2;
  OtaUpdate fresh;
  fresh.setSender(captureSend, &sent);
  bootFresh(fresh, new_slot, board2);
  fresh.loop();
  ASSERT_EQ(sent.size(), 1u);
  mesh::Packet health = makePacket(sent[0].payload);
  fresh.onTxResult(&health, false);   // TX timeout
  EXPECT_EQ(board2.reboots, 1);
  EXPECT_FALSE(fs.exists(OTA_PEND_FILE));
}

TEST_F(OtaTest, BootHealthProbeTimeoutRollsBack) {
  applyImage(testImage(16));

  FakeSlotIo new_slot;
  MockBoard board2;
  OtaUpdate fresh;
  bootFresh(fresh, new_slot, board2);
  std::vector<SentFrame> fresh_sent;
  fresh.setSender(captureSend, &fresh_sent);
  fresh.loop();                       // probe dispatched, never completed
  ASSERT_EQ(fresh_sent.size(), 1u);
  g_mock_millis += 20000;             // health deadline (15 s) expires with no TX result
  fresh.loop();
  EXPECT_EQ(board2.reboots, 1);
}

// An image that hangs or crashes before loop() can never fail the probe, so
// the attempt counter in the marker is what flips it back. Each boot of the
// new image bumps it; past the limit the node rolls back without needing a
// working radio.

TEST_F(OtaTest, EarlyBootCheckCountsAttemptsWithoutRollingBackYet) {
  applyImage(testImage(16));
  ASSERT_TRUE(fs.exists(OTA_PEND_FILE));

  FakeSlotIo new_slot;
  MockBoard board2;
  OtaUpdate fresh;
  bootFresh(fresh, new_slot, board2);
  fresh.earlyBootCheck(&fs, &board2);
  EXPECT_EQ(board2.reboots, 0);        // first boot of a new image: let it run
  EXPECT_TRUE(fs.exists(OTA_PEND_FILE));
}

TEST_F(OtaTest, EarlyBootCheckRollsBackAfterAttemptsExhausted) {
  applyImage(testImage(16));

  FakeSlotIo new_slot;
  MockBoard board2;
  OtaUpdate fresh;
  bootFresh(fresh, new_slot, board2);
  for (int i = 0; i < OTA_HEALTH_MAX_ATTEMPTS; i++) {
    fresh.earlyBootCheck(&fs, &board2);         // a boot that crashed before the probe
    EXPECT_EQ(board2.reboots, 0);
  }
  fresh.earlyBootCheck(&fs, &board2);           // one boot too many
  EXPECT_EQ(board2.reboots, 1);
  EXPECT_TRUE(slot.set_boot_called || new_slot.set_boot_called);
  EXPECT_FALSE(fs.exists(OTA_PEND_FILE)); // the old image must not re-trigger
}

TEST_F(OtaTest, EarlyBootCheckIsInertWithoutAMarker) {
  MockBoard board2;
  OtaUpdate fresh;
  fresh.setSlotIo(&slot);
  fresh.begin(&fs, &gate, &board2, "test-fw-2.0.0", &rng);
  fresh.earlyBootCheck(&fs, &board2);
  EXPECT_EQ(board2.reboots, 0);
}

// A freshly-applied image whose radio cannot start halts before loop(), so the
// probe never runs. It must flip back on that first boot instead of stranding
// the node until someone power-cycles it.
TEST_F(OtaTest, RadioInitFailureOnAFreshImageRollsBackImmediately) {
  applyImage(testImage(16));

  FakeSlotIo new_slot;
  MockBoard board2;
  OtaUpdate fresh;
  bootFresh(fresh, new_slot, board2);
  fresh.earlyBootCheck(&fs, &board2);
  EXPECT_EQ(board2.reboots, 0);        // counted, not yet a failure
  EXPECT_TRUE(fresh.radioInitFailed());
  EXPECT_EQ(board2.reboots, 1);        // flipped back without a power cycle
  EXPECT_TRUE(new_slot.set_boot_called);
  EXPECT_FALSE(fs.exists(OTA_PEND_FILE));
}

TEST_F(OtaTest, RadioInitFailureWithoutAFreshImageDoesNotRollBack) {
  MockBoard board2;
  OtaUpdate fresh;
  fresh.setSlotIo(&slot);
  fresh.begin(&fs, &gate, &board2, "test-fw-2.0.0", &rng);
  EXPECT_FALSE(fresh.radioInitFailed());   // nothing owed: ordinary boot
  EXPECT_EQ(board2.reboots, 0);
}

TEST_F(OtaTest, RollbackFromIdleFlipsPartitionAndReboots) {
  char reply[160];
  EXPECT_TRUE(ota.rollbackCommand(reply));
  EXPECT_EQ(board.reboots, 1);
  EXPECT_TRUE(slot.set_boot_called);
  EXPECT_FALSE(fs.exists(OTA_PEND_FILE));
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

TEST_F(OtaTest, StageCliRefusesNullSha) {
  char reply[160];
  EXPECT_FALSE(ota.stageCommand("8", nullptr, "1.2.3", reply));
  EXPECT_NE(std::string(reply).find("Err - sha256"), std::string::npos);
}

TEST_F(OtaTest, StageCliRefusesLongVersion) {
  std::string longver(100, 'a');
  char reply[160];
  uint8_t sha[32] = {0};
  char sha_hex[65];
  for (int i = 0; i < 32; i++) snprintf(sha_hex + i * 2, 3, "%02x", sha[i]);
  EXPECT_FALSE(ota.stageCommand("8", sha_hex, longver.c_str(), reply));
  EXPECT_NE(std::string(reply).find("ver"), std::string::npos);
}

TEST_F(OtaTest, StageCliRefusesWhileBatterySuspended) {
  suspendBattery();
  char reply[160];
  auto image = testImage(8);
  uint8_t sha[32];
  mockSha256(image, sha);
  char sha_hex[65];
  for (int i = 0; i < 32; i++) snprintf(sha_hex + i * 2, 3, "%02x", sha[i]);
  otaCLI(ota, (std::string("stage 8 ") + sha_hex + " 1.2.3").c_str(), reply);
  EXPECT_NE(std::string(reply).find("battery gate"), std::string::npos);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
}

TEST_F(OtaTest, StageCliReturnsTokenAndDoesNotArm) {
  auto image = testImage(8);
  std::string reply = stage(image);
  EXPECT_NE(reply.find("staging armed"), std::string::npos);
  EXPECT_NE(reply.find("token="), std::string::npos);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);   // pending arm, not a session
}

TEST_F(OtaTest, CliVerifyEndToEndStageThenStart) {
  auto image = testImage(8);
  stage(image);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Idle);
  arm(image);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Staging);
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

TEST_F(OtaTest, CorruptSegmentLengthReportsNoImageInsteadOfWrapping) {
  auto image = syntheticImage(true);
  image[24 + 4] = 0x00;
  image[24 + 5] = 0x00;
  image[24 + 6] = 0x00;
  image[24 + 7] = 0xFF;   // declares a ~4 GB segment
  std::copy(image.begin(), image.end(), slot.source.begin());
  auto report = feed(bodyFrame(OTA_OP_BLOCK_HASH, {0}));
  ASSERT_EQ(report.size(), OTA_HASH_META_LEN);
  EXPECT_EQ(le32From(report.data() + 3), 0u);   // unreadable image
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
