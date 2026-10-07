// Native tests for the simple_repeater fleet manager (FleetManager): the
// fleet channel config, the tag set and /fleet_cfg persistence. Reach the
// feature through its public API and fleetCLI() exactly as MyMesh would.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <Arduino.h>        // g_mock_millis (NativeShim.h is force-included)
#include "FleetManager.h"
#include "CliUtil.h"

// ------------------------------------------------------------------ fixture

struct FleetTest : public ::testing::Test {
  NativeFS fs;
  FleetManager fleet;

  void SetUp() override { g_mock_millis = 1000; }
  void TearDown() override { g_mock_millis = 0; }

  // flush a dirty save: edits mark the config dirty, loop() writes it once
  // LAZY_SAVE_DELAY_MS has passed
  void flushSave() {
    g_mock_millis += LAZY_SAVE_DELAY_MS + 1;
    fleet.loop(&fs);
  }

  // a 32-byte PSK whose bytes are 0x00..0x1F, as hex
  static std::string psk32hex() {
    return "000102030405060708090A0B0C0D0E0F"
           "101112131415161718191A1B1C1D1E1F";
  }
};

// ---------------------------------------------------------------- channel

TEST_F(FleetTest, FreshNodeHasNoChannel) {
  EXPECT_FALSE(fleet.isEnabled());
  EXPECT_FALSE(fleet.hasChannel());
  EXPECT_EQ(fleet.getChanSecretLen(), 0);
  EXPECT_EQ(fleet.getTagCount(), 0);
  EXPECT_EQ(fleet.getReplyWindowMs(), (uint32_t)FLEET_REPLY_WINDOW_MS);
}

TEST_F(FleetTest, SetChannel32Bytes) {
  ASSERT_TRUE(fleet.setChannel(psk32hex().c_str()));
  EXPECT_TRUE(fleet.hasChannel());
  EXPECT_EQ(fleet.getChanSecretLen(), 32);

  // stored secret matches the decoded hex
  uint8_t expected[32];
  ASSERT_EQ(cliDecodeHex(psk32hex().c_str(), 64, expected, sizeof(expected)), 32);
  EXPECT_EQ(memcmp(fleet.getChanSecret(), expected, 32), 0);

  // the on-air hash is sha256(secret)[0]
  uint8_t digest[32];
  mesh::Utils::sha256(digest, sizeof(digest), expected, sizeof(expected));
  EXPECT_EQ(fleet.getChanHash(), digest[0]);
}

TEST_F(FleetTest, SetChannel16BytesZeroPads) {
  const char* hex16 = "8b3387e9c5cdea6ac9e5edbaa115cd72";
  ASSERT_TRUE(fleet.setChannel(hex16));
  EXPECT_EQ(fleet.getChanSecretLen(), 16);
  for (int i = 16; i < 32; i++) {
    ASSERT_EQ(fleet.getChanSecret()[i], 0) << "byte " << i << " not zero-padded";
  }
  uint8_t secret[16];
  cliDecodeHex(hex16, 32, secret, sizeof(secret));
  EXPECT_EQ(memcmp(fleet.getChanSecret(), secret, 16), 0);
}

TEST_F(FleetTest, SetChannelRefusesBadHex) {
  ASSERT_TRUE(fleet.setChannel(psk32hex().c_str()));
  EXPECT_FALSE(fleet.setChannel(""));       // empty
  EXPECT_FALSE(fleet.setChannel("00ff"));   // 2 bytes
  EXPECT_FALSE(fleet.setChannel("00ff00")); // odd length
  EXPECT_FALSE(fleet.setChannel("zz3387e9c5cdea6ac9e5edbaa115cd72"));  // non-hex
  EXPECT_FALSE(fleet.setChannel("000102030405060708090A0B0C0D0E0F101112131415161718191A1B1C1D1E1F20"));  // 33 bytes
  // a refusal leaves the previous channel untouched
  EXPECT_EQ(fleet.getChanSecretLen(), 32);
  uint8_t expected[32];
  cliDecodeHex(psk32hex().c_str(), 64, expected, sizeof(expected));
  EXPECT_EQ(memcmp(fleet.getChanSecret(), expected, 32), 0);
}

TEST_F(FleetTest, SetChannelReplaces) {
  ASSERT_TRUE(fleet.setChannel(psk32hex().c_str()));
  const char* other = "ffffffffffffffffffffffffffffffff";
  ASSERT_TRUE(fleet.setChannel(other));
  uint8_t expected[16];
  cliDecodeHex(other, 32, expected, sizeof(expected));
  EXPECT_EQ(fleet.getChanSecretLen(), 16);
  EXPECT_EQ(memcmp(fleet.getChanSecret(), expected, 16), 0);
}

TEST_F(FleetTest, ClearChannel) {
  ASSERT_TRUE(fleet.setChannel(psk32hex().c_str()));
  fleet.clearChannel();
  EXPECT_FALSE(fleet.hasChannel());
  EXPECT_EQ(fleet.getChanSecretLen(), 0);
  fleet.clearChannel();   // idempotent
  EXPECT_FALSE(fleet.hasChannel());
}

// ---------------------------------------------------------------- tags

TEST_F(FleetTest, TagAddAndHas) {
  EXPECT_TRUE(fleet.addTag("xiao"));
  EXPECT_TRUE(fleet.addTag("siteA"));
  EXPECT_EQ(fleet.getTagCount(), 2);
  EXPECT_TRUE(fleet.hasTag("xiao"));
  EXPECT_TRUE(fleet.hasTag("siteA"));
  EXPECT_FALSE(fleet.hasTag("roof"));
  EXPECT_STREQ(fleet.getTag(0), "xiao");
  EXPECT_STREQ(fleet.getTag(1), "siteA");
}

TEST_F(FleetTest, TagAddDuplicateNoOps) {
  ASSERT_TRUE(fleet.addTag("xiao"));
  EXPECT_TRUE(fleet.addTag("xiao"));   // no-op success
  EXPECT_EQ(fleet.getTagCount(), 1);
}

TEST_F(FleetTest, TagCharsetAndLength) {
  EXPECT_TRUE(fleet.addTag("a"));                       // 1 char min
  EXPECT_TRUE(fleet.addTag("0123456789abcdef"));        // 16 chars max
  EXPECT_TRUE(fleet.addTag("A-b_c.d"));
  EXPECT_FALSE(fleet.addTag(""));                       // empty
  EXPECT_FALSE(fleet.addTag("0123456789abcdefg"));      // 17 chars
  EXPECT_FALSE(fleet.addTag("has space"));
  EXPECT_FALSE(fleet.addTag("no*wildcards"));
  EXPECT_FALSE(fleet.addTag("no!bang"));
  EXPECT_FALSE(fleet.addTag("no/slash"));
  EXPECT_FALSE(fleet.addTag("no:colon"));
  EXPECT_EQ(fleet.getTagCount(), 3);
}

TEST_F(FleetTest, TagCapAtMaxTags) {
  for (int i = 0; i < FLEET_MAX_TAGS; i++) {
    char name[8];
    snprintf(name, sizeof(name), "tag%d", i);
    ASSERT_TRUE(fleet.addTag(name)) << name;
  }
  EXPECT_EQ(fleet.getTagCount(), FLEET_MAX_TAGS);
  EXPECT_FALSE(fleet.addTag("onemore"));
  EXPECT_EQ(fleet.getTagCount(), FLEET_MAX_TAGS);
}

TEST_F(FleetTest, TagDelMiddleKeepsOrder) {
  ASSERT_TRUE(fleet.addTag("a"));
  ASSERT_TRUE(fleet.addTag("b"));
  ASSERT_TRUE(fleet.addTag("c"));
  ASSERT_TRUE(fleet.delTag("b"));
  EXPECT_EQ(fleet.getTagCount(), 2);
  EXPECT_STREQ(fleet.getTag(0), "a");
  EXPECT_STREQ(fleet.getTag(1), "c");
  EXPECT_FALSE(fleet.delTag("b"));    // already gone
  EXPECT_FALSE(fleet.delTag("never-set"));
}

TEST_F(FleetTest, TagClear) {
  ASSERT_TRUE(fleet.addTag("a"));
  ASSERT_TRUE(fleet.addTag("b"));
  fleet.clearTags();
  EXPECT_EQ(fleet.getTagCount(), 0);
  fleet.clearTags();   // idempotent
  EXPECT_EQ(fleet.getTagCount(), 0);
}

// ---------------------------------------------------------------- reply window

TEST_F(FleetTest, ReplyWindowBounds) {
  fleet.setReplyWindowMs(90000);
  EXPECT_EQ(fleet.getReplyWindowMs(), (uint32_t)90000);
  fleet.setReplyWindowMs(0);                            // refused
  EXPECT_EQ(fleet.getReplyWindowMs(), (uint32_t)90000);
  fleet.setReplyWindowMs(FLEET_REPLY_WINDOW_MAX_MS + 1); // refused
  EXPECT_EQ(fleet.getReplyWindowMs(), (uint32_t)90000);
  fleet.setReplyWindowMs(FLEET_REPLY_WINDOW_MAX_MS);     // at the cap: accepted
  EXPECT_EQ(fleet.getReplyWindowMs(), (uint32_t)FLEET_REPLY_WINDOW_MAX_MS);
}

// ---------------------------------------------------------------- persistence

TEST_F(FleetTest, SaveOnlyWhenDirtyAndDue) {
  ASSERT_TRUE(fleet.setChannel(psk32hex().c_str()));
  fleet.loop(&fs);   // dirty but not due yet
  EXPECT_FALSE(fs.exists("/fleet_cfg"));
  flushSave();
  EXPECT_TRUE(fs.exists("/fleet_cfg"));
}

TEST_F(FleetTest, NotSavedWhenNotDirty) {
  flushSave();
  EXPECT_FALSE(fs.exists("/fleet_cfg"));
}

TEST_F(FleetTest, Roundtrip) {
  ASSERT_TRUE(fleet.setChannel(psk32hex().c_str()));
  ASSERT_TRUE(fleet.addTag("xiao"));
  ASSERT_TRUE(fleet.addTag("siteA"));
  fleet.setEnabled(true);
  fleet.setReplyWindowMs(120000);
  flushSave();

  FleetManager loaded;
  loaded.begin(&fs);
  EXPECT_TRUE(loaded.isEnabled());
  EXPECT_TRUE(loaded.hasChannel());
  EXPECT_EQ(loaded.getChanSecretLen(), 32);
  uint8_t expected[32];
  cliDecodeHex(psk32hex().c_str(), 64, expected, sizeof(expected));
  EXPECT_EQ(memcmp(loaded.getChanSecret(), expected, 32), 0);
  EXPECT_EQ(loaded.getChanHash(), fleet.getChanHash());
  EXPECT_EQ(loaded.getTagCount(), 2);
  EXPECT_STREQ(loaded.getTag(0), "xiao");
  EXPECT_STREQ(loaded.getTag(1), "siteA");
  EXPECT_EQ(loaded.getReplyWindowMs(), (uint32_t)120000);
}

TEST_F(FleetTest, MissingFileLoadsDefaults) {
  FleetManager loaded;
  loaded.begin(&fs);
  EXPECT_FALSE(loaded.isEnabled());
  EXPECT_FALSE(loaded.hasChannel());
  EXPECT_EQ(loaded.getTagCount(), 0);
  EXPECT_EQ(loaded.getReplyWindowMs(), (uint32_t)FLEET_REPLY_WINDOW_MS);
}

// Corrupt one payload byte: the CRC must reject the file, and the manager must
// come back empty rather than partially trusting the record.
TEST_F(FleetTest, CorruptedPayloadRejectedByCrc) {
  ASSERT_TRUE(fleet.setChannel(psk32hex().c_str()));
  ASSERT_TRUE(fleet.addTag("xiao"));
  fleet.setEnabled(true);
  flushSave();

  fs.files["/fleet_cfg"][50] ^= 0x40;   // flip a bit inside the tag array
  FleetManager loaded;
  loaded.begin(&fs);
  EXPECT_FALSE(loaded.isEnabled());
  EXPECT_FALSE(loaded.hasChannel());
  EXPECT_EQ(loaded.getTagCount(), 0);
}

TEST_F(FleetTest, CorruptedCrcRejected) {
  ASSERT_TRUE(fleet.setChannel(psk32hex().c_str()));
  flushSave();
  fs.files["/fleet_cfg"][FLEET_CFG_RECORD_BYTES - 1] ^= 0x01;   // CRC byte
  FleetManager loaded;
  loaded.begin(&fs);
  EXPECT_FALSE(loaded.hasChannel());
}

TEST_F(FleetTest, TruncatedFileRejected) {
  ASSERT_TRUE(fleet.setChannel(psk32hex().c_str()));
  flushSave();
  fs.files["/fleet_cfg"].resize(64);   // well short of the 182-byte record
  FleetManager loaded;
  loaded.begin(&fs);
  EXPECT_FALSE(loaded.hasChannel());
  EXPECT_EQ(loaded.getTagCount(), 0);
}

TEST_F(FleetTest, BadMagicRejected) {
  ASSERT_TRUE(fleet.setChannel(psk32hex().c_str()));
  flushSave();
  memcpy(&fs.files["/fleet_cfg"][0], "XXXX", 4);
  FleetManager loaded;
  loaded.begin(&fs);
  EXPECT_FALSE(loaded.hasChannel());
}

TEST_F(FleetTest, BadVersionRejected) {
  ASSERT_TRUE(fleet.setChannel(psk32hex().c_str()));
  flushSave();
  fs.files["/fleet_cfg"][4] = 99;   // version byte
  FleetManager loaded;
  loaded.begin(&fs);
  EXPECT_FALSE(loaded.hasChannel());
}

TEST_F(FleetTest, OutOfRangeTagCountClamps) {
  // hand-craft a record whose tag_count exceeds the cap: the CRC must be
  // recomputed over the patched payload so the clamp path is what's tested
  ASSERT_TRUE(fleet.setChannel(psk32hex().c_str()));
  ASSERT_TRUE(fleet.addTag("xiao"));
  flushSave();
  std::vector<uint8_t> rec = fs.files["/fleet_cfg"];
  ASSERT_EQ(rec.size(), (size_t)FLEET_CFG_RECORD_BYTES);
  rec[39] = 200;   // tag_count field
  uint16_t crc = crc16_ccitt(rec.data(), FLEET_CFG_PAYLOAD_BYTES);
  memcpy(&rec[FLEET_CFG_PAYLOAD_BYTES], &crc, 2);
  fs.files["/fleet_cfg"] = rec;

  FleetManager loaded;
  loaded.begin(&fs);
  EXPECT_TRUE(loaded.hasChannel());   // rest of the record adopted
  EXPECT_EQ(loaded.getTagCount(), 1); // clamped to the valid prefix
  EXPECT_STREQ(loaded.getTag(0), "xiao");
}

TEST_F(FleetTest, BackupRecoveredAndRepaired) {
  ASSERT_TRUE(fleet.setChannel(psk32hex().c_str()));
  ASSERT_TRUE(fleet.addTag("xiao"));
  flushSave();   // canonical exists; promote it to .bak via a second save
  ASSERT_TRUE(fleet.addTag("siteA"));
  flushSave();   // now canonical is the 2-tag version, .bak the 1-tag one

  // corrupt the canonical: load must recover from the backup and schedule a
  // repair save
  fs.files["/fleet_cfg"][40] ^= 0xFF;
  FleetManager loaded;
  loaded.begin(&fs);
  EXPECT_TRUE(loaded.hasChannel());
  EXPECT_EQ(loaded.getTagCount(), 1);
  EXPECT_STREQ(loaded.getTag(0), "xiao");

  // the repair save lands once the lazy delay passes: before that, loop() must
  // leave the corrupted canonical untouched
  std::vector<uint8_t> before = fs.files["/fleet_cfg"];
  loaded.loop(&fs);
  EXPECT_EQ(fs.files["/fleet_cfg"], before) << "repair save must wait out the lazy delay";
  g_mock_millis += LAZY_SAVE_DELAY_MS + 1;
  loaded.loop(&fs);
  ASSERT_NE(fs.files["/fleet_cfg"], before);

  // the repaired canonical is loadable again
  FleetManager reloaded;
  reloaded.begin(&fs);
  EXPECT_EQ(reloaded.getTagCount(), 1);
  EXPECT_STREQ(reloaded.getTag(0), "xiao");
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}