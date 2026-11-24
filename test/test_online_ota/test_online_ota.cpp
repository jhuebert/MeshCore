// test_online_ota.cpp — host tests for online OTA: version/URL policy, the
// async core with a fake network, and local staging on the real OTA engine.
// Built by [env:native_online_ota].

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include <Arduino.h>
#include <SHA256.h>
#include <Utils.h>

#include "OtaUpdate.h"
#include "BatteryGate.h"
#include "CliUtil.h"
#include "PersistUtil.h"
#include "OnlineOtaPolicy.h"
#include "OnlineOtaCore.h"

using namespace OnlineOtaPolicy;

static const char* kEnv = "Xiao_S3_WIO_repeater_observer";
static const char* kVersion = "1.18.0-dev.11";
static const char* kSha = "sha256:39483a72f358ea976125928a45394115d452ec29b4dbe4bf35c9e2ddf3f2d826";

struct MockBoard : public mesh::MainBoard {
  uint16_t mV = 4200;
  uint16_t getBattMilliVolts() override { return mV; }
  const char* getManufacturerName() const override { return "mock"; }
  void reboot() override {}
  uint8_t getStartupReason() const override { return 0; }
};

struct MockRng : public mesh::RNG {
  void random(uint8_t* d, size_t n) override { for (size_t i = 0; i < n; i++) d[i] = (uint8_t)(i + 1); }
};

struct FakeSlotIo : public OtaSlotIo {
  uint32_t slot_size = 0x200000;
  std::vector<uint8_t> staged;
  uint32_t image_size = 0;
  bool aborted = false;
  uint32_t slotSize() const override { return slot_size; }
  uint32_t sourceSize() const override { return 0x200000; }
  bool beginWrite(uint32_t sz) override { staged.clear(); image_size = sz; aborted = false; return true; }
  bool append(const uint8_t* d, size_t n) override {
    if ((uint64_t)staged.size() + n > image_size) return false;
    staged.insert(staged.end(), d, d + n);
    return true;
  }
  bool readSource(uint32_t, uint8_t*, size_t) override { return false; }
  bool readStaged(uint32_t off, uint8_t* out, size_t n) override {
    if ((uint64_t)off + n > staged.size()) return false;
    memcpy(out, staged.data() + off, n);
    return true;
  }
  bool endWrite() override { return true; }
  void abortWrite() override { aborted = true; }
  bool setInactiveAsBoot() override { return true; }
};

static std::vector<uint8_t> imageBytes(size_t n = 64) {
  std::vector<uint8_t> image(n);
  for (size_t i = 0; i < n; i++) image[i] = (uint8_t)((i * 37 + 11) & 0xFF);
  return image;
}

static std::string toHex(const uint8_t* data, size_t n) {
  static const char* digits = "0123456789abcdef";
  std::string out;
  for (size_t i = 0; i < n; i++) {
    out += digits[data[i] >> 4];
    out += digits[data[i] & 0x0F];
  }
  return out;
}

static std::string imageSha(const std::vector<uint8_t>& image) {
  uint8_t digest[32] = {0};
  SHA256 sha;
  sha.update(image.data(), image.size());
  sha.finalize(digest, 32);
  return toHex(digest, 32);
}

// A lazy network: contentLength() is 0 until the first step, then reports the
// image size. Every non-final chunk is tagged More, never Busy.
class FakeNet : public OnlineOtaNet {
public:
  bool available_ = true, station_up_ = true, clock_valid_ = true, heap_ok_ = true;
  bool want_station = false;
  int cancels = 0;
  bool fail_begin = false;
  uint8_t step_err = 0;
  uint32_t declared_size = 0;   // 0 means "use body size"
  std::string body;
  size_t offset = 0;
  bool headers = false;
  uint8_t chunk[32];

  bool available() const override { return available_; }
  void requestStation(bool want) override { want_station = want; }
  bool stationConnected() override { return station_up_; }
  bool clockValid() override { return clock_valid_; }
  bool heapOk() override { return heap_ok_; }
  uint32_t contentLength() const override {
    if (!headers) return 0;
    return declared_size != 0 ? declared_size : (uint32_t)body.size();
  }
  bool beginGet(const char*, uint32_t) override { return !fail_begin; }
  Step step(const uint8_t** out, size_t* len, uint8_t* err) override {
    *out = nullptr;
    *len = 0;
    if (step_err != 0) { *err = step_err; step_err = 0; return Step::Error; }
    if (!headers) { headers = true; return Step::Busy; }   // headers arrived
    if (offset >= body.size()) return Step::Done;
    const size_t n = std::min(sizeof(chunk), body.size() - offset);
    memcpy(chunk, body.data() + offset, n);
    offset += n;
    *out = chunk;
    *len = n;
    return offset >= body.size() ? Step::Done : Step::More;
  }
  void cancel() override { cancels++; }
};

class OnlineTest : public ::testing::Test {
protected:
  NativeFS fs;
  MockBoard board;
  MockRng rng;
  BatteryGate gate;
  FakeSlotIo slot;
  OtaUpdate ota;
  FakeNet net;
  OnlineOtaCore core;

  void SetUp() override {
    g_mock_millis = 0;
    gate.begin(&fs);
    ota.setSlotIo(&slot);
    ota.begin(&fs, &gate, &board, "test-fw-1.0.0", &rng);
    core.begin(&net, &ota, kEnv, slot.slotSize());
    setImage(imageBytes());
  }

  void setImage(const std::vector<uint8_t>& image) {
    net.body.assign(image.begin(), image.end());
    net.offset = 0;
    net.headers = false;
    net.declared_size = 0;
  }

  void pump(int steps = 60) {
    for (int i = 0; i < steps && core.phase() != OnlineOtaCore::Phase::Ready &&
                       core.phase() != OnlineOtaCore::Phase::Error; i++) core.loop();
  }

  std::string status() {
    char reply[192];
    ota.formatStatus(reply, sizeof(reply));
    return reply;
  }

  bool arm(const std::vector<uint8_t>& image) {
    char reply[160];
    if (!core.setVersion(kVersion, reply)) return false;
    if (!core.setSha(("sha256:" + imageSha(image)).c_str(), reply)) return false;
    return core.start(reply);
  }
};

// ==================================================== policy ================

TEST(OnlineOtaPolicyTest, NormalizesVersionTokens) {
  char out[ONLINE_OTA_MAX_VERSION + 1];
  ASSERT_TRUE(normalizeVersion("1.18.0-dev.11", out, sizeof(out)));
  EXPECT_STREQ(out, "1.18.0-dev.11");
  ASSERT_TRUE(normalizeVersion(" v1.18.0-dev.11-filter-10d4da2 ", out, sizeof(out)));
  EXPECT_STREQ(out, "1.18.0-dev.11");
  EXPECT_FALSE(normalizeVersion("", out, sizeof(out)));
  EXPECT_FALSE(normalizeVersion("1.0/../evil", out, sizeof(out)));
  EXPECT_FALSE(normalizeVersion(std::string(65, 'a').c_str(), out, sizeof(out)));
}

TEST(OnlineOtaPolicyTest, ParsesShaWithOptionalPrefix) {
  uint8_t digest[32];
  const std::string hex = std::string(kSha).substr(7);
  ASSERT_TRUE(parseSha(kSha, digest));
  EXPECT_EQ(toHex(digest, 32), hex);
  ASSERT_TRUE(parseSha(hex.c_str(), digest));
  EXPECT_EQ(toHex(digest, 32), hex);
  EXPECT_FALSE(parseSha("sha256:xyz", digest));
  EXPECT_FALSE(parseSha((std::string("sha256:") + hex.substr(1)).c_str(), digest));
}

TEST(OnlineOtaPolicyTest, BuildsAssetUrlFromVersionAndEnv) {
  char url[ONLINE_OTA_MAX_URL];
  ASSERT_TRUE(buildAssetUrl(kVersion, kEnv, url, sizeof(url)));
  EXPECT_STREQ(url, ONLINE_OTA_BASE_URL "filter-v1.18.0-dev.11/"
                    "Xiao_S3_WIO_repeater_observer-v1.18.0-dev.11.bin");
}

// ==================================================== core ==================

TEST_F(OnlineTest, ThreeCommandsArmAndTheReplyShapesAreStable) {
  char reply[160];
  ASSERT_TRUE(core.setVersion("v1.18.0-dev.11", reply));
  EXPECT_STREQ(reply, "OK - online version set ver=1.18.0-dev.11");
  EXPECT_EQ(core.phase(), OnlineOtaCore::Phase::Idle);
  ASSERT_TRUE(core.setSha(kSha, reply));
  EXPECT_EQ(std::string(reply).find("OK - online sha set sha=39483a72"), 0u);
  EXPECT_FALSE(net.want_station);   // inputs alone never request WiFi
  ASSERT_TRUE(core.start(reply));
  EXPECT_STREQ(reply, "OK - online fetch armed ver=1.18.0-dev.11");
  EXPECT_EQ(core.phase(), OnlineOtaCore::Phase::Wifi);
  EXPECT_TRUE(net.want_station);
  EXPECT_TRUE(core.needsAwake());
}

TEST_F(OnlineTest, StartRequiresBothInputsAndRefusesBadSha) {
  char reply[160];
  EXPECT_FALSE(core.start(reply));
  EXPECT_NE(std::string(reply).find("set ver and sha first"), std::string::npos);
  ASSERT_TRUE(core.setVersion(kVersion, reply));
  EXPECT_FALSE(core.start(reply));
  EXPECT_FALSE(core.setSha("sha256:not-hex", reply));
  EXPECT_NE(std::string(reply).find("64 hex chars"), std::string::npos);
  EXPECT_EQ(core.phase(), OnlineOtaCore::Phase::Idle);
}

TEST_F(OnlineTest, FullDownloadReachesReadyAndReleasesTheStation) {
  auto image = imageBytes();
  ASSERT_TRUE(arm(image));
  pump();
  EXPECT_EQ(core.phase(), OnlineOtaCore::Phase::Ready);
  EXPECT_FALSE(net.want_station);
  EXPECT_FALSE(core.needsAwake());
  EXPECT_EQ(slot.staged, image);
  EXPECT_EQ(ota.state(), OtaUpdate::State::Ready);
  EXPECT_NE(status().find("state=ready"), std::string::npos);
  char reply[160];
  ASSERT_TRUE(ota.applyCommand(reply));
  EXPECT_STREQ(reply, "OK - applied; rebooting");
}

TEST_F(OnlineTest, AbortClearsInputsAndEveryPhase) {
  const OnlineOtaCore::Phase phases[] = {
      OnlineOtaCore::Phase::Wifi, OnlineOtaCore::Phase::Clock,
      OnlineOtaCore::Phase::Asset, OnlineOtaCore::Phase::Verify,
  };
  for (auto target : phases) {
    ASSERT_TRUE(arm(imageBytes())) << (int)target;
    while (core.phase() < target) core.loop();
    ASSERT_EQ(core.phase(), target) << (int)target;
    core.abort();
    EXPECT_EQ(core.phase(), OnlineOtaCore::Phase::Idle);
    EXPECT_FALSE(net.want_station);
    char reply[160];
    EXPECT_FALSE(core.start(reply));   // stored inputs were cleared
  }
}

TEST_F(OnlineTest, ErrorIsReArmableAndReadyRefusesRestart) {
  net.step_err = ONLINE_ERR_HTTP;
  ASSERT_TRUE(arm(imageBytes()));
  pump();
  ASSERT_EQ(core.phase(), OnlineOtaCore::Phase::Error);
  EXPECT_EQ(core.error(), ONLINE_ERR_HTTP);
  EXPECT_NE(status().find("err=14"), std::string::npos);

  net.step_err = 0;
  ASSERT_TRUE(arm(imageBytes()));
  pump();
  ASSERT_EQ(core.phase(), OnlineOtaCore::Phase::Ready);
  char reply[160];
  EXPECT_FALSE(core.start(reply));
  EXPECT_NE(std::string(reply).find("staging active"), std::string::npos);
}

TEST_F(OnlineTest, StationClockHeapAndTlsFailuresMapToTheirCodes) {
  char reply[160];
  {
    net.station_up_ = false;
    ASSERT_TRUE(arm(imageBytes()));
    core.loop();
    g_mock_millis += 40000;
    core.loop();
    EXPECT_EQ(core.error(), ONLINE_ERR_WIFI);
  }
  {
    core.abort(); net.station_up_ = true; net.clock_valid_ = false; g_mock_millis = 0;
    ASSERT_TRUE(arm(imageBytes()));
    core.loop(); core.loop();
    g_mock_millis += 50000; core.loop();
    EXPECT_EQ(core.error(), ONLINE_ERR_CLOCK);
  }
  {
    core.abort(); net.clock_valid_ = true; net.heap_ok_ = false; g_mock_millis = 0;
    ASSERT_TRUE(arm(imageBytes()));
    core.loop(); core.loop(); core.loop();
    EXPECT_EQ(core.error(), ONLINE_ERR_HEAP);
  }
  {
    core.abort(); net.heap_ok_ = true; net.fail_begin = true; g_mock_millis = 0;
    ASSERT_TRUE(arm(imageBytes()));
    pump();
    EXPECT_EQ(core.error(), ONLINE_ERR_TLS);
  }
  EXPECT_FALSE(net.want_station);
  (void)reply;
}

TEST_F(OnlineTest, ShortBodyAndBatterySuspensionMapToTheirCodes) {
  char reply[160];
  {
    auto image = imageBytes(64);
    net.body = std::string(image.begin(), image.end());
    net.declared_size = 128;   // headers promise more than the body delivers
    ASSERT_TRUE(arm(image));
    pump();
    EXPECT_EQ(core.error(), ONLINE_ERR_DOWNLOAD);
  }
  {
    core.abort();
    setImage(imageBytes(64));
    ASSERT_TRUE(arm(imageBytes(64)));
    while (core.phase() < OnlineOtaCore::Phase::Asset) core.loop();
    board.mV = 3000; gate.setThresholds(3400, 3600);
    gate.loop(&fs, board); g_mock_millis += 60000; gate.loop(&fs, board);
    core.loop();
    EXPECT_EQ(core.error(), OTA_ERR_BATTERY);
    EXPECT_NE(status().find("err=2"), std::string::npos);
  }
  EXPECT_FALSE(net.want_station);
  (void)reply;
}

TEST_F(OnlineTest, NetReplyAndFeatureBitFollowTheFrozenSurface) {
  char reply[160];
  onlineOtaCLI(core, "net", reply);
  EXPECT_STREQ(reply, "net=idle ver= sha=00000000 got=0 size=0 err=0");
  onlineOtaCLI(core, "fetch ver", reply);
  EXPECT_NE(std::string(reply).find("Usage: ota"), std::string::npos);
  onlineOtaCLI(core, "fetch start extra", reply);
  EXPECT_NE(std::string(reply).find("Usage: ota"), std::string::npos);
  onlineOtaCLI(core, "bogus", reply);
  EXPECT_NE(std::string(reply).find("Usage: ota"), std::string::npos);
  ASSERT_LT(strlen(reply), (size_t)CLI_REPLY_MAX);

  uint8_t report[OTA_REPORT_MAX];
  std::vector<uint8_t> info = {OTA_MAGIC0, OTA_MAGIC1, OTA_OP_INFO};
  uint16_t crc = crc16_ccitt(info.data(), info.size());
  info.push_back((uint8_t)(crc & 0xFF));
  info.push_back((uint8_t)(crc >> 8));
  ASSERT_EQ(ota.onFrame(info.data(), info.size(), report, sizeof(report)), OTA_INFO_REPORT_LEN);
  uint32_t features = (uint32_t)report[14] | ((uint32_t)report[15] << 8) |
                      ((uint32_t)report[16] << 16) | ((uint32_t)report[17] << 24);
  EXPECT_EQ(features & OTA_FEAT_ONLINE_FETCH, OTA_FEAT_ONLINE_FETCH);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
