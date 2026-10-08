// Native tests for the simple_repeater fleet manager (FleetManager): the
// fleet channel config, the tag set and /fleet_cfg persistence. Reach the
// feature through its public API and fleetCLI() exactly as MyMesh would.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <Arduino.h>        // g_mock_millis (NativeShim.h is force-included)
#include "CliScript.h"
#include <helpers/TxtDataHelpers.h>   // TXT_TYPE_PLAIN
#include "FleetManager.h"
#include "CliUtil.h"

// a 32-byte PSK whose bytes are 0x00..0x1F, as hex (shared by all fixtures)
static std::string psk32hex() {
  return "000102030405060708090A0B0C0D0E0F"
         "101112131415161718191A1B1C1D1E1F";
}

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

// ---------------------------------------------------------------- runner
//
// The CliScriptRunner is reached directly here, exactly as FleetManager drives
// it: parse() for targeting decisions, enqueue() for the queue, run() once per
// loop pass.

// recording exec callback, in the shape of MyMesh's execCliLine trampoline
struct ExecRecorder {
  std::vector<std::string> lines;
  bool fail_next = false;

  static void fn(void* ctx, const char* key, char* line, char* reply) {
    auto* r = (ExecRecorder*)ctx;
    r->lines.push_back(line);
    if (r->fail_next) {
      strcpy(reply, "Err - injected failure");
      r->fail_next = false;
    } else {
      snprintf(reply, CLI_REPLY_MAX, "OK - done: %s", line);
    }
  }
};

struct RunnerTest : public ::testing::Test {
  CliScriptRunner runner;
  ExecRecorder rec;

  void TearDown() override { g_mock_millis = 0; }

  // run until the queue stops making progress
  void drain() {
    for (int i = 0; i < 10; i++) {
      if (!runner.run(ExecRecorder::fn, &rec)) break;
    }
  }
  CliRunResult finish() {
    CliRunResult out;
    memset(&out, 0, sizeof(out));
    runner.run(ExecRecorder::fn, &rec, &out);
    return out;
  }
};

TEST_F(RunnerTest, ParseRequiresIdMarker) {
  CliScriptMeta meta;
  EXPECT_EQ(CliScriptRunner::parse("set radio 869.650", &meta), CLI_ENQUEUE_NO_ID);
  EXPECT_EQ(CliScriptRunner::parse("", &meta), CLI_ENQUEUE_NO_ID);
}

TEST_F(RunnerTest, ParseKeyCharsetAndLength) {
  CliScriptMeta meta;
  EXPECT_EQ(CliScriptRunner::parse("!id ok.Key-1_x\nset name x", &meta), CLI_ENQUEUE_OK);
  EXPECT_STREQ(meta.key, "ok.Key-1_x");
  EXPECT_EQ(CliScriptRunner::parse("!id has space\nset", &meta), CLI_ENQUEUE_BAD_KEY);
  EXPECT_EQ(CliScriptRunner::parse("!id\nset", &meta), CLI_ENQUEUE_NO_ID);   // no "!id " marker
  EXPECT_EQ(CliScriptRunner::parse("!id 0123456789012345678901234567890123\nset", &meta),
            CLI_ENQUEUE_BAD_KEY);   // 33 chars
}

TEST_F(RunnerTest, ParseCrlfAndComments) {
  CliScriptMeta meta;
  const char* script = "!id k1\r\n# a comment\r\n\r\nset name x\r\n";
  EXPECT_EQ(CliScriptRunner::parse(script, &meta), CLI_ENQUEUE_OK);
  EXPECT_EQ(meta.body_off, 23);   // "set name x" starts after the CRLFs/comment
}

TEST_F(RunnerTest, ParseUnknownDirectiveRejects) {
  CliScriptMeta meta;
  EXPECT_EQ(CliScriptRunner::parse("!id k1\n!future stuff\nset", &meta),
            CLI_ENQUEUE_BAD_DIRECTIVE);
}

TEST_F(RunnerTest, ParseDirectiveBlockEachAtMostOnceEitherOrder) {
  CliScriptMeta meta;
  EXPECT_EQ(CliScriptRunner::parse("!id k1\n!ack err\n!tags a,b\nset", &meta), CLI_ENQUEUE_OK);
  EXPECT_EQ(meta.ack, CLI_ACK_ERR);
  EXPECT_EQ(meta.tag_count, 2);
  EXPECT_EQ(CliScriptRunner::parse("!id k1\n!tags a\n!ack\nset", &meta), CLI_ENQUEUE_OK);
  EXPECT_EQ(meta.ack, CLI_ACK_ALWAYS);
  // repeats reject
  EXPECT_EQ(CliScriptRunner::parse("!id k1\n!tags a\n!tags b\nset", &meta),
            CLI_ENQUEUE_BAD_DIRECTIVE);
  EXPECT_EQ(CliScriptRunner::parse("!id k1\n!ack\n!ack err\nset", &meta),
            CLI_ENQUEUE_BAD_DIRECTIVE);
  EXPECT_EQ(CliScriptRunner::parse("!id k1\n!at 1781268000\n!at 1781268001\nset", &meta),
            CLI_ENQUEUE_BAD_DIRECTIVE);
}

TEST_F(RunnerTest, ParseBlockLinesOnlyKnownDirectives) {
  CliScriptMeta meta;
  // !delay is a body directive, not a block directive
  EXPECT_EQ(CliScriptRunner::parse("!id k1\n!delay 100\nset", &meta),
            CLI_ENQUEUE_BAD_DIRECTIVE);
  // an unknown block line rejects, even before a known one
  EXPECT_EQ(CliScriptRunner::parse("!id k1\n!ack\n!what\nset", &meta),
            CLI_ENQUEUE_BAD_DIRECTIVE);
}

TEST_F(RunnerTest, ParseTagsAfterCommandsRejects) {
  CliScriptMeta meta;
  EXPECT_EQ(CliScriptRunner::parse("!id k1\nset name x\n!tags a", &meta),
            CLI_ENQUEUE_BAD_DIRECTIVE);
}

TEST_F(RunnerTest, ParseTagsListValidation) {
  CliScriptMeta meta;
  // absent = broadcast
  EXPECT_EQ(CliScriptRunner::parse("!id k1\nset", &meta), CLI_ENQUEUE_OK);
  EXPECT_EQ(meta.tag_count, 0);
  // valid list
  EXPECT_EQ(CliScriptRunner::parse("!id k1\n!tags a.b-c_d,xyz\nset", &meta), CLI_ENQUEUE_OK);
  EXPECT_EQ(meta.tag_count, 2);
  EXPECT_STREQ(meta.tags[0], "a.b-c_d");
  // exactly 8 tags fit
  EXPECT_EQ(CliScriptRunner::parse(
      "!id k1\n!tags t1,t2,t3,t4,t5,t6,t7,t8\nset", &meta), CLI_ENQUEUE_OK);
  EXPECT_EQ(meta.tag_count, 8);
  EXPECT_EQ(CliScriptRunner::parse(
      "!id k1\n!tags t1,t2,t3,t4,t5,t6,t7,t8,t9\nset", &meta), CLI_ENQUEUE_BAD_DIRECTIVE);
  // charset/length, exact list (no whitespace honored)
  EXPECT_EQ(CliScriptRunner::parse("!id k1\n!tags xiao, siteB\nset", &meta),
            CLI_ENQUEUE_BAD_DIRECTIVE);   // leading space becomes part of the tag
  EXPECT_EQ(CliScriptRunner::parse("!id k1\n!tags a,,b\nset", &meta),
            CLI_ENQUEUE_BAD_DIRECTIVE);
  EXPECT_EQ(CliScriptRunner::parse("!id k1\n!tags 0123456789abcdefg\nset", &meta),
            CLI_ENQUEUE_BAD_DIRECTIVE);   // 17 chars
  EXPECT_EQ(CliScriptRunner::parse("!id k1\n!tags no*wild\nset", &meta),
            CLI_ENQUEUE_BAD_DIRECTIVE);
  EXPECT_EQ(CliScriptRunner::parse("!id k1\n!tags\nset", &meta), CLI_ENQUEUE_BAD_DIRECTIVE);
}

TEST_F(RunnerTest, ParseAtValidation) {
  CliScriptMeta meta;
  EXPECT_EQ(CliScriptRunner::parse("!id k1\n!at 1781268000\nreset", &meta), CLI_ENQUEUE_OK);
  EXPECT_EQ(meta.due_epoch, (uint32_t)1781268000);
  // not 10 digits, or not all digits, or above the RTC range
  EXPECT_EQ(CliScriptRunner::parse("!id k1\n!at 178126800\nreset", &meta),
            CLI_ENQUEUE_BAD_DIRECTIVE);
  EXPECT_EQ(CliScriptRunner::parse("!id k1\n!at 17812680000\nreset", &meta),
            CLI_ENQUEUE_BAD_DIRECTIVE);
  EXPECT_EQ(CliScriptRunner::parse("!id k1\n!at 17812o8000\nreset", &meta),
            CLI_ENQUEUE_BAD_DIRECTIVE);
  EXPECT_EQ(CliScriptRunner::parse("!id k1\n!at 9999999999\nreset", &meta),
            CLI_ENQUEUE_BAD_DIRECTIVE);   // above uint32 epoch range
  EXPECT_EQ(CliScriptRunner::parse("!id k1\n!at\nreset", &meta), CLI_ENQUEUE_BAD_DIRECTIVE);
}

TEST_F(RunnerTest, ParseDelayValidation) {
  CliScriptMeta meta;
  EXPECT_EQ(CliScriptRunner::parse("!id k1\nset a\n!delay 5000\nreset", &meta), CLI_ENQUEUE_OK);
  EXPECT_EQ(CliScriptRunner::parse("!id k1\nset a\n!delay 0\nreset", &meta),
            CLI_ENQUEUE_BAD_DIRECTIVE);
  EXPECT_EQ(CliScriptRunner::parse("!id k1\nset a\n!delay 300001\nreset", &meta),
            CLI_ENQUEUE_BAD_DIRECTIVE);
  EXPECT_EQ(CliScriptRunner::parse("!id k1\nset a\n!delay\nreset", &meta),
            CLI_ENQUEUE_BAD_DIRECTIVE);
  EXPECT_EQ(CliScriptRunner::parse("!id k1\nset a\n!delay 5000 6000\nreset", &meta),
            CLI_ENQUEUE_BAD_DIRECTIVE);
  // any number of times, each after a command (!delay is not a block
  // directive: a block line that is none of tags/ack/at rejects)
  EXPECT_EQ(CliScriptRunner::parse("!id k1\nset a\n!delay 10\nset b\n!delay 20\nset c\n!delay 30", &meta),
            CLI_ENQUEUE_OK);
}

TEST_F(RunnerTest, ParseIsPureNoStateNoCounters) {
  CliScriptMeta meta;
  EXPECT_EQ(CliScriptRunner::parse("chat message, not a script", &meta), CLI_ENQUEUE_NO_ID);
  EXPECT_EQ(CliScriptRunner::parse("!id k1\n!bogus\nset", &meta), CLI_ENQUEUE_BAD_DIRECTIVE);
  EXPECT_EQ(runner.getNoId(), (uint32_t)0);
  EXPECT_EQ(runner.getRefused(), (uint32_t)0);
  EXPECT_EQ(runner.getDup(), (uint32_t)0);
  EXPECT_EQ(runner.getPendingCount(), 0);
  EXPECT_FALSE(runner.keySeen("k1"));
}

TEST_F(RunnerTest, EnqueueThenRunExecutesInOrder) {
  ASSERT_EQ(runner.enqueue("!id k1\nset a\nset b\nset c"), CLI_ENQUEUE_OK);
  drain();
  ASSERT_EQ(rec.lines.size(), (size_t)3);
  EXPECT_EQ(rec.lines[0], "set a");
  EXPECT_EQ(rec.lines[2], "set c");
  EXPECT_EQ(runner.getRan(), (uint32_t)1);
  EXPECT_EQ(runner.getPendingCount(), 0);
}

TEST_F(RunnerTest, EnqueueRefusalsCountedAndNotMarked) {
  EXPECT_EQ(runner.enqueue("no marker"), CLI_ENQUEUE_NO_ID);
  EXPECT_EQ(runner.getNoId(), (uint32_t)1);
  EXPECT_EQ(runner.enqueue("!id bad key"), CLI_ENQUEUE_BAD_KEY);
  EXPECT_EQ(runner.getRefused(), (uint32_t)1);
  // refused scripts are not marked: a re-send still reaches them
  ASSERT_EQ(runner.enqueue("!id k1\nset a"), CLI_ENQUEUE_OK);
  ASSERT_EQ(runner.enqueue("!id k1\nset a"), CLI_ENQUEUE_DUP);
  EXPECT_EQ(runner.getDup(), (uint32_t)1);
}

TEST_F(RunnerTest, QueueFullRefusesWithoutMarking) {
  ASSERT_EQ(runner.enqueue("!id k1\nset a\n!delay 1000"), CLI_ENQUEUE_OK);
  ASSERT_EQ(runner.enqueue("!id k2\nset a\n!delay 1000"), CLI_ENQUEUE_OK);
  EXPECT_EQ(runner.enqueue("!id k3\nset a"), CLI_ENQUEUE_FULL);
  EXPECT_EQ(runner.getRefused(), (uint32_t)1);
  EXPECT_FALSE(runner.keySeen("k3"));
}

TEST_F(RunnerTest, ForgetRearmsKeyAndRebootResets) {
  ASSERT_EQ(runner.enqueue("!id k1\nset a"), CLI_ENQUEUE_OK);
  drain();
  ASSERT_TRUE(runner.forgetKey("k1"));
  EXPECT_EQ(runner.enqueue("!id k1\nset a"), CLI_ENQUEUE_OK);   // runs again
  drain();
  runner.forgetAll();
  ASSERT_EQ(runner.enqueue("!id k2\nset a"), CLI_ENQUEUE_OK);
  runner.reset();   // reboot: queue, seen ring and counters all reset
  EXPECT_EQ(runner.getPendingCount(), 0);
  EXPECT_FALSE(runner.keySeen("k2"));
  EXPECT_EQ(runner.getRan(), (uint32_t)0);
}

TEST_F(RunnerTest, SeenRingEvictsOldest) {
  char script[64];
  for (int i = 0; i < FLEET_CLI_SEEN_SIZE + 1; i++) {
    snprintf(script, sizeof(script), "!id k%d\nset a", i);
    ASSERT_EQ(runner.enqueue(script), CLI_ENQUEUE_OK);
    drain();   // free the queue slot; the seen key stays marked
  }
  EXPECT_EQ(runner.getSeenCount(), FLEET_CLI_SEEN_SIZE);
  // k0 was evicted: it can be enqueued again
  EXPECT_EQ(runner.enqueue("!id k0\nset a"), CLI_ENQUEUE_OK);
}

TEST_F(RunnerTest, SummarySingleCommandIsItsReply) {
  ASSERT_EQ(runner.enqueue("!id k1\nfilter stats"), CLI_ENQUEUE_OK);
  CliRunResult out = finish();
  EXPECT_STREQ(out.summary, "OK - done: filter stats");
  EXPECT_FALSE(out.error);
  EXPECT_STREQ(out.key, "k1");
}

TEST_F(RunnerTest, SummaryMultiOk) {
  ASSERT_EQ(runner.enqueue("!id k1\nset a\nset b\nset c"), CLI_ENQUEUE_OK);
  CliRunResult out = finish();
  EXPECT_STREQ(out.summary, "ran 3 ok");
  EXPECT_FALSE(out.error);
}

TEST_F(RunnerTest, SummaryFirstError) {
  ASSERT_EQ(runner.enqueue("!id k1\nset a\nset b\nset c"), CLI_ENQUEUE_OK);
  rec.fail_next = true;   // "set a" fails
  CliRunResult out = finish();
  EXPECT_STREQ(out.summary, "ran 3; err: Err - injected failure");
  EXPECT_TRUE(out.error);
}

TEST_F(RunnerTest, AckModeCarriedToResult) {
  ASSERT_EQ(runner.enqueue("!id k1\n!ack\nset a"), CLI_ENQUEUE_OK);
  EXPECT_EQ(finish().ack, CLI_ACK_ALWAYS);
  ASSERT_EQ(runner.enqueue("!id k2\n!ack err\nset a"), CLI_ENQUEUE_OK);
  EXPECT_EQ(finish().ack, CLI_ACK_ERR);
  ASSERT_EQ(runner.enqueue("!id k3\nset a"), CLI_ENQUEUE_OK);
  EXPECT_EQ(finish().ack, CLI_ACK_NONE);
}

TEST_F(RunnerTest, ReplyNeverMatchesIdMarker) {
  // a reply is group text on the fleet channel, so every other member parses
  // it: the summary is built from CLI replies, which start with OK/Err —
  // never '!' — and can therefore never look like a script
  ASSERT_EQ(runner.enqueue("!id k1\nfilter stats"), CLI_ENQUEUE_OK);
  CliRunResult out = finish();
  EXPECT_NE(out.summary[0], '!');
}

TEST_F(RunnerTest, DelayResumesAfterDeadline) {
  g_mock_millis = 1000;
  ASSERT_EQ(runner.enqueue("!id k1\nset a\n!delay 5000\nset b"), CLI_ENQUEUE_OK);

  // first pass: "set a" runs, the delay arms, the script keeps its slot
  CliRunResult out;
  memset(&out, 0, sizeof(out));
  ASSERT_TRUE(runner.run(ExecRecorder::fn, &rec, &out));
  EXPECT_TRUE(runner.getPendingCount() == 1);
  ASSERT_EQ(rec.lines.size(), (size_t)1);
  EXPECT_EQ(rec.lines[0], "set a");
  EXPECT_EQ(runner.getRan(), (uint32_t)0);   // not finished yet

  // deadline not passed: no progress
  EXPECT_FALSE(runner.run(ExecRecorder::fn, &rec));

  // after the deadline: "set b" runs and the script finishes
  g_mock_millis += 5000;
  out = finish();
  ASSERT_EQ(rec.lines.size(), (size_t)2);
  EXPECT_EQ(rec.lines[1], "set b");
  EXPECT_STREQ(out.summary, "ran 2 ok");
  EXPECT_EQ(runner.getRan(), (uint32_t)1);
}

TEST_F(RunnerTest, DirectivesNeverExecute) {
  ASSERT_EQ(runner.enqueue("!id k1\n!tags x\n!ack\nset a"), CLI_ENQUEUE_OK);
  drain();
  ASSERT_EQ(rec.lines.size(), (size_t)1);
  EXPECT_EQ(rec.lines[0], "set a");
}

TEST_F(RunnerTest, DelayAnywhereAnyNumber) {
  ASSERT_EQ(runner.enqueue("!id k1\nset a\n!delay 10\nset b\n!delay 20\nset c\n!delay 30"),
            CLI_ENQUEUE_OK);
  g_mock_millis = 0;
  // each pass executes up to the next delay
  ASSERT_TRUE(runner.run(ExecRecorder::fn, &rec));
  ASSERT_EQ(rec.lines.size(), (size_t)1);
  g_mock_millis += 10;
  ASSERT_TRUE(runner.run(ExecRecorder::fn, &rec));
  ASSERT_EQ(rec.lines.size(), (size_t)2);
  g_mock_millis += 20;
  ASSERT_TRUE(runner.run(ExecRecorder::fn, &rec));
  ASSERT_EQ(rec.lines.size(), (size_t)3);
  g_mock_millis += 30;
  CliRunResult out = finish();
  EXPECT_EQ(rec.lines.size(), (size_t)3);   // nothing after the last delay
  EXPECT_STREQ(out.summary, "ran 3 ok");
}

TEST_F(RunnerTest, TrailingDelayStillSleepsThenFinishes) {
  ASSERT_EQ(runner.enqueue("!id k1\nset a\n!delay 5000"), CLI_ENQUEUE_OK);
  ASSERT_TRUE(runner.run(ExecRecorder::fn, &rec));   // runs "set a", arms the delay
  EXPECT_EQ(runner.getPendingCount(), 1);
  EXPECT_FALSE(runner.run(ExecRecorder::fn, &rec));
  g_mock_millis += 5000;
  CliRunResult out = finish();
  // single-command script: the actual reply, even across a trailing delay
  EXPECT_STREQ(out.summary, "OK - done: set a");
  // ...and the same rule for a query-style job
  ASSERT_EQ(runner.enqueue("!id k2\nfilter stats\n!delay 100"), CLI_ENQUEUE_OK);
  ASSERT_TRUE(runner.run(ExecRecorder::fn, &rec));
  g_mock_millis += 100;
  out = finish();
  EXPECT_STREQ(out.summary, "OK - done: filter stats");
}

TEST_F(RunnerTest, SleepingScriptsHoldTheirSlots) {
  ASSERT_EQ(runner.enqueue("!id k1\nset a\n!delay 60000"), CLI_ENQUEUE_OK);
  ASSERT_EQ(runner.enqueue("!id k2\nset a\n!delay 60000"), CLI_ENQUEUE_OK);
  // first pass: k1 runs its command and arms its delay; k2 waits (strict FIFO,
  // a sleeping head blocks the queue)
  ASSERT_TRUE(runner.run(ExecRecorder::fn, &rec));
  ASSERT_EQ(rec.lines.size(), (size_t)1);
  EXPECT_EQ(runner.getPendingCount(), 2);
  EXPECT_EQ(runner.enqueue("!id k3\nset a"), CLI_ENQUEUE_FULL);
  EXPECT_FALSE(runner.keySeen("k3"));   // refused, not marked
  // the head sleeper is resumed first
  g_mock_millis += 60000;
  CliRunResult out;
  memset(&out, 0, sizeof(out));
  ASSERT_TRUE(runner.run(ExecRecorder::fn, &rec, &out));
  EXPECT_STREQ(out.key, "k1");
  EXPECT_EQ(runner.getPendingCount(), 1);   // k2 kept its slot
}

TEST_F(RunnerTest, OverlongLineSkippedRunContinues) {
  char script[MAX_PACKET_PAYLOAD + 1];
  int n = snprintf(script, sizeof(script), "!id k1\n%0*d\nset a", FLEET_CLI_LINE_MAX + 10, 7);
  ASSERT_GT(n, 0);
  ASSERT_EQ(runner.enqueue(script), CLI_ENQUEUE_OK);
  drain();
  ASSERT_EQ(rec.lines.size(), (size_t)1);
  EXPECT_EQ(rec.lines[0], "set a");
  EXPECT_EQ(runner.getBadLines(), (uint32_t)1);
}

TEST_F(RunnerTest, MarkKeySeenAndEnqueueValidated) {
  // the scheduled-script path: mark at admission, queue later without marking
  EXPECT_TRUE(runner.markKeySeen("k1"));
  EXPECT_TRUE(runner.keySeen("k1"));
  EXPECT_FALSE(runner.markKeySeen("k1"));   // duplicate
  EXPECT_EQ(runner.getDup(), (uint32_t)0);  // the caller counts, not the runner
  CliScriptMeta meta;
  ASSERT_EQ(CliScriptRunner::parse("!id k1\n!ack\nset a", &meta), CLI_ENQUEUE_OK);
  ASSERT_TRUE(runner.enqueueValidated("!id k1\n!ack\nset a", meta.key, meta.ack,
                                      meta.body_off, NULL, 0));
  EXPECT_EQ(runner.getPendingCount(), 1);
  CliRunResult out = finish();
  EXPECT_STREQ(out.summary, "OK - done: set a");   // single command: its actual reply
  EXPECT_EQ(out.ack, CLI_ACK_ALWAYS);
  // execution starts at the command body: directives never reach the callback
  ASSERT_EQ(rec.lines.size(), (size_t)1);
}

// Run a fleet CLI command and return the reply (canary-checked, like the
// filter suite's helper).
static std::string cli(FleetManager& fleet, const char* command) {
  static const uint8_t CANARY = 0xA5;
  uint8_t raw[CLI_REPLY_MAX + 1];
  memset(raw, CANARY, sizeof(raw));
  char* reply = (char*)raw;
  reply[0] = 0;
  fleetCLI(fleet, command, reply);
  EXPECT_EQ(raw[CLI_REPLY_MAX], CANARY) << "reply overran " << CLI_REPLY_MAX << " bytes: " << command;
  EXPECT_NE(memchr(reply, 0, CLI_REPLY_MAX), nullptr)
      << "reply not NUL-terminated in " << CLI_REPLY_MAX << " bytes: " << command;
  return std::string(reply);
}

// ------------------------------------------------------------------ hooks
//
// FleetManager::onGroupData + runScripts + fleetCLI, driven exactly as
// MyMesh drives them.

// GRP_TXT decrypted payload: ts(4) | txt_type(1) | "<sender>: <text>" (same
// shape the packet-filter suite builds)
struct GroupTextPayload {
  uint8_t data[MAX_PACKET_PAYLOAD];
  size_t len;
};

static GroupTextPayload makeGroupText(const char* sender, const char* text) {
  GroupTextPayload p;
  memset(&p, 0, sizeof(p));
  uint32_t ts = 12345;
  memcpy(p.data, &ts, 4);
  p.data[4] = TXT_TYPE_PLAIN;
  size_t off = 5;
  size_t slen = strlen(sender);
  memcpy(&p.data[off], sender, slen);
  off += slen;
  p.data[off++] = ':';
  if (text[0]) p.data[off++] = ' ';
  size_t tlen = strlen(text);
  memcpy(&p.data[off], text, tlen);
  off += tlen;
  p.len = off;
  return p;
}

// The packet the core hands onGroupDataRecv(): the payload is the channel hash
// followed by the bytes (the native crypto mocks cannot MAC, so the plaintext
// stands in for ciphertext; the hook only sees it through the packet hash).
static mesh::Packet makeGroupPacket(uint8_t type, uint8_t hash, const GroupTextPayload& p) {
  mesh::Packet pkt;
  memset(&pkt, 0, sizeof(pkt));
  pkt.header = type << PH_TYPE_SHIFT;
  size_t n = p.len < MAX_PACKET_PAYLOAD - 1 ? p.len : MAX_PACKET_PAYLOAD - 1;
  pkt.payload[0] = hash;
  memcpy(&pkt.payload[1], p.data, n);
  pkt.payload_len = (uint16_t)(1 + n);
  return pkt;
}

// deliver a decrypted group message to the hook the way MyMesh does
static void groupRecv(FleetManager& f, uint8_t type, const mesh::GroupChannel& c,
                      const GroupTextPayload& p) {
  mesh::Packet pkt = makeGroupPacket(type, c.hash[0], p);
  f.onGroupData(&pkt, type, c, p.data, p.len);
}

// what the send callback saw
struct SentReply {
  uint8_t secret[PUB_KEY_SIZE];
  uint8_t hash;
  std::string body;
};

// injected device services, recording what they were given
struct HookEnv {
  std::vector<SentReply> sent;
  uint32_t clock_now = 1800000000UL;   // a fixed epoch above the clock floor
  uint32_t jitter = 12345;
  uint32_t last_window = 0;
  ExecRecorder rec;

  static uint32_t timeFn(void* ctx) { return ((HookEnv*)ctx)->clock_now; }
  static uint32_t jitterFn(void* ctx, uint32_t window) {
    auto* e = (HookEnv*)ctx;
    e->last_window = window;
    return e->jitter;
  }
  static void sendFn(void* ctx, const uint8_t* secret, uint8_t hash, const char* body) {
    auto* e = (HookEnv*)ctx;
    SentReply r;
    r.hash = hash;
    r.body = body;
    memcpy(r.secret, secret, PUB_KEY_SIZE);
    e->sent.push_back(r);
  }
};

struct FleetHooksTest : public ::testing::Test {
  NativeFS fs;
  FleetManager fleet;
  HookEnv env;
  mesh::GroupChannel chan;   // the fleet channel as core would deliver it

  void SetUp() override {
    g_mock_millis = 1000;
    ASSERT_TRUE(fleet.setChannel(psk32hex().c_str()));
    ASSERT_TRUE(fleet.addTag("xiao"));
    fleet.setEnabled(true);
    memset(&chan, 0, sizeof(chan));
    uint8_t secret[32];
    cliDecodeHex(psk32hex().c_str(), 64, secret, sizeof(secret));
    memcpy(chan.secret, secret, sizeof(chan.secret));
    chan.hash[0] = fleet.getChanHash();
    fleet.setClock(HookEnv::timeFn, &env);
    fleet.setJitter(HookEnv::jitterFn, &env);
    fleet.setSender(HookEnv::sendFn, &env);
  }
  void TearDown() override { g_mock_millis = 0; }

  void deliver(const char* text) {
    GroupTextPayload p = makeGroupText("alice", text);
    groupRecv(fleet, PAYLOAD_TYPE_GRP_TXT, chan, p);
  }
  // one runScripts pass, mirroring MyMesh::loop()
  bool pass() { fleet.runScripts(ExecRecorder::fn, &env.rec); return env.rec.lines.size() > 0; }
  void flushSave() {
    g_mock_millis += LAZY_SAVE_DELAY_MS + 1;
    fleet.loop(&fs);
  }
};

TEST_F(FleetHooksTest, HookInertWhenDisabledOrChannelless) {
  fleet.setEnabled(false);
  deliver("!id k1\nset a");
  EXPECT_EQ(fleet.getOffered(), (uint32_t)0);
  fleet.setEnabled(true);
  fleet.clearChannel();
  deliver("!id k1\nset a");
  EXPECT_EQ(fleet.getOffered(), (uint32_t)0);
  EXPECT_EQ(fleet.getTagCount(), 1);   // config preserved while idle
}

TEST_F(FleetHooksTest, OtherChannelAndNonTxtIgnored) {
  mesh::GroupChannel other = chan;
  other.secret[0] ^= 0xFF;
  GroupTextPayload p = makeGroupText("alice", "!id k1\nset a");
  groupRecv(fleet, PAYLOAD_TYPE_GRP_TXT, other, p);
  EXPECT_EQ(fleet.getOffered(), (uint32_t)0);
  groupRecv(fleet, PAYLOAD_TYPE_GRP_DATA, chan, p);
  EXPECT_EQ(fleet.getOffered(), (uint32_t)0);
}

TEST_F(FleetHooksTest, ChatIsNotAScript) {
  deliver("hello from alice");
  EXPECT_EQ(fleet.getOffered(), (uint32_t)0);
  EXPECT_EQ(fleet.getScripts().getNoId(), (uint32_t)1);   // counted, never queued
  EXPECT_EQ(fleet.getScripts().getPendingCount(), 0);
}

TEST_F(FleetHooksTest, BroadcastRunsRegardlessOfTags) {
  deliver("!id k1\nset a");
  EXPECT_EQ(fleet.getOffered(), (uint32_t)1);
  EXPECT_EQ(fleet.getMatched(), (uint32_t)1);
  ASSERT_TRUE(pass());
  ASSERT_EQ(env.rec.lines.size(), (size_t)1);
  EXPECT_EQ(env.rec.lines[0], "set a");
}

TEST_F(FleetHooksTest, TagMatchRunsNoMatchSkipped) {
  deliver("!id k1\n!tags xiao\nset a");
  EXPECT_EQ(fleet.getMatched(), (uint32_t)1);
  ASSERT_TRUE(pass());
  ASSERT_EQ(env.rec.lines.size(), (size_t)1);

  deliver("!id k2\n!tags roof\nset a");
  EXPECT_EQ(fleet.getOffered(), (uint32_t)2);
  EXPECT_EQ(fleet.getMatched(), (uint32_t)1);   // unchanged: no match, no enqueue
  EXPECT_EQ(fleet.getScripts().getPendingCount(), 0);
  EXPECT_FALSE(fleet.getScripts().keySeen("k2"));   // unmarked: a later re-send after
                                                    // `fleet tag add roof` still works
}

TEST_F(FleetHooksTest, ExecutionIsDeferredOutOfTheReceivePath) {
  deliver("!id k1\nset a");
  EXPECT_EQ(env.rec.lines.size(), (size_t)0);   // nothing ran in the hook
  // one drain = one execution
  ASSERT_TRUE(pass());
  ASSERT_TRUE(pass());   // second pass has nothing to do
  EXPECT_EQ(env.rec.lines.size(), (size_t)1);
}

TEST_F(FleetHooksTest, ReplyNeverRuns) {
  // a reply is group text on the fleet channel: it must never parse as a script
  GroupTextPayload p = makeGroupText("xiao", "k1 ran 2 ok");   // sender + key prefix, no '!'
  groupRecv(fleet, PAYLOAD_TYPE_GRP_TXT, chan, p);
  EXPECT_EQ(fleet.getOffered(), (uint32_t)0);
}

TEST_F(FleetHooksTest, AckAlwaysRepliesJitteredInChannel) {
  deliver("!id k1\n!ack\nfilter stats");
  ASSERT_TRUE(pass());   // runs, schedules the ack
  EXPECT_EQ(fleet.getReplyCount(), 1);
  EXPECT_EQ(env.sent.size(), (size_t)0);   // deadline (1000 + 12345) not passed

  g_mock_millis += env.jitter;
  fleet.runScripts(ExecRecorder::fn, &env.rec);   // due: sends the reply
  ASSERT_EQ(env.sent.size(), (size_t)1);
  EXPECT_EQ(env.sent[0].body, "k1\nOK - done: filter stats");
  EXPECT_EQ(env.sent[0].hash, chan.hash[0]);
  EXPECT_EQ(memcmp(env.sent[0].secret, chan.secret, PUB_KEY_SIZE), 0);
  EXPECT_EQ(fleet.getReplyCount(), 0);
  // the jitter window came from the (default) config
  EXPECT_EQ(env.last_window, (uint32_t)FLEET_REPLY_WINDOW_MS);
}

TEST_F(FleetHooksTest, AckErrSilentOnSuccessRepliesOnError) {
  deliver("!id k1\n!ack err\nset a");
  ASSERT_TRUE(pass());   // all OK: silence means success
  EXPECT_EQ(fleet.getReplyCount(), 0);
  g_mock_millis += env.jitter;
  fleet.runScripts(ExecRecorder::fn, &env.rec);
  EXPECT_EQ(env.sent.size(), (size_t)0);

  deliver("!id k2\n!ack err\nset a\nset b");
  env.rec.fail_next = true;   // "set a" fails
  ASSERT_TRUE(pass());
  EXPECT_EQ(fleet.getReplyCount(), 1);
  g_mock_millis += env.jitter;
  fleet.runScripts(ExecRecorder::fn, &env.rec);
  ASSERT_EQ(env.sent.size(), (size_t)1);
  EXPECT_EQ(env.sent[0].body, "k2\nran 2; err: Err - injected failure");
}

TEST_F(FleetHooksTest, NoAckDirectiveStaysSilent) {
  deliver("!id k1\nset a");
  ASSERT_TRUE(pass());
  g_mock_millis += env.jitter;
  fleet.runScripts(ExecRecorder::fn, &env.rec);
  EXPECT_EQ(env.sent.size(), (size_t)0);
}

TEST_F(FleetHooksTest, ReplyGoesToChannelOfArrival) {
  deliver("!id k1\n!ack\nset a");
  ASSERT_TRUE(pass());
  // the config changes before the reply is due: the captured channel is used
  const char* other = "ffffffffffffffffffffffffffffffff";
  ASSERT_TRUE(fleet.setChannel(other));
  g_mock_millis += env.jitter;
  fleet.runScripts(ExecRecorder::fn, &env.rec);
  ASSERT_EQ(env.sent.size(), (size_t)1);
  EXPECT_EQ(memcmp(env.sent[0].secret, chan.secret, PUB_KEY_SIZE), 0);
  EXPECT_EQ(env.sent[0].hash, chan.hash[0]);
}

TEST_F(FleetHooksTest, ReplyStoreFullDropsAndCounts) {
  deliver("!id k1\n!ack\nset a");
  deliver("!id k2\n!ack\nset a");
  ASSERT_TRUE(pass());
  ASSERT_TRUE(pass());
  EXPECT_EQ(fleet.getReplyCount(), FLEET_REPLY_STORE);
  uint32_t refused_before = fleet.getScripts().getRefused();
  deliver("!id k3\n!ack\nset a");
  ASSERT_TRUE(pass());
  EXPECT_EQ(fleet.getReplyCount(), FLEET_REPLY_STORE);   // third ack dropped
  EXPECT_EQ(fleet.getScripts().getRefused(), refused_before + 1);
}

TEST_F(FleetHooksTest, ReplyWindowFromConfig) {
  ASSERT_EQ(cli(fleet, "reply 90"), "OK - reply window 90s");
  deliver("!id k1\n!ack\nset a");
  ASSERT_TRUE(pass());
  EXPECT_EQ(env.last_window, (uint32_t)90000);
}

TEST_F(FleetHooksTest, AtStaleRefused) {
  char script[64];
  snprintf(script, sizeof(script), "!id k1\n!at %u\nreset", (unsigned)(env.clock_now - FLEET_AT_STALE_SECS - 1));
  deliver(script);
  EXPECT_EQ(fleet.getOffered(), (uint32_t)1);
  EXPECT_EQ(fleet.getSchedCount(), 0);
  EXPECT_EQ(fleet.getScripts().getRefused(), (uint32_t)1);
  EXPECT_FALSE(fleet.getScripts().keySeen("k1"));   // refused: not marked
  // within the grace it is admitted (and fires immediately, being already due)
  snprintf(script, sizeof(script), "!id k2\n!at %u\nreset", (unsigned)(env.clock_now - FLEET_AT_STALE_SECS));
  deliver(script);
  EXPECT_EQ(fleet.getSchedCount(), 1);
  ASSERT_TRUE(pass());
  ASSERT_EQ(env.rec.lines.size(), (size_t)1);
}

TEST_F(FleetHooksTest, AtClockFloorRefused) {
  env.clock_now = FLEET_AT_CLOCK_FLOOR - 1;   // RTC unset
  deliver("!id k1\n!at 2000000000\nreset");
  EXPECT_EQ(fleet.getSchedCount(), 0);
  EXPECT_EQ(fleet.getScripts().getRefused(), (uint32_t)1);
}

TEST_F(FleetHooksTest, AtFutureArmedFiresWhenRtcPasses) {
  deliver("!id k1\n!at 1800000600\nreset");   // env.clock_now + 600
  EXPECT_EQ(fleet.getSchedCount(), 1);
  EXPECT_TRUE(fleet.getScripts().keySeen("k1"));   // marked at admission
  EXPECT_EQ(env.rec.lines.size(), (size_t)0);      // not yet
  pass();
  EXPECT_EQ(env.rec.lines.size(), (size_t)0);      // still armed

  env.clock_now = 1800000600;
  ASSERT_TRUE(pass());
  ASSERT_EQ(env.rec.lines.size(), (size_t)1);
  EXPECT_EQ(fleet.getSchedCount(), 0);
}

TEST_F(FleetHooksTest, AtScheduledAckRepliesAfterExecution) {
  deliver("!id k1\n!at 1800000600\n!ack\nfilter stats");
  pass();   // armed only: nothing runs, nothing acks
  EXPECT_EQ(env.rec.lines.size(), (size_t)0);
  EXPECT_EQ(fleet.getReplyCount(), 0);
  env.clock_now = 1800000600;
  ASSERT_TRUE(pass());   // fires, then the ack is scheduled
  EXPECT_EQ(fleet.getReplyCount(), 1);
  g_mock_millis += env.jitter;
  fleet.runScripts(ExecRecorder::fn, &env.rec);   // due: sends the reply
  ASSERT_EQ(env.sent.size(), (size_t)1);
  EXPECT_EQ(env.sent[0].body, "k1\nOK - done: filter stats");
}

TEST_F(FleetHooksTest, AtDupAndStoreFull) {
  deliver("!id k1\n!at 1800000600\nreset");
  deliver("!id k2\n!at 1800000601\nreset");
  EXPECT_EQ(fleet.getSchedCount(), 2);
  // a re-sent job is a no-op, counted as a dup
  uint32_t dup_before = fleet.getScripts().getDup();
  deliver("!id k1\n!at 1800000600\nreset");
  EXPECT_EQ(fleet.getSchedCount(), 2);
  EXPECT_EQ(fleet.getScripts().getDup(), dup_before + 1);
  // the store is full: a third job is refused, key not marked
  uint32_t refused_before = fleet.getScripts().getRefused();
  deliver("!id k3\n!at 1800000602\nreset");
  EXPECT_EQ(fleet.getSchedCount(), 2);
  EXPECT_EQ(fleet.getScripts().getRefused(), refused_before + 1);
  EXPECT_FALSE(fleet.getScripts().keySeen("k3"));
}

TEST_F(FleetHooksTest, ScheduledWaitsForRunQueueSpace) {
  deliver("!id s1\nset a\nset c\n!delay 5000");
  deliver("!id s2\nset a");   // queued behind the sleeper
  deliver("!id at1\n!at 1800000600\nset b");
  EXPECT_EQ(fleet.getSchedCount(), 1);
  env.clock_now = 1800000600;
  // the run queue is full: the due script keeps waiting in the store
  ASSERT_TRUE(pass());   // s1 runs its command, arms the delay
  EXPECT_EQ(fleet.getSchedCount(), 1);
  pass();   // s1 sleeping: nothing drains, sched still blocked
  g_mock_millis += 5000;
  ASSERT_TRUE(pass());   // s1 resumes and finishes, freeing its slot
  EXPECT_EQ(fleet.getSchedCount(), 1);   // at1 moves on the next pass, behind s2
  ASSERT_TRUE(pass());   // s2 (queued ahead of the scheduled script) runs
  ASSERT_TRUE(pass());   // at1, now at the queue head, runs
  ASSERT_EQ(env.rec.lines.size(), (size_t)4);   // s1's two lines + s2's + at1's
  EXPECT_EQ(env.rec.lines[3], "set b");
  EXPECT_EQ(fleet.getSchedCount(), 0);
}

TEST_F(FleetHooksTest, ScheduledAndRepliesAreRamOnly) {
  deliver("!id k1\n!at 1800000600\n!ack\nset a");
  ASSERT_TRUE(fleet.addTag("siteA"));
  flushSave();
  FleetManager loaded;
  loaded.begin(&fs);   // a reboot: config loads, armed jobs and acks do not
  EXPECT_TRUE(loaded.isEnabled());
  EXPECT_EQ(loaded.getTagCount(), 2);
  EXPECT_EQ(loaded.getSchedCount(), 0);
  EXPECT_EQ(loaded.getReplyCount(), 0);
  EXPECT_FALSE(loaded.getScripts().keySeen("k1"));   // seen ring fresh too
}

TEST_F(FleetHooksTest, CheckForwardExemptsOnlyTheVerifiedFleetPacket) {
  GroupTextPayload p = makeGroupText("bob", "!id f1\nset a");
  // the core delivers a fleet-channel packet only after its MAC verified, and
  // that packet is the one the battery gate must not see
  mesh::Packet txt = makeGroupPacket(PAYLOAD_TYPE_GRP_TXT, chan.hash[0], p);
  fleet.onGroupData(&txt, PAYLOAD_TYPE_GRP_TXT, chan, p.data, p.len);
  EXPECT_FALSE(fleet.checkForward(&txt));
  // the same content relayed with a longer path is still the same packet
  mesh::Packet relayed = txt;
  relayed.path_len = 3;
  EXPECT_FALSE(fleet.checkForward(&relayed));
  // GRP_DATA verified under the fleet key is exempt the same way
  GroupTextPayload q = makeGroupText("bob", "data");
  mesh::Packet data = makeGroupPacket(PAYLOAD_TYPE_GRP_DATA, chan.hash[0], q);
  fleet.onGroupData(&data, PAYLOAD_TYPE_GRP_DATA, chan, q.data, q.len);
  EXPECT_FALSE(fleet.checkForward(&data));
  // any other group packet, even on the same hash byte, defers to the gate
  GroupTextPayload r = makeGroupText("eve", "hello from another channel");
  mesh::Packet other = makeGroupPacket(PAYLOAD_TYPE_GRP_TXT, chan.hash[0], r);
  EXPECT_TRUE(fleet.checkForward(&other));
  mesh::Packet other_hash = makeGroupPacket(PAYLOAD_TYPE_GRP_TXT, chan.hash[0] ^ 0xFF, p);
  EXPECT_TRUE(fleet.checkForward(&other_hash));
  // everything that is not group traffic defers to the gate ("true = let the
  // battery gate decide")
  mesh::Packet advert = makeGroupPacket(PAYLOAD_TYPE_ADVERT, chan.hash[0], p);
  EXPECT_TRUE(fleet.checkForward(&advert));
  // hooks idle: the gate decides for everything
  fleet.setEnabled(false);
  EXPECT_TRUE(fleet.checkForward(&txt));
  fleet.setEnabled(true);
  fleet.clearChannel();
  EXPECT_TRUE(fleet.checkForward(&txt));
}

// a colliding channel's packet is never exempt, even while a fleet packet is
// the recorded one: its content differs, and its MAC never verified under the fleet key
TEST_F(FleetHooksTest, ForeignChannelWithSameHashKeepsGateApplied) {
  GroupTextPayload p = makeGroupText("bob", "!id f2\nset a");
  mesh::Packet fleet_pkt = makeGroupPacket(PAYLOAD_TYPE_GRP_TXT, chan.hash[0], p);
  fleet.onGroupData(&fleet_pkt, PAYLOAD_TYPE_GRP_TXT, chan, p.data, p.len);
  ASSERT_FALSE(fleet.checkForward(&fleet_pkt));

  mesh::GroupChannel other;
  memset(&other, 0, sizeof(other));
  memset(other.secret, 0x77, sizeof(other.secret));
  other.hash[0] = chan.hash[0];                 // forced collision
  GroupTextPayload o = makeGroupText("eve", "hello");
  mesh::Packet other_pkt = makeGroupPacket(PAYLOAD_TYPE_GRP_TXT, chan.hash[0], o);
  fleet.onGroupData(&other_pkt, PAYLOAD_TYPE_GRP_TXT, other, o.data, o.len);
  EXPECT_TRUE(fleet.checkForward(&other_pkt));  // gate decides, not exempt
  EXPECT_FALSE(fleet.checkForward(&fleet_pkt)); // the fleet packet still is
}

TEST_F(FleetHooksTest, AppendChannelByHash) {
  mesh::GroupChannel dest[4];
  memset(dest, 0, sizeof(dest));
  // hash match: the fleet channel is offered, deduped against the entries the
  // filter store already handed over (the filled prefix)
  uint8_t hash[1] = { chan.hash[0] };
  memcpy(dest[0].secret, chan.secret, PUB_KEY_SIZE);
  EXPECT_EQ(fleet.appendChannelByHash(hash, dest, 4, 1), 0);   // same secret already present
  memset(dest, 0, sizeof(dest));
  EXPECT_EQ(fleet.appendChannelByHash(hash, dest, 4, 0), 1);
  EXPECT_EQ(dest[0].hash[0], chan.hash[0]);
  EXPECT_EQ(memcmp(dest[0].secret, chan.secret, PUB_KEY_SIZE), 0);
  // a different secret with the same hash is appended at dest[filled], after
  // the filter's entries — never over them
  mesh::GroupChannel mixed[4];
  memset(mixed, 0, sizeof(mixed));
  memset(mixed[0].secret, 0xEE, PUB_KEY_SIZE);
  EXPECT_EQ(fleet.appendChannelByHash(hash, mixed, 4, 1), 1);
  EXPECT_EQ(mixed[1].hash[0], chan.hash[0]);
  EXPECT_EQ(memcmp(mixed[1].secret, chan.secret, PUB_KEY_SIZE), 0);
  EXPECT_EQ(mixed[0].secret[0], 0xEE);   // the earlier entry is untouched
  // store full: nothing offered
  EXPECT_EQ(fleet.appendChannelByHash(hash, mixed, 4, 4), 0);
  // no hash match, or hooks idle: nothing offered
  uint8_t other_hash[1] = { (uint8_t)(chan.hash[0] ^ 0xFF) };
  EXPECT_EQ(fleet.appendChannelByHash(other_hash, dest, 4, 0), 0);
  fleet.setEnabled(false);
  EXPECT_EQ(fleet.appendChannelByHash(hash, dest, 4, 0), 0);
}

TEST_F(FleetHooksTest, LoadPreservesDeviceServices) {
  // MyMesh wires the device services in just before fleet.begin(fs): a
  // load-time resetToDefaults() must not wipe them (wiring, not state)
  flushSave();   // a persisted config exists
  FleetManager loaded;
  loaded.setClock(HookEnv::timeFn, &env);
  loaded.begin(&fs);   // load() runs resetToDefaults()
  // observable through the scheduled path: with the clock wired, an !at job
  // within the stale window is admitted and fires on the next pass; a wiped
  // clock would read as unset and refuse it
  GroupTextPayload p = makeGroupText("alice", "!id k1\n!at 1800000000\nreset");
  groupRecv(loaded, PAYLOAD_TYPE_GRP_TXT, chan, p);
  EXPECT_EQ(loaded.getSchedCount(), 1);   // clock survived load: job admitted
  loaded.runScripts(ExecRecorder::fn, &env.rec);
  ASSERT_EQ(env.rec.lines.size(), (size_t)1);   // dequeued by the wired clock
  EXPECT_EQ(env.rec.lines[0], "reset");
}

// ------------------------------------------------------------------ CLI

TEST_F(FleetHooksTest, StatusLine) {
  std::string r = cli(fleet, "");
  EXPECT_EQ(r.substr(0, 3), "on,");
  EXPECT_NE(r.find("chan h="), std::string::npos);
  EXPECT_NE(r.find("tags 1/8"), std::string::npos);
  EXPECT_NE(r.find("scripts ran:0 dup:0 noid:0 refused:0"), std::string::npos);
  EXPECT_NE(r.find("pending 0/2"), std::string::npos);
  EXPECT_NE(r.find("sched 0/2"), std::string::npos);
  EXPECT_NE(r.find("seen 0/32"), std::string::npos);
  EXPECT_NE(r.find("ackq 0/2"), std::string::npos);
  EXPECT_NE(r.find("badline 0"), std::string::npos);
  // the psk is never echoed
  EXPECT_EQ(r.find("00010203"), std::string::npos);
}

TEST_F(FleetHooksTest, StatusCountsSkippedLines) {
  // an over-long command line is skipped at run time and shows in the status.
  // The whole message must fit one packet (5 B header + "alice: " + text).
  std::string script = "!id bl1\n" + std::string(FLEET_CLI_LINE_MAX + 1, 'a');
  ASSERT_LE(script.size(), (size_t)MAX_PACKET_PAYLOAD - 12);
  deliver(script.c_str());
  fleet.runScripts(ExecRecorder::fn, &env.rec);
  EXPECT_TRUE(env.rec.lines.empty());
  EXPECT_NE(cli(fleet, "").find("badline 1"), std::string::npos);
}

TEST_F(FleetHooksTest, OnOffReplies) {
  EXPECT_EQ(cli(fleet, "off"), "OK - fleet off");
  EXPECT_EQ(cli(fleet, "off junk"), "Err - usage: on|off|chan|tag|reply|seen|forget");
  EXPECT_FALSE(fleet.isEnabled());
  EXPECT_EQ(cli(fleet, "on"), "OK - fleet on");
  EXPECT_TRUE(fleet.isEnabled());
}

TEST_F(FleetHooksTest, ChanCommands) {
  // bad psk refused, previous kept
  EXPECT_EQ(cli(fleet, "chan set ff"), "Err - psk must be 32 or 64 hex chars");
  EXPECT_TRUE(fleet.hasChannel());
  // a valid set replaces and reports the derived hash, never the psk
  uint8_t secret[16];
  cliDecodeHex("ffffffffffffffffffffffffffffffff", 32, secret, sizeof(secret));
  uint8_t digest[32];
  mesh::Utils::sha256(digest, sizeof(digest), secret, sizeof(secret));
  char want[80];
  snprintf(want, sizeof(want), "OK - fleet channel set h=%02X (PSK holder = admin)", digest[0]);
  EXPECT_EQ(cli(fleet, "chan set ffffffffffffffffffffffffffffffff"), std::string(want));
  EXPECT_EQ(cli(fleet, "chan set ffffffffffffffffffffffffffffffff junk"),
            "Err - usage: chan set <psk-hex>|clear");
  EXPECT_EQ(cli(fleet, "chan"), "Err - usage: chan set <psk-hex>|clear");
  // cleared: receipt stops, status shows chan -
  EXPECT_EQ(cli(fleet, "chan clear"), "OK - fleet channel cleared");
  EXPECT_EQ(cli(fleet, "chan clear junk"), "Err - usage: chan set <psk-hex>|clear");
  EXPECT_FALSE(fleet.hasChannel());
  std::string r = cli(fleet, "");
  EXPECT_NE(r.find("chan -"), std::string::npos);
}

TEST_F(FleetHooksTest, TagCommands) {
  EXPECT_EQ(cli(fleet, "tag add roof"), "OK - tag roof added (2/8)");
  EXPECT_EQ(cli(fleet, "tag add roof"), "OK - tag roof already set (2/8)");
  EXPECT_EQ(cli(fleet, "tag add no*wild"),
            "Err - tag must be 1..16 chars of [A-Za-z0-9._-]");
  EXPECT_EQ(cli(fleet, "tag add"), "Err - usage: tag add <tag>|del <tag>|list|clear");
  EXPECT_EQ(cli(fleet, "tag del roof"), "OK - tag roof deleted");
  EXPECT_EQ(cli(fleet, "tag del roof"), "Err - tag roof not set");
  EXPECT_EQ(cli(fleet, "tag list"), "tags: xiao");
  EXPECT_EQ(cli(fleet, "tag add"), "Err - usage: tag add <tag>|del <tag>|list|clear");
  EXPECT_EQ(cli(fleet, "tag"), "tags: xiao");   // bare `tag` lists, like bare `fleet`
  EXPECT_EQ(cli(fleet, "tag bogus"), "Err - usage: tag add <tag>|del <tag>|list|clear");
  fleet.clearTags();
  EXPECT_EQ(cli(fleet, "tag list"), "tags: (none) - broadcast scripts only");
  EXPECT_EQ(cli(fleet, "tag clear"), "OK - tags cleared");
  for (int i = 0; i < FLEET_MAX_TAGS; i++) {
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "tag add t%d", i);
    ASSERT_EQ(cli(fleet, cmd).substr(0, 3), "OK ");
  }
  EXPECT_EQ(cli(fleet, "tag add onemore"), "Err - tag store full (8/8)");
}

TEST_F(FleetHooksTest, ReplyWindowCommands) {
  EXPECT_EQ(cli(fleet, "reply"), "reply window 60s");
  EXPECT_EQ(cli(fleet, "reply 90"), "OK - reply window 90s");
  EXPECT_EQ(fleet.getReplyWindowMs(), (uint32_t)90000);
  EXPECT_EQ(cli(fleet, "reply"), "reply window 90s");
  EXPECT_EQ(cli(fleet, "reply 0"), "Err - reply window must be 1..600 seconds");
  EXPECT_EQ(cli(fleet, "reply 601"), "Err - reply window must be 1..600 seconds");
  EXPECT_EQ(cli(fleet, "reply 90 junk"), "Err - usage: reply [<secs>]");
  EXPECT_EQ(fleet.getReplyWindowMs(), (uint32_t)90000);
  EXPECT_EQ(cli(fleet, "reply 600"), "OK - reply window 600s");
}

TEST_F(FleetHooksTest, SeenForgetCommands) {
  deliver("!id k1\nset a");   // marks the key at enqueue
  EXPECT_EQ(cli(fleet, "seen"), "seen 1/32");
  EXPECT_EQ(cli(fleet, "seen k1"), "OK - key k1 seen");
  EXPECT_EQ(cli(fleet, "seen nope"), "Err - key nope not seen");
  EXPECT_EQ(cli(fleet, "seen k1 junk"), "Err - usage: seen [<key>]");
  EXPECT_EQ(cli(fleet, "forget k1"), "OK - key forgotten");
  EXPECT_EQ(cli(fleet, "forget k1"), "Err - key not seen");
  EXPECT_EQ(cli(fleet, "forget"), "Err - usage: forget <key>|all");
  EXPECT_EQ(cli(fleet, "forget all"), "OK - seen table cleared");
}

TEST_F(FleetHooksTest, UsageLines) {
  EXPECT_EQ(cli(fleet, "bogus"), "Err - usage: on|off|chan|tag|reply|seen|forget");
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
// ------------------------------------------------------- adversarial regressions

TEST_F(FleetHooksTest, AtZeroEpochIsStaleNotRunNow) {
  // !at 0000000000 parses to due_epoch 0. It must be refused as a stale,
  // unset-clock timestamp, never run as if it had no !at at all.
  env.clock_now = 0;   // RTC unset
  deliver("!id z1\n!at 0000000000\nset a");
  EXPECT_FALSE(pass());
  EXPECT_EQ(fleet.getScripts().getRefused(), (uint32_t)1);
}

TEST_F(FleetHooksTest, ChatCountsAsNoId) {
  deliver("just chatting");
  EXPECT_EQ(fleet.getScripts().getNoId(), (uint32_t)1);
}

TEST_F(FleetHooksTest, MalformedScriptCountsRefused) {
  deliver("!id bad key!\nset a");        // BAD_KEY
  deliver("!id ok1\n!bogus\nset a");     // BAD_DIRECTIVE
  EXPECT_EQ(fleet.getScripts().getRefused(), (uint32_t)2);
  EXPECT_FALSE(pass());
}

TEST_F(FleetHooksTest, ForgetWhileQueuedDoesNotDoubleRun) {
  deliver("!id d1\nset a");
  fleet.getScripts().forgetKey("d1");
  deliver("!id d1\nset a");              // re-send after forget
  EXPECT_EQ(fleet.getScripts().getPendingCount(), 1);
}

TEST_F(FleetHooksTest, ForgetWhileArmedDoesNotDoubleArm) {
  env.clock_now = 1800000000UL;
  uint32_t due = env.clock_now + 600;
  char text[80];
  snprintf(text, sizeof(text), "!id a1\n!at %lu\nset a", (unsigned long)due);
  deliver(text);
  fleet.getScripts().forgetKey("a1");
  deliver(text);
  EXPECT_EQ(fleet.getSchedCount(), 1);
}

TEST_F(FleetHooksTest, FleetOffCancelsArmedJob) {
  env.clock_now = 1800000000UL;
  char text[80];
  snprintf(text, sizeof(text), "!id k9\n!at %lu\nset a", (unsigned long)(env.clock_now + 60));
  deliver(text);
  fleet.setEnabled(false);               // kill switch
  env.clock_now += 120;                  // due time passes
  EXPECT_FALSE(pass());
  EXPECT_EQ(fleet.getSchedCount(), 0);
}

TEST_F(FleetHooksTest, FleetOffCancelsQueuedScript) {
  deliver("!id q1\nset a");
  fleet.setEnabled(false);               // kill switch
  EXPECT_FALSE(pass());
  EXPECT_EQ(fleet.getScripts().getPendingCount(), 0);
}

TEST_F(FleetHooksTest, NonPlainTxtTypeNotAScript) {
  GroupTextPayload p = makeGroupText("alice", "!id t2\nset a");
  p.data[4] = 2 << 2;   // TXT_TYPE_SIGNED_PLAIN in the on-air header
  groupRecv(fleet, PAYLOAD_TYPE_GRP_TXT, chan, p);
  EXPECT_FALSE(pass());
}

// Random-script fuzz: no crash, no overrun, and the stores stay within their caps
// whatever mix of directives, clock values and kill switches arrives.
TEST_F(FleetHooksTest, FuzzScriptsNeverCrash) {
  static const char* atoms[] = {
      "!id ", "!id k", "!tags ", "!ack", "!ack err", "!at ", "!delay ", "!delay 1",
      "!", "\n", "\r\n", "\n\n", "#c", " ", ",", "xiao", "all", "set a", "fleet off",
      "fleet on", "fleet forget all", "fleet seen", "0000000000", "1800000000",
      "1800000301", "1700000000", "4294967296", "300000", "300001", "k1", "\"", "=",
      "reboot?", "filter add chan=#x", "fleet chan clear", "fleet tag del xiao"};
  const int natoms = sizeof(atoms) / sizeof(atoms[0]);
  uint32_t seed = 0xC0FFEE;
  auto rnd = [&](uint32_t m) { seed = seed * 1103515245u + 12345u; return (seed >> 8) % m; };
  for (int iter = 0; iter < 4000; iter++) {
    std::string s;
    if (rnd(4) != 0) s += "!id f" + std::to_string(rnd(40)) + "\n";
    int parts = 1 + rnd(12);
    for (int i = 0; i < parts; i++) s += atoms[rnd(natoms)];
    if (s.size() > 170) s.resize(170);
    GroupTextPayload p = makeGroupText("alice", s.c_str());
    groupRecv(fleet, PAYLOAD_TYPE_GRP_TXT, chan, p);
    if (rnd(3) == 0) { env.clock_now += rnd(900); g_mock_millis += rnd(4000); }
    fleet.runScripts(ExecRecorder::fn, &env.rec);
    if (rnd(50) == 0) fleet.getScripts().forgetAll();
    if (rnd(40) == 0) fleet.setEnabled(!fleet.isEnabled());
    if (rnd(60) == 0) fleet.setEnabled(true);
    if (env.rec.lines.size() > 100000) env.rec.lines.clear();
  }
  EXPECT_LE(fleet.getScripts().getPendingCount(), FLEET_CLI_QUEUE_DEPTH);
  EXPECT_LE(fleet.getSchedCount(), FLEET_SCHED_STORE);
  EXPECT_LE(fleet.getReplyCount(), FLEET_REPLY_STORE);
}

// Random fleet CLI fuzz: every reply stays inside the 160-byte buffer and is
// NUL-terminated (the cli() helper asserts both).
TEST_F(FleetHooksTest, FuzzFleetCliReplyBounded) {
  static const char* atoms[] = {
      "fleet", "on", "off", "chan", "set", "clear", "tag", "add", "del", "list",
      "reply", "seen", "forget", "all", "0", "1", "600", "601", "-1", "x", "\"",
      "\"a b\"", "00112233445566778899aabbccddeeff", "zz", "xiao", "a.b_c-d", "12345678901234567890123456789012345678901234567890", " ", "  "};
  const int natoms = sizeof(atoms) / sizeof(atoms[0]);
  uint32_t seed = 0xBEEF;
  auto rnd = [&](uint32_t m) { seed = seed * 1103515245u + 12345u; return (seed >> 8) % m; };
  for (int iter = 0; iter < 3000; iter++) {
    std::string s = "fleet";
    int parts = rnd(8);
    for (int i = 0; i < parts; i++) { s += " "; s += atoms[rnd(natoms)]; }
    cli(fleet, s.c_str());
  }
  SUCCEED();
}

TEST_F(FleetHooksTest, ChannelClearCancelsQueuedAndArmed) {
  env.clock_now = 1800000000UL;
  char text[80];
  snprintf(text, sizeof(text), "!id ca1\n!at %lu\nset a", (unsigned long)(env.clock_now + 60));
  deliver(text);
  deliver("!id ca2\nset b");
  ASSERT_EQ(fleet.getSchedCount(), 1);
  ASSERT_EQ(fleet.getScripts().getPendingCount(), 1);
  fleet.clearChannel();                  // kill switch
  env.clock_now += 120;
  EXPECT_FALSE(pass());
  EXPECT_EQ(fleet.getSchedCount(), 0);
  EXPECT_EQ(fleet.getScripts().getPendingCount(), 0);
}

// a script that switches the fleet off while it is running: the in-flight script
// finishes, the one queued behind it is cancelled, and nothing corrupts the slot
TEST_F(FleetHooksTest, FleetOffFromInsideRunningScriptIsSafe) {
  struct Ctx { FleetManager* f; std::vector<std::string> lines; } ctx{&fleet, {}};
  auto fn = [](void* c, const char*, char* line, char* reply) {
    auto* x = (Ctx*)c;
    x->lines.push_back(line);
    if (strcmp(line, "fleet off") == 0) x->f->setEnabled(false);
    reply[0] = 0;
  };
  deliver("!id in1\nset a\nfleet off\nset b");
  deliver("!id in2\nset c");
  fleet.runScripts(fn, &ctx);
  EXPECT_EQ(ctx.lines, (std::vector<std::string>{"set a", "fleet off", "set b"}));
  EXPECT_FALSE(fleet.isEnabled());
  // the cancel lands on the next pass; nothing else runs
  fleet.runScripts(fn, &ctx);
  EXPECT_EQ(ctx.lines.size(), (size_t)3);
  EXPECT_EQ(fleet.getScripts().getPendingCount(), 0);
}

TEST_F(FleetHooksTest, ReSendOfCancelledJobStaysConsumed) {
  deliver("!id cx\nset a");
  fleet.setEnabled(false);
  EXPECT_FALSE(pass());
  fleet.setEnabled(true);
  deliver("!id cx\nset a");               // same key: consumed, not re-run
  EXPECT_EQ(fleet.getScripts().getPendingCount(), 0);
  EXPECT_EQ(fleet.getScripts().getDup(), (uint32_t)1);
}
