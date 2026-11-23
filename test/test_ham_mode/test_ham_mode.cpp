// test_ham_mode.cpp: native tests for ham mode (examples/simple_repeater/HamMode.*).
// Built by [env:native_ham_mode]; see that env for the shim and include-order
// notes. Every check drives the same entry points the firmware uses:
// classify(), selectPower(), loop()/idDue(), hamCLI() and load()/save().

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include <Arduino.h>
#include <Packet.h>
#include <Utils.h>

#include "CliReplyHelper.h"
#include "HamMode.h"
#include "KeyedChannel.h"
#include "PersistUtil.h"

// ---------------------------------------------------------------- helpers

static void publicKey(uint8_t out[16]) {
  ASSERT_TRUE(mesh::Utils::fromHex(out, 16, MESHCORE_PUBLIC_PSK_HEX));
}

// Build a group datagram the way Mesh::createGroupDatagram() does: a 1-byte
// channel hash followed by MAC(2) plus AES ciphertext. Real crypto is linked
// in this env, so a MAC check is a genuine proof of channel membership.
static void makeGroup(mesh::Packet& pkt, uint8_t type, const uint8_t secret[16],
                      const char* text) {
  memset(&pkt, 0, sizeof(pkt));
  pkt.header = (uint8_t)(type << PH_TYPE_SHIFT);
  uint8_t key[PUB_KEY_SIZE];
  memset(key, 0, sizeof(key));
  memcpy(key, secret, 16);
  uint8_t enc[MAX_PACKET_PAYLOAD];
  int n = mesh::Utils::encryptThenMAC(key, enc, (const uint8_t*)text, (int)strlen(text));
  uint8_t hash;
  mesh::Utils::sha256(&hash, 1, key, 16);
  pkt.payload[0] = hash;
  memcpy(&pkt.payload[1], enc, n);
  pkt.payload_len = (uint16_t)(1 + n);
}

static void plainPacket(mesh::Packet& pkt, uint8_t type) {
  memset(&pkt, 0, sizeof(pkt));
  pkt.header = (uint8_t)(type << PH_TYPE_SHIFT);
}

static void writeRecord(NativeFS& fs, const char* path, const HamCfgRecord& rec,
                        bool good_crc = true) {
  uint8_t buf[HAM_CFG_RECORD_BYTES];
  memcpy(buf, &rec, HAM_CFG_PAYLOAD_BYTES);
  uint16_t crc = crc16_ccitt(buf, HAM_CFG_PAYLOAD_BYTES);
  if (!good_crc) crc ^= 0x0001;
  memcpy(&buf[HAM_CFG_PAYLOAD_BYTES], &crc, 2);
  fs.files[path] = std::vector<uint8_t>(buf, buf + HAM_CFG_RECORD_BYTES);
}

static HamCfgRecord defaultRecord() {
  HamCfgRecord rec;
  memset(&rec, 0, sizeof(rec));
  memcpy(rec.magic, HAM_CFG_MAGIC, 4);
  rec.version = HAM_CFG_VERSION;
  rec.ham_dbm = HAM_TX_POWER;
  return rec;
}

static std::string cli(HamMode& ham, const char* name, const char* command,
                       CliReplyBuf& buf) {
  // hamCLI() takes the command after the "ham" verb, as MyMesh's dispatch does.
  const char* sub = command;
  if (strncmp(command, "ham", 3) == 0) {
    sub = command + 3;
    while (*sub == ' ') sub++;
  }
  buf.reply()[0] = 0;
  hamCLI(ham, name, sub, buf.reply());
  buf.check(command);
  return std::string(buf.reply());
}

class HamTest : public ::testing::Test {
protected:
  NativeFS fs;
  HamMode ham;

  void SetUp() override {
    g_mock_millis = 0;
    ham.load(&fs);           // no file: disabled defaults
    ham.noteRadioPower(7);   // stand in for `set tx 7`
  }
};

// ============================================================ crypto linkage

TEST(HamCrypto, RealDigestsAreLinked) {
  uint8_t psk[16];
  publicKey(psk);
  uint8_t air;
  mesh::Utils::sha256(&air, 1, psk, 16);
  EXPECT_EQ(air, 0x11);

  uint8_t secret[16], hash;
  deriveChannelKey("#test", secret, &hash);
  char hex[33];
  mesh::Utils::toHex(hex, secret, 16);
  EXPECT_STREQ(hex, "9CD8FCF22A47333B591D96A2B848B73F");
}

// ============================================================ classification

TEST_F(HamTest, PlaintextTypesArePublic) {
  const uint8_t types[] = {PAYLOAD_TYPE_ADVERT, PAYLOAD_TYPE_ACK,
                           PAYLOAD_TYPE_TRACE, PAYLOAD_TYPE_CONTROL};
  for (uint8_t t : types) {
    mesh::Packet pkt;
    plainPacket(pkt, t);
    EXPECT_EQ(ham.classify(&pkt), HAM_TX_PUBLIC) << "type " << (int)t;
  }
}

TEST_F(HamTest, EncryptedTypesArePrivate) {
  for (int t = 0; t < 16; t++) {
    switch (t) {
      case PAYLOAD_TYPE_ADVERT: case PAYLOAD_TYPE_ACK:
      case PAYLOAD_TYPE_TRACE: case PAYLOAD_TYPE_CONTROL:
      case PAYLOAD_TYPE_MULTIPART:
      case PAYLOAD_TYPE_GRP_TXT: case PAYLOAD_TYPE_GRP_DATA:
        continue;
    }
    mesh::Packet pkt;
    plainPacket(pkt, (uint8_t)t);
    EXPECT_EQ(ham.classify(&pkt), HAM_TX_ENCRYPTED) << "type " << t;
  }
}

TEST_F(HamTest, OnlyMultipartAckIsPublic) {
  mesh::Packet pkt;
  plainPacket(pkt, PAYLOAD_TYPE_MULTIPART);
  pkt.payload_len = 5;
  pkt.payload[0] = (uint8_t)((2 << 4) | PAYLOAD_TYPE_ACK);
  EXPECT_EQ(ham.classify(&pkt), HAM_TX_PUBLIC);
  pkt.payload[0] = (uint8_t)((2 << 4) | PAYLOAD_TYPE_TXT_MSG);
  EXPECT_EQ(ham.classify(&pkt), HAM_TX_ENCRYPTED);
  pkt.payload_len = 0;
  pkt.payload[0] = PAYLOAD_TYPE_ACK;
  EXPECT_EQ(ham.classify(&pkt), HAM_TX_ENCRYPTED);
}

TEST_F(HamTest, GroupOnPublicIsPublic) {
  uint8_t psk[16];
  publicKey(psk);
  mesh::Packet pkt;
  makeGroup(pkt, PAYLOAD_TYPE_GRP_TXT, psk, "hello");
  EXPECT_EQ(ham.classify(&pkt), HAM_TX_PUBLIC);
  makeGroup(pkt, PAYLOAD_TYPE_GRP_DATA, psk, "data");
  EXPECT_EQ(ham.classify(&pkt), HAM_TX_PUBLIC);
}

TEST_F(HamTest, GroupOnListedTagIsPublic) {
  uint8_t secret[16], hash;
  deriveChannelKey("#test", secret, &hash);
  ASSERT_TRUE(ham.addTag("#test"));
  mesh::Packet pkt;
  makeGroup(pkt, PAYLOAD_TYPE_GRP_TXT, secret, "tagged");
  EXPECT_EQ(ham.classify(&pkt), HAM_TX_PUBLIC);
}

TEST_F(HamTest, GroupOnUnlistedTagIsPrivate) {
  uint8_t secret[16], hash;
  deriveChannelKey("#other", secret, &hash);
  ham.addTag("#test");
  mesh::Packet pkt;
  makeGroup(pkt, PAYLOAD_TYPE_GRP_TXT, secret, "unlisted");
  EXPECT_EQ(ham.classify(&pkt), HAM_TX_ENCRYPTED);
}

TEST_F(HamTest, GroupOnPrivatePskIsPrivate) {
  uint8_t secret[16];
  memset(secret, 0x42, sizeof(secret));
  mesh::Packet pkt;
  makeGroup(pkt, PAYLOAD_TYPE_GRP_TXT, secret, "private");
  EXPECT_EQ(ham.classify(&pkt), HAM_TX_ENCRYPTED);
}

TEST_F(HamTest, FleetAckUsesPrivatePower) {
  // Fleet ACKs are not PAYLOAD_TYPE_ACK: MyMesh builds them as GRP_TXT on the
  // fleet channel's private PSK. That key is not in the ham list, so even an
  // active node must send them at `set tx`.
  ham.setEnabled(true);
  ham.setCallsign("N0CALL");
  ham.setHamDbm(22);
  uint8_t secret[16];
  memset(secret, 0x5A, sizeof(secret));
  mesh::Packet ack;
  makeGroup(ack, PAYLOAD_TYPE_GRP_TXT, secret, "jobkey\nok");
  EXPECT_EQ(ham.classify(&ack), HAM_TX_ENCRYPTED);
  EXPECT_EQ(ham.selectPower(&ack, "N0CALL Test", 7), 7);
}

TEST_F(HamTest, HashMatchWithoutMacIsNotEnough) {
  uint8_t psk[16];
  publicKey(psk);
  uint8_t air;
  mesh::Utils::sha256(&air, 1, psk, 16);
  mesh::Packet pkt;
  plainPacket(pkt, PAYLOAD_TYPE_GRP_TXT);
  pkt.payload[0] = air;             // right hash
  pkt.payload_len = 19;
  memset(&pkt.payload[1], 0xAA, 18); // garbage MAC and ciphertext
  EXPECT_EQ(ham.classify(&pkt), HAM_TX_ENCRYPTED);
}

TEST_F(HamTest, ShortGroupPayloadIsPrivate) {
  uint8_t psk[16];
  publicKey(psk);
  uint8_t air;
  mesh::Utils::sha256(&air, 1, psk, 16);
  mesh::Packet pkt;
  plainPacket(pkt, PAYLOAD_TYPE_GRP_TXT);
  pkt.payload[0] = air;
  pkt.payload_len = 18;             // one byte short of hash+MAC+block
  EXPECT_EQ(ham.classify(&pkt), HAM_TX_ENCRYPTED);
}

// ============================================================ activation gate

TEST_F(HamTest, DisabledByDefault) {
  EXPECT_FALSE(ham.isActive("N0CALL Test"));
  EXPECT_STREQ(ham.inactiveReason("N0CALL Test"), "disabled");
}

TEST_F(HamTest, NeedsCallsign) {
  ham.setEnabled(true);
  EXPECT_STREQ(ham.inactiveReason("N0CALL Test"), "no callsign");
}

TEST_F(HamTest, WholeWordCaseInsensitiveGate) {
  ham.setEnabled(true);
  ham.setCallsign("N0CALL");
  EXPECT_TRUE(ham.isActive("N0CALL Test"));
  EXPECT_TRUE(ham.isActive("net n0call"));
  EXPECT_FALSE(ham.isActive("N0CALLS Test"));   // substring false positive
  EXPECT_FALSE(ham.isActive("XN0CALL Test"));
  EXPECT_STREQ(ham.inactiveReason("Other Node"), "name lacks callsign");
}

TEST_F(HamTest, NameChangeAfterCallsign) {
  ham.setEnabled(true);
  ham.setCallsign("N0CALL");
  EXPECT_FALSE(ham.isActive("Other Node"));
  EXPECT_TRUE(ham.isActive("N0CALL Node"));     // the live name is passed in
}

// ============================================================ power selection

TEST_F(HamTest, DisabledReturnsSetTxForEveryClass) {
  uint8_t psk[16];
  publicKey(psk);
  mesh::Packet pub;
  makeGroup(pub, PAYLOAD_TYPE_GRP_TXT, psk, "hello");
  mesh::Packet ack;
  plainPacket(ack, PAYLOAD_TYPE_ACK);
  EXPECT_EQ(ham.selectPower(&pub, "N0CALL Test", 7), 7);
  EXPECT_EQ(ham.selectPower(&ack, "N0CALL Test", 7), 7);
  EXPECT_EQ(ham.getPublicCount(), 0u);
  EXPECT_EQ(ham.getPrivateCount(), 0u);
}

TEST_F(HamTest, PublicUsesHamTxEncryptedUsesSetTx) {
  ham.setEnabled(true);
  ham.setCallsign("N0CALL");
  ham.setHamDbm(22);
  uint8_t psk[16];
  publicKey(psk);
  mesh::Packet pub;
  makeGroup(pub, PAYLOAD_TYPE_GRP_TXT, psk, "hello");
  mesh::Packet dm;
  plainPacket(dm, PAYLOAD_TYPE_TXT_MSG);
  EXPECT_EQ(ham.selectPower(&pub, "N0CALL Test", 7), 22);
  EXPECT_EQ(ham.selectPower(&dm, "N0CALL Test", 7), 7);
  EXPECT_EQ(ham.getPublicCount(), 1u);
  EXPECT_EQ(ham.getPrivateCount(), 1u);
  // Changing `set tx` takes effect immediately for encrypted traffic.
  ham.noteRadioPower(10);
  EXPECT_EQ(ham.selectPower(&dm, "N0CALL Test", 10), 10);
  EXPECT_EQ(ham.selectPower(&pub, "N0CALL Test", 10), 22);
}

TEST_F(HamTest, AppliedPowerCacheOnlyChangesOnClassChange) {
  EXPECT_EQ(ham.getAppliedDbm(), 7);
  EXPECT_TRUE(ham.shouldApplyPower(22));
  ham.noteAppliedPower(22);
  EXPECT_FALSE(ham.shouldApplyPower(22));
  EXPECT_TRUE(ham.shouldApplyPower(7));
}

// ============================================================ CLI

TEST_F(HamTest, CliStatusAndOnOff) {
  CliReplyBuf b;
  EXPECT_EQ(cli(ham, "N0CALL Test", "ham", b), "off - all packets use normal tx power");
  EXPECT_EQ(cli(ham, "N0CALL Test", "status", b), "off - all packets use normal tx power");
  EXPECT_EQ(cli(ham, "N0CALL Test", "on", b), "OK - ham on");
  EXPECT_EQ(cli(ham, "N0CALL Test", "ham", b), "waiting - no callsign");
  EXPECT_EQ(cli(ham, "N0CALL Test", "callsign N0CALL", b), "OK - callsign set to N0CALL");
  EXPECT_EQ(cli(ham, "N0CALL Test", "ham", b),
            "on - call N0CALL, ham tx 20, set tx 7, tags 0, next ID 0s");
  EXPECT_EQ(cli(ham, "Other Node", "ham", b), "waiting - node name must include N0CALL");
  EXPECT_EQ(cli(ham, "N0CALL Test", "off", b), "OK - ham off");
}

TEST_F(HamTest, CliCallsignRules) {
  CliReplyBuf b;
  EXPECT_EQ(cli(ham, "N0CALL Test", "callsign", b), "no callsign set");
  EXPECT_EQ(cli(ham, "N0CALL Test", "callsign n0call", b), "OK - callsign set to N0CALL");
  EXPECT_EQ(cli(ham, "N0CALL Test", "callsign", b), "callsign is N0CALL");
  EXPECT_EQ(cli(ham, "N0CALL Test", "callsign AB", b),
            "Err - callsign must be one word, 3..15 chars");
  EXPECT_EQ(cli(ham, "N0CALL Test", "callsign ABCDEF1234567890", b),
            "Err - callsign must be one word, 3..15 chars");
  EXPECT_NE(cli(ham, "N0CALL Test", "callsign OK extra", b).find("usage"), std::string::npos);
}

TEST_F(HamTest, CliTxShowsAndSetsOnlyPublicPower) {
  CliReplyBuf b;
  EXPECT_EQ(cli(ham, "N0CALL Test", "tx", b), "ham tx is 20 dBm");
  EXPECT_EQ(cli(ham, "N0CALL Test", "tx 22", b), "OK - ham tx set to 22 dBm");
  EXPECT_EQ(ham.getHamDbm(), 22);
  EXPECT_EQ(ham.getPrivateDbm(), 7);   // set tx untouched
  EXPECT_EQ(cli(ham, "N0CALL Test", "tx -9", b), "OK - ham tx set to -9 dBm");
  EXPECT_EQ(cli(ham, "N0CALL Test", "tx 31", b), "Err - ham tx must be a number from -9 to 30");
  EXPECT_EQ(cli(ham, "N0CALL Test", "tx abc", b), "Err - ham tx must be a number from -9 to 30");
  // No legacy power verbs from the source fork.
  EXPECT_NE(cli(ham, "N0CALL Test", "public 22", b).find("usage"), std::string::npos);
  EXPECT_NE(cli(ham, "N0CALL Test", "private 7", b).find("usage"), std::string::npos);
}

TEST_F(HamTest, CliChannelRules) {
  CliReplyBuf b;
  EXPECT_EQ(cli(ham, "N0CALL Test", "channels", b), "Public");
  EXPECT_EQ(cli(ham, "N0CALL Test", "channel add test", b), "OK - #test now uses ham power");
  EXPECT_EQ(cli(ham, "N0CALL Test", "channel add #Nebraska", b), "OK - #Nebraska now uses ham power");
  EXPECT_EQ(cli(ham, "N0CALL Test", "channels", b), "Public, #test, #Nebraska");
  // Duplicate, one word, length, case sensitivity.
  EXPECT_NE(cli(ham, "N0CALL Test", "channel add #test", b).find("already"), std::string::npos);
  EXPECT_NE(cli(ham, "N0CALL Test", "channel add \"#two words\"", b).find("one word"), std::string::npos);
  EXPECT_NE(cli(ham, "N0CALL Test", "channel add #123456789012345", b).find("max 14"), std::string::npos);
  EXPECT_EQ(cli(ham, "N0CALL Test", "channel add #nebraska", b), "OK - #nebraska now uses ham power");
  EXPECT_EQ(cli(ham, "N0CALL Test", "channel remove #test", b), "OK - #test removed");
  EXPECT_NE(cli(ham, "N0CALL Test", "channel remove #test", b).find("not on the list"), std::string::npos);
  // The ham list is self-contained: no filter config file is touched.
  EXPECT_FALSE(fs.exists("/filter_cfg"));
}

TEST_F(HamTest, CliChannelFullList) {
  CliReplyBuf b;
  for (int i = 0; i < HAM_MAX_TAGS; i++) {
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "channel add #t%d", i);
    ASSERT_NE(cli(ham, "N0CALL Test", cmd, b).find("OK"), std::string::npos);
  }
  EXPECT_NE(cli(ham, "N0CALL Test", "channel add #ninth", b).find("full"), std::string::npos);
  EXPECT_EQ(ham.getTagCount(), HAM_MAX_TAGS);
}

TEST_F(HamTest, CliStatsAndUsage) {
  CliReplyBuf b;
  EXPECT_EQ(cli(ham, "N0CALL Test", "help", b).substr(0, 12), std::string("Err - usage:"));
  EXPECT_NE(cli(ham, "N0CALL Test", "bogus", b).find("usage"), std::string::npos);
  EXPECT_EQ(cli(ham, "N0CALL Test", "stats", b),
            "0 public, 0 private; applied 7 dBm; next ID 0s");
}

// ============================================================ persistence

TEST_F(HamTest, SaveLoadRoundtrip) {
  ham.setEnabled(true);
  ham.setCallsign("N0CALL");
  ham.setHamDbm(22);
  ham.addTag("#test");
  ham.save(&fs);
  ASSERT_TRUE(fs.exists(HAM_CFG_FILE));

  HamMode fresh;
  fresh.load(&fs);
  EXPECT_TRUE(fresh.isEnabled());
  EXPECT_STREQ(fresh.getCallsign(), "N0CALL");
  EXPECT_EQ(fresh.getHamDbm(), 22);
  EXPECT_EQ(fresh.getTagCount(), 1);
  EXPECT_STREQ(fresh.getTag(0), "#test");
  EXPECT_TRUE(fresh.isActive("N0CALL Test"));
}

TEST_F(HamTest, BadRecordsKeepDisabledDefaults) {
  HamCfgRecord rec = defaultRecord();
  rec.enabled = 1;
  strcpy(rec.callsign, "N0CALL");
  rec.ham_dbm = 22;

  writeRecord(fs, HAM_CFG_FILE, rec, false);   // bad CRC
  HamMode a; a.load(&fs);
  EXPECT_FALSE(a.isEnabled());
  EXPECT_STREQ(a.getCallsign(), "");

  rec.magic[0] = 'X';
  writeRecord(fs, HAM_CFG_FILE, rec);
  HamMode b; b.load(&fs);
  EXPECT_FALSE(b.isEnabled());

  rec = defaultRecord();
  rec.version = 2;
  writeRecord(fs, HAM_CFG_FILE, rec);
  HamMode c; c.load(&fs);
  EXPECT_FALSE(c.isEnabled());

  // Overlong record: valid bytes plus a trailing byte is refused.
  rec = defaultRecord();
  rec.enabled = 1;
  uint8_t buf[HAM_CFG_RECORD_BYTES + 1];
  memcpy(buf, &rec, HAM_CFG_PAYLOAD_BYTES);
  uint16_t crc = crc16_ccitt(buf, HAM_CFG_PAYLOAD_BYTES);
  memcpy(&buf[HAM_CFG_PAYLOAD_BYTES], &crc, 2);
  buf[HAM_CFG_RECORD_BYTES] = 0;
  fs.files[HAM_CFG_FILE] = std::vector<uint8_t>(buf, buf + sizeof(buf));
  HamMode d; d.load(&fs);
  EXPECT_FALSE(d.isEnabled());
}

TEST_F(HamTest, LazySaveTiming) {
  ham.setEnabled(true);
  ham.setCallsign("N0CALL");
  EXPECT_FALSE(fs.exists(HAM_CFG_FILE));
  ham.loop(0, &fs, "N0CALL Test");
  EXPECT_FALSE(fs.exists(HAM_CFG_FILE));   // not due yet

  g_mock_millis = LAZY_SAVE_DELAY_MS - 1;
  ham.loop(g_mock_millis, &fs, "N0CALL Test");
  EXPECT_FALSE(fs.exists(HAM_CFG_FILE));

  g_mock_millis = LAZY_SAVE_DELAY_MS;
  ham.loop(g_mock_millis, &fs, "N0CALL Test");
  EXPECT_TRUE(fs.exists(HAM_CFG_FILE));
}

TEST_F(HamTest, RecoversFromBackup) {
  HamCfgRecord rec = defaultRecord();
  rec.enabled = 1;
  strcpy(rec.callsign, "N0CALL");
  writeRecord(fs, HAM_CFG_FILE, rec);
  HamMode seeded; seeded.load(&fs);

  // A save rotates the good canonical into .bak before promoting the new one.
  seeded.setEnabled(false);
  seeded.save(&fs);
  ASSERT_TRUE(fs.exists("/ham_cfg.bak"));

  // A torn canonical falls back to the backup.
  fs.files[HAM_CFG_FILE].resize(3);
  HamMode recovered;
  recovered.load(&fs);
  EXPECT_TRUE(recovered.isEnabled());
  EXPECT_STREQ(recovered.getCallsign(), "N0CALL");
}

// ============================================================ station ID timer

TEST_F(HamTest, ArmsOnActivationAndResetsOnAirEveryAdvert) {
  ham.setEnabled(true);
  ham.setCallsign("N0CALL");
  ham.loop(1000, &fs, "N0CALL Test");       // false-to-true transition
  EXPECT_TRUE(ham.idDue(1000));             // immediately due
  EXPECT_EQ(ham.secondsToId(1000), 0u);

  ham.noteAdvertOnAir(1000);
  uint32_t next = 1000 + HAM_ID_INTERVAL_MS - HAM_ID_SEND_DELAY_MS;
  EXPECT_FALSE(ham.idDue(next - 1));
  EXPECT_TRUE(ham.idDue(next));
  EXPECT_EQ(ham.secondsToId(1000), (HAM_ID_INTERVAL_MS - HAM_ID_SEND_DELAY_MS) / 1000);
}

TEST_F(HamTest, ArmsFromBootPrefsOnFirstLoop) {
  HamCfgRecord rec = defaultRecord();
  rec.enabled = 1;
  strcpy(rec.callsign, "N0CALL");
  writeRecord(fs, HAM_CFG_FILE, rec);
  HamMode booted;
  booted.load(&fs);
  booted.loop(5000, &fs, "N0CALL Test");
  EXPECT_TRUE(booted.idDue(5000));
}

TEST_F(HamTest, RearmsWhenNameStartsMatching) {
  ham.setEnabled(true);
  ham.setCallsign("N0CALL");
  ham.loop(1000, &fs, "Other Node");         // inactive
  EXPECT_FALSE(ham.idDue(1000));
  ham.loop(2000, &fs, "N0CALL Test");        // name changed, now active
  EXPECT_TRUE(ham.idDue(2000));
}

TEST_F(HamTest, DeactivationStopsAndReactivationRearms) {
  ham.setEnabled(true);
  ham.setCallsign("N0CALL");
  ham.loop(1000, &fs, "N0CALL Test");
  ham.noteAdvertOnAir(1000);

  ham.setEnabled(false);
  ham.loop(2000, &fs, "N0CALL Test");
  EXPECT_FALSE(ham.idDue(2000 + HAM_ID_INTERVAL_MS));

  ham.setEnabled(true);
  ham.loop(3000, &fs, "N0CALL Test");
  EXPECT_TRUE(ham.idDue(3000));
}

TEST_F(HamTest, MillisWrapIsHandled) {
  ham.setEnabled(true);
  ham.setCallsign("N0CALL");
  g_mock_millis = 0xFFFFFFF0u;
  ham.loop(g_mock_millis, &fs, "N0CALL Test");
  ham.noteAdvertOnAir(g_mock_millis);
  uint32_t next = g_mock_millis + HAM_ID_INTERVAL_MS - HAM_ID_SEND_DELAY_MS;
  EXPECT_FALSE(ham.idDue(next - 1));
  EXPECT_TRUE(ham.idDue(next));
  EXPECT_EQ(ham.secondsToId(g_mock_millis),
            (HAM_ID_INTERVAL_MS - HAM_ID_SEND_DELAY_MS) / 1000);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
