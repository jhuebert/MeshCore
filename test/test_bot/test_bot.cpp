// Native tests for the chat bot (ChatBot). Reach the feature through the same
// entry points MyMesh uses: onGroupData(), appendChannelByHash(), botCLI(), and
// load()/loop() for persistence.

#include <gtest/gtest.h>

#include <Arduino.h>        // g_mock_millis (NativeShim.h is force-included)
#include <string.h>
#include <string>
#include <vector>

#include "ChatBot.h"
#include "CliUtil.h"
#include "KeyedChannel.h"
#include <helpers/TxtDataHelpers.h>   // TXT_TYPE_PLAIN
#include "../test_packet_filter/CliReplyHelper.h"

static const char* HEX16 = "0123456789ABCDEF0123456789ABCDEF";

static std::string expectedChanHash() {
  uint8_t secret[16], hash;
  deriveChannelKey("#bottest", secret, &hash);
  char buf[8];
  snprintf(buf, sizeof(buf), "%02X", hash);
  return std::string(buf);
}

struct BotTest : public ::testing::Test {
  NativeFS fs;
  ChatBot bot;

  int sends = 0;
  std::string sent_text;
  std::vector<uint8_t> sent_secret;
  uint8_t sent_hash = 0, sent_profile = 0;
  uint32_t sent_delay = 0;
  uint32_t jitter_ms = 1500;
  bool fail_send = false;

  static bool sendFn(void* ctx, const uint8_t* secret, uint8_t hash, uint8_t profile,
                     uint32_t delay, const char* text, size_t len) {
    BotTest* self = (BotTest*)ctx;
    self->sends++;
    if (self->fail_send) return false;
    self->sent_secret.assign(secret, secret + 32);
    self->sent_hash = hash;
    self->sent_profile = profile;
    self->sent_delay = delay;
    self->sent_text.assign(text, len);
    return true;
  }

  static uint32_t jitterFn(void* ctx) { return ((BotTest*)ctx)->jitter_ms; }

  void SetUp() override {
    g_mock_millis = 1000;
    bot.begin(&fs, this, sendFn, jitterFn);
  }
  void TearDown() override { g_mock_millis = 0; }

  void arm(const char* hex = HEX16) {
    ASSERT_TRUE(bot.setChannel(hex));
    bot.setEnabled(true);
  }

  // Feed one decrypted group text. `path` is raw path bytes; hash_size groups
  // them into hops the way the sender's Path Hash Size setting does.
  void feed(const char* sender, const char* text, uint8_t hash_size = 2,
            const std::vector<uint8_t>& path = {}, uint8_t profile = 0) {
    uint8_t data[256];
    memset(data, 0, sizeof(data));
    uint32_t ts = 12345;
    memcpy(data, &ts, 4);
    data[4] = TXT_TYPE_PLAIN;
    std::string body = std::string(sender) + ": " + text;
    memcpy(data + 5, body.c_str(), body.size() + 1);

    mesh::Packet pkt;
    pkt.setPathHashSizeAndCount(hash_size, (uint8_t)(path.size() / hash_size));
    if (!path.empty()) memcpy(pkt.path, path.data(), path.size());
    pkt.setRxProfile(profile);

    mesh::GroupChannel chan;
    memset(&chan, 0, sizeof(chan));
    chan.hash[0] = bot.getChanHash();
    memcpy(chan.secret, bot.getPsk(), sizeof(chan.secret));
    bot.onGroupData(&pkt, PAYLOAD_TYPE_GRP_TXT, chan, data, 5 + body.size(), NULL, "NodeName");
  }

  void flushSave() {
    g_mock_millis += LAZY_SAVE_DELAY_MS + 1;
    bot.loop(&fs);
  }

  // Run a CLI command through a canaried buffer and return the reply.
  std::string cli(const char* cmd) {
    const char* sub = cmd + 3;   // MyMesh strips the "bot" verb before dispatch
    while (*sub == ' ') sub++;
    CliReplyBuf buf;
    botCLI(bot, sub, buf.reply());
    buf.check(cmd);
    return buf.reply();
  }
};

// ---------------------------------------------------------------- defaults

TEST_F(BotTest, FreshNodeIsOffWithNoChannelAndWritesNoFile) {
  EXPECT_FALSE(bot.isEnabled());
  EXPECT_EQ(bot.getPskLen(), 0);
  EXPECT_EQ(bot.getChanHash(), 0);
  EXPECT_EQ(bot.getName()[0], 0);
  flushSave();
  EXPECT_TRUE(fs.files.empty());
}

TEST_F(BotTest, DisabledBotIgnoresEverything) {
  arm();
  bot.setEnabled(false);
  feed("Alice", "test");
  EXPECT_EQ(sends, 0);
  EXPECT_EQ(bot.isEnabled(), false);
}

TEST_F(BotTest, WrongChannelOrTypeIsIgnored) {
  arm();
  uint8_t data[64];
  memset(data, 0, sizeof(data));
  data[4] = TXT_TYPE_PLAIN;
  mesh::Packet pkt;
  mesh::GroupChannel chan;
  memset(&chan, 0, sizeof(chan));
  chan.hash[0] = bot.getChanHash();
  chan.secret[0] ^= 1;   // hash matches but secret does not
  bot.onGroupData(&pkt, PAYLOAD_TYPE_GRP_TXT, chan, data, 6, NULL, "NodeName");
  EXPECT_EQ(sends, 0);
  feed("Alice", "test");
  EXPECT_EQ(sends, 1);
}

// Core decides a 16-byte key by checking that bytes 16..31 are zero, and HMACs
// all 32 bytes (BaseChatMesh.cpp / Utils.cpp). A garbage tail makes the channel
// match its hash but fail to decrypt. Poison the stack first so an uninitialised
// tail cannot pass by luck on a fresh stack page.
static void poisonStack() {
  volatile uint8_t junk[128];
  memset((void*)junk, 0xA5, sizeof(junk));
}

TEST_F(BotTest, ChannelSpecZeroPadsSixteenByteKeys) {
  uint8_t secret[PUB_KEY_SIZE], len, hash;
  memset(secret, 0xA5, sizeof(secret));
  ASSERT_TRUE(parseChannelSpec("#bottest", secret, &len, &hash));
  EXPECT_EQ(len, 16);
  for (int i = 16; i < PUB_KEY_SIZE; i++) EXPECT_EQ(secret[i], 0) << i;

  memset(secret, 0xA5, sizeof(secret));
  ASSERT_TRUE(parseChannelSpec(HEX16, secret, &len, &hash));
  EXPECT_EQ(len, 16);
  for (int i = 16; i < PUB_KEY_SIZE; i++) EXPECT_EQ(secret[i], 0) << i;
}

TEST_F(BotTest, KeyOfferedToCoreIsZeroPadded) {
  poisonStack();
  ASSERT_TRUE(bot.setChannel("#bottest"));
  bot.setEnabled(true);
  uint8_t h[1] = { bot.getChanHash() };
  mesh::GroupChannel dest[2];
  memset(dest, 0xFF, sizeof(dest));   // core's candidate array is raw stack
  ASSERT_EQ(bot.appendChannelByHash(h, dest, 2, 0), 1);
  for (int i = 16; i < PUB_KEY_SIZE; i++) EXPECT_EQ(dest[0].secret[i], 0) << i;
  for (int i = 16; i < PUB_KEY_SIZE; i++) EXPECT_EQ(bot.getPsk()[i], 0) << i;
}

// ---------------------------------------------------------------- channels

TEST_F(BotTest, HashtagChannelMatchesFilterDerivation) {
  uint8_t secret[16], hash;
  deriveChannelKey("#bottest", secret, &hash);
  ASSERT_TRUE(bot.setChannel("#bottest"));
  EXPECT_EQ(bot.getPskLen(), 16);
  EXPECT_EQ(bot.getChanHash(), hash);
  EXPECT_EQ(memcmp(bot.getPsk(), secret, 16), 0);
}

TEST_F(BotTest, HexAndHashtagFormsAndOff) {
  EXPECT_FALSE(bot.setChannel("#"));
  EXPECT_FALSE(bot.setChannel("zz"));
  EXPECT_FALSE(bot.setChannel("0123456789ABCDEF"));
  EXPECT_TRUE(bot.setChannel(HEX16));
  EXPECT_EQ(bot.getPskLen(), 16);
  EXPECT_TRUE(bot.setChannel("#bottest"));
  EXPECT_EQ(bot.getPskLen(), 16);
  bot.clearChannel();
  EXPECT_EQ(bot.getPskLen(), 0);
  EXPECT_EQ(bot.getChanHash(), 0);
}

TEST_F(BotTest, AppendChannelByHashOffersOnlyWhenEnabledAndSet) {
  uint8_t hash[1] = { (uint8_t)bot.getChanHash() };
  mesh::GroupChannel dest[4];
  memset(dest, 0, sizeof(dest));
  EXPECT_EQ(bot.appendChannelByHash(hash, dest, 4, 0), 0);   // no key yet
  bot.setEnabled(true);
  EXPECT_EQ(bot.appendChannelByHash(hash, dest, 4, 0), 0);   // still no key
  ASSERT_TRUE(bot.setChannel(HEX16));
  hash[0] = bot.getChanHash();
  EXPECT_EQ(bot.appendChannelByHash(hash, dest, 4, 0), 1);
  EXPECT_EQ(dest[0].hash[0], hash[0]);
  EXPECT_EQ(memcmp(dest[0].secret, bot.getPsk(), 32), 0);
  // duplicate key already offered: no second slot burned
  EXPECT_EQ(bot.appendChannelByHash(hash, dest, 4, 1), 0);
  hash[0] ^= 1;
  EXPECT_EQ(bot.appendChannelByHash(hash, dest, 4, 1), 0);
  EXPECT_EQ(bot.appendChannelByHash(hash, dest, 1, 1), 0);   // list full
}

// ---------------------------------------------------------------- matching

TEST_F(BotTest, UnifiedAliasesAllReachThePathReply) {
  arm();
  const char* triggers[] = { "test", "t", "path", "p", "m", "multitest", "mtest",
                             "atest", "testing 123", "multipath", "mt", "!TEST",
                             "  test  " };
  const std::vector<uint8_t> path = { 0x1A, 0x2B, 0x3C, 0x4D };
  for (const char* tr : triggers) {
    sends = 0;
    feed("Alice", tr, 2, path);
    ASSERT_EQ(sends, 1) << tr;
    EXPECT_EQ(sent_text, std::string("NodeName: @[Alice]\n1A2B 3C4D")) << tr;
    g_mock_millis += BOT_MIN_INTERVAL_MS + 1;
  }
}

TEST_F(BotTest, PingIsMentionOnlyAndPNeverMeansPing) {
  arm();
  feed("Alice", "ping");
  ASSERT_EQ(sends, 1);
  EXPECT_EQ(sent_text, "NodeName: @[Alice]");
  g_mock_millis += BOT_MIN_INTERVAL_MS + 1;
  feed("Alice", "p");
  ASSERT_EQ(sends, 2);
  EXPECT_EQ(sent_text, "NodeName: @[Alice]\ndirect");
}

TEST_F(BotTest, HelpHasNoMentionLine) {
  arm();
  feed("Alice", "help");
  ASSERT_EQ(sends, 1);
  EXPECT_EQ(sent_text, "Commands: ping, test");
  g_mock_millis += BOT_MIN_INTERVAL_MS + 1;
  feed("Alice", "h");
  EXPECT_EQ(sent_text, "Commands: ping, test");
}

TEST_F(BotTest, UnrelatedAndSingleLettersInsideWordsAreSilent) {
  arm();
  const char* quiet[] = { "hello", "hi", "I saw a t earlier", "status time", "weather" };
  for (const char* q : quiet) {
    feed("Alice", q);
    EXPECT_EQ(sends, 0) << q;
  }
}

TEST_F(BotTest, PingWinsOverTestByTableOrder) {
  arm();
  feed("Alice", "ping test");
  ASSERT_EQ(sends, 1);
  EXPECT_EQ(sent_text, "NodeName: @[Alice]");
}

TEST_F(BotTest, BotNamePrefixAndSenderSanitation) {
  arm();
  bot.setName("TestBot");
  feed("  A\x01lice  ", "test");
  ASSERT_EQ(sends, 1);
  EXPECT_EQ(sent_text, "TestBot: @[Alice]\ndirect");
  bot.setName("");
  g_mock_millis += BOT_MIN_INTERVAL_MS + 1;
  feed("", "test", 2, {});
  EXPECT_EQ(sent_text, "NodeName: @[anon]\ndirect");
}

TEST_F(BotTest, PathRenderingForEachHashSize) {
  arm();
  const uint8_t p1[] = { 0x1A, 0x2B, 0x3C };
  feed("Alice", "test", 1, std::vector<uint8_t>(p1, p1 + 3));
  EXPECT_EQ(sent_text, "NodeName: @[Alice]\n1A 2B 3C");
  g_mock_millis += BOT_MIN_INTERVAL_MS + 1;
  const uint8_t p3[] = { 1, 2, 3, 4, 5, 6 };
  feed("Alice", "test", 3, std::vector<uint8_t>(p3, p3 + 6));
  EXPECT_EQ(sent_text, "NodeName: @[Alice]\n010203 040506");
}

TEST_F(BotTest, SendMetadataAndReplyBound) {
  arm();
  const uint8_t p[] = { 0xAA, 0xBB, 0xCC, 0xDD };
  feed("Alice", "test", 2, std::vector<uint8_t>(p, p + 4), 1);
  EXPECT_EQ(sends, 1);
  EXPECT_EQ(sent_hash, bot.getChanHash());
  EXPECT_EQ(sent_profile, 1);
  EXPECT_EQ(sent_delay, 1500);
  EXPECT_EQ(sent_secret, std::vector<uint8_t>(bot.getPsk(), bot.getPsk() + 32));
  EXPECT_LE(sent_text.size(), (size_t)BOT_TEXT_MAX);
}

// ---------------------------------------------------------------- rate gate

TEST_F(BotTest, GlobalIntervalSuppressesSecondReply) {
  arm();
  feed("Alice", "test");
  feed("Bob", "ping");
  EXPECT_EQ(sends, 1);
  g_mock_millis += BOT_MIN_INTERVAL_MS + 1;
  feed("Bob", "ping");
  EXPECT_EQ(sends, 2);
}

TEST_F(BotTest, FailedSendLeavesNoRateState) {
  arm();
  fail_send = true;
  feed("Alice", "test");
  EXPECT_EQ(sends, 1);
  fail_send = false;
  feed("Alice", "test");   // same millis: the failed send closed no interval
  EXPECT_EQ(sends, 2);
}

// ---------------------------------------------------------------- persistence

TEST_F(BotTest, ConfigRoundTripsThroughBotCfg) {
  arm();
  bot.setName("TestBot");
  flushSave();
  ASSERT_TRUE(fs.exists(BOT_CFG_FILE));
  ChatBot loaded;
  loaded.load(&fs);
  EXPECT_TRUE(loaded.isEnabled());
  EXPECT_STREQ(loaded.getName(), "TestBot");
  EXPECT_EQ(loaded.getPskLen(), 16);
  EXPECT_EQ(loaded.getChanHash(), bot.getChanHash());
  EXPECT_EQ(memcmp(loaded.getPsk(), bot.getPsk(), 32), 0);
}

TEST_F(BotTest, CorruptRecordsFallBackToDefaults) {
  bot.setEnabled(true);
  ASSERT_TRUE(bot.setChannel(HEX16));
  flushSave();
  std::vector<uint8_t> good = fs.files[BOT_CFG_FILE];
  ASSERT_EQ(good.size(), BOT_CFG_RECORD_BYTES);

  // bad magic
  fs.files[BOT_CFG_FILE] = good;
  fs.files[BOT_CFG_FILE][0] ^= 1;
  bot.load(&fs);
  EXPECT_FALSE(bot.isEnabled());
  // bad version
  fs.files[BOT_CFG_FILE] = good;
  fs.files[BOT_CFG_FILE][4] = 2;
  bot.load(&fs);
  EXPECT_FALSE(bot.isEnabled());
  // bad CRC
  fs.files[BOT_CFG_FILE] = good;
  fs.files[BOT_CFG_FILE][BOT_CFG_PAYLOAD_BYTES] ^= 1;
  bot.load(&fs);
  EXPECT_FALSE(bot.isEnabled());
  // short / long
  fs.files[BOT_CFG_FILE] = std::vector<uint8_t>(good.begin(), good.end() - 1);
  bot.load(&fs);
  EXPECT_FALSE(bot.isEnabled());
  fs.files[BOT_CFG_FILE] = good;
  fs.files[BOT_CFG_FILE].push_back(0);
  bot.load(&fs);
  EXPECT_FALSE(bot.isEnabled());
  // bad psk_len
  fs.files[BOT_CFG_FILE] = good;
  fs.files[BOT_CFG_FILE][23] = 7;
  bot.load(&fs);
  EXPECT_FALSE(bot.isEnabled());
}

// ---------------------------------------------------------------- CLI

TEST_F(BotTest, CliStatusOnOffNameAndChan) {
  EXPECT_EQ(cli("bot"), "bot: off, name (node), chan (none)");
  EXPECT_EQ(cli("bot on"), "OK - bot on");
  EXPECT_EQ(cli("bot name TestBot"), "OK - name set");
  EXPECT_EQ(cli("bot chan #bottest"), "OK - chan h=" + expectedChanHash());
  EXPECT_EQ(cli("bot"), "bot: on, name TestBot, chan h=" + expectedChanHash());
  EXPECT_EQ(cli("bot name"), "OK - name set");
  EXPECT_EQ(cli("bot chan off"), "OK - chan cleared");
  EXPECT_EQ(cli("bot off"), "OK - bot off");
}

TEST_F(BotTest, CliAcceptsAFullLength32BytePsk) {
  const char* hex64 = "000102030405060708090A0B0C0D0E0F"
                      "101112131415161718191A1B1C1D1E1F";
  std::string cmd = std::string("bot chan ") + hex64;
  uint8_t key[32], h;
  int key_len = 0;
  ASSERT_TRUE(cliDecodePskHex(hex64, key, sizeof(key), &key_len));
  mesh::Utils::sha256(&h, 1, key, 32);
  char expected[24];
  snprintf(expected, sizeof(expected), "OK - chan h=%02X", h);
  EXPECT_EQ(cli(cmd.c_str()), expected);
  EXPECT_EQ(bot.getPskLen(), 32);
}

TEST_F(BotTest, CliRejectsBothChanFormsAndBadValues) {
  std::string both = std::string("bot chan #bottest ") + HEX16;
  EXPECT_EQ(cli(both.c_str()), "Err - usage: chan #<name>|<psk-hex> | chan off");
  EXPECT_EQ(cli("bot chan #"), "Err - chan name must be 1..15 chars");
  EXPECT_EQ(cli("bot chan 0123"), "Err - psk must be 32 or 64 hex digits");
  EXPECT_EQ(cli("bot name thisnameiswaytoolongtofit"), "Err - name must be 1..16 chars");
  EXPECT_EQ(cli("bot hops 2"), "Err - usage: on|off|name|chan");
  EXPECT_EQ(cli("bot stats"), "Err - usage: on|off|name|chan");
  EXPECT_EQ(cli("bot tips reset"), "Err - usage: on|off|name|chan");
  EXPECT_EQ(cli("bot on junk"), "Err - usage: bot on|off");
}

TEST_F(BotTest, NonPlainTextWrongTypeAndShortPayloadAreIgnored) {
  arm();
  uint8_t data[64];
  mesh::Packet pkt;
  mesh::GroupChannel chan;
  memset(&chan, 0, sizeof(chan));
  chan.hash[0] = bot.getChanHash();
  memcpy(chan.secret, bot.getPsk(), sizeof(chan.secret));

  // too short for the 5-byte group-text header
  bot.onGroupData(&pkt, PAYLOAD_TYPE_GRP_TXT, chan, data, 4, NULL, "NodeName");
  EXPECT_EQ(sends, 0);

  memset(data, 0, sizeof(data));
  data[4] = TXT_TYPE_CLI_DATA << 2;
  memcpy(data + 5, "Alice: test", 12);
  bot.onGroupData(&pkt, PAYLOAD_TYPE_GRP_TXT, chan, data, 17, NULL, "NodeName");
  EXPECT_EQ(sends, 0);

  data[4] = TXT_TYPE_PLAIN;
  bot.onGroupData(&pkt, PAYLOAD_TYPE_GRP_DATA, chan, data, 17, NULL, "NodeName");
  EXPECT_EQ(sends, 0);
}

TEST_F(BotTest, LongPskWorksAndOffKeepsTheChannel) {
  const char* hex64 = "000102030405060708090A0B0C0D0E0F"
                      "101112131415161718191A1B1C1D1E1F";
  ASSERT_TRUE(bot.setChannel(hex64));
  EXPECT_EQ(bot.getPskLen(), 32);
  bot.setEnabled(true);
  feed("Alice", "test");
  ASSERT_EQ(sends, 1);
  uint8_t expected_hash;
  mesh::Utils::sha256(&expected_hash, 1, bot.getPsk(), 32);
  EXPECT_EQ(bot.getChanHash(), expected_hash);

  bot.setEnabled(false);
  EXPECT_EQ(bot.getPskLen(), 32);   // off preserves the key
  bot.setEnabled(true);             // one command to re-enable
  g_mock_millis += BOT_MIN_INTERVAL_MS + 1;
  feed("Alice", "test");
  EXPECT_EQ(sends, 2);
}

TEST_F(BotTest, RecordBytesAreDeterministic) {
  arm();
  bot.setName("TestBot");
  flushSave();
  std::vector<uint8_t> first = fs.files[BOT_CFG_FILE];
  ASSERT_EQ(first.size(), BOT_CFG_RECORD_BYTES);
  fs.files.erase(BOT_CFG_FILE);
  bot.save(&fs);   // write the same state again
  EXPECT_EQ(fs.files[BOT_CFG_FILE], first);
}

TEST_F(BotTest, FirstReplyAtMillisZeroClosesTheGate) {
  g_mock_millis = 0;
  arm();
  feed("Alice", "test");
  ASSERT_EQ(sends, 1);
  feed("Alice", "test");   // millis still 0: must be suppressed
  EXPECT_EQ(sends, 1);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
