// Behavior-level tests for the repeater's second radio preset (Radio2*):
// core profile metadata, policy matrix, timing, scheduler, persistence and CLI.
// Reached the same way a user or the firmware reaches the code.

#include <gtest/gtest.h>

#include <Arduino.h>

#include <algorithm>
#include <vector>

#include <helpers/StaticPoolPacketManager.h>

#include "CliUtil.h"
#include "Radio2.h"

namespace {

class FakeDevice : public Radio2Device {
public:
  bool in_recv = true;
  bool completed = false;
  bool in_progress = false;
  bool retune_ok = true;
  float capture = 6.0f;   // SX126x
  int noise = -117;
  int retunes = 0;

  bool retune(float, float, uint8_t, uint8_t, bool) override {
    retunes++;
    return retune_ok;
  }
  bool hasCompletedReceive() const override { return completed; }
  bool isReceiveInProgress() override { return in_progress; }
  bool isInRecvMode() const override { return in_recv; }
  int getNoiseFloor() const override { return noise; }
  float captureCostSymbols() const override { return capture; }
};

void setPrimary(Radio2& r) {
  r.setPrimary(910.525f, 125.0f, 10, 5);
}

// Scheduler tests drive loop() with a real (in-memory) store: a dirty config
// may fall due while the mock clock advances.
NativeFS scheduler_fs;

void writeRadio2Config(NativeFS& fs, const Radio2CfgRecord& rec) {
  uint8_t buf[RADIO2_CFG_RECORD_BYTES];
  memcpy(buf, &rec, RADIO2_CFG_PAYLOAD_BYTES);
  uint16_t crc = crc16_ccitt(buf, RADIO2_CFG_PAYLOAD_BYTES);
  memcpy(&buf[RADIO2_CFG_PAYLOAD_BYTES], &crc, 2);
  fs.files[RADIO2_CFG_FILE].assign(buf, buf + RADIO2_CFG_RECORD_BYTES);
}

mesh::Packet makePacket(uint8_t payload_type, uint8_t route_type) {
  mesh::Packet p;
  p.header = (uint8_t)((payload_type << PH_TYPE_SHIFT) | route_type);
  return p;
}

// Records the order of the calls checkSend() must make: select the TX profile
// before estimating airtime and before the CAD/receive check.
class OrderRadio : public mesh::Radio {
public:
  std::vector<int> order;
  bool select_ok = true;
  bool started = false;
  uint8_t selected = 0xff;

  int recvRaw(uint8_t*, int) override { order.push_back(0); return 0; }
  bool selectTxProfile(uint8_t profile) override {
    order.push_back(1);
    selected = profile;
    return select_ok;
  }
  uint32_t getEstAirtimeFor(int) override { order.push_back(2); return 100; }
  bool isReceiving() override { order.push_back(3); return false; }
  float packetScore(float, int) override { return 1.0f; }
  bool startSendRaw(const uint8_t*, int) override { started = true; return true; }
  bool isSendComplete() override { return true; }
  void onSendFinished() override {}
  bool isInRecvMode() const override { return true; }
};

class FixedClock : public mesh::MillisecondClock {
public:
  unsigned long now = 1;
  unsigned long getMillis() override { return now; }
};

class TestDispatcher : public mesh::Dispatcher {
public:
  TestDispatcher(mesh::Radio& r, mesh::MillisecondClock& ms, mesh::PacketManager& m)
      : Dispatcher(r, ms, m) {}
  mesh::DispatcherAction onRecvPacket(mesh::Packet*) override { return ACTION_RELEASE; }
};

}  // namespace

// ---------------------------------------------------------------- core packet

TEST(PacketProfile, SizeStays260) {
  EXPECT_EQ(sizeof(mesh::Packet), 260u);
}

TEST(PacketProfile, BitIsolation) {
  mesh::Packet p;
  EXPECT_EQ(p.getTxProfile(), RADIO_PROFILE_PRIMARY);
  EXPECT_EQ(p.getRxProfile(), RADIO_PROFILE_PRIMARY);

  p.setTxProfile(RADIO_PROFILE_SECONDARY);
  p.setRxProfile(2);   // unknown-ish value still fits the 2-bit field
  EXPECT_EQ(p.getTxProfile(), RADIO_PROFILE_SECONDARY);
  EXPECT_EQ(p.getRxProfile(), 2u);

  p.setRxProfile(RADIO_PROFILE_UNKNOWN);
  EXPECT_EQ(p.getTxProfile(), RADIO_PROFILE_SECONDARY);
  EXPECT_EQ(p.getRxProfile(), RADIO_PROFILE_UNKNOWN);
}

TEST(PacketProfile, WireFormatAndHashIgnoreProfile) {
  mesh::Packet a;
  a.header = (PAYLOAD_TYPE_ADVERT << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD;
  a.payload_len = 3;
  a.payload[0] = 1; a.payload[1] = 2; a.payload[2] = 3;
  a.setTxProfile(1);
  a.setRxProfile(1);

  mesh::Packet b = a;
  b.setTxProfile(0);
  b.setRxProfile(0);

  uint8_t ha[MAX_HASH_SIZE], hb[MAX_HASH_SIZE];
  a.calculatePacketHash(ha);
  b.calculatePacketHash(hb);
  EXPECT_EQ(memcmp(ha, hb, MAX_HASH_SIZE), 0);

  uint8_t raw[64];
  uint8_t len = a.writeTo(raw);
  mesh::Packet c;
  ASSERT_TRUE(c.readFrom(raw, len));
  EXPECT_EQ(c.getTxProfile(), RADIO_PROFILE_PRIMARY);
  EXPECT_EQ(c.getRxProfile(), RADIO_PROFILE_PRIMARY);
}

TEST(PacketQueue, PeekDoesNotRemove) {
  PacketQueue q(4);
  mesh::Packet low, high;
  low.header = 0;
  high.header = 0;
  ASSERT_TRUE(q.add(&low, 5, 0));
  ASSERT_TRUE(q.add(&high, 1, 0));

  EXPECT_EQ(q.peek(0), &high);
  EXPECT_EQ(q.count(), 2);
  EXPECT_EQ(q.get(0), &high);
  EXPECT_EQ(q.count(), 1);
  EXPECT_EQ(q.peek(0), &low);
}

TEST(PacketQueue, FutureEntriesAreInvisibleToPeek) {
  PacketQueue q(2);
  mesh::Packet p;
  ASSERT_TRUE(q.add(&p, 1, 100));
  EXPECT_EQ(q.peek(0), nullptr);
  EXPECT_EQ(q.peek(100), &p);
}

TEST(DispatcherProfiles, SelectsBeforeAirtimeAndCad) {
  OrderRadio radio;
  FixedClock clock;
  StaticPoolPacketManager mgr(4);
  TestDispatcher d(radio, clock, mgr);
  d.begin();
  clock.now = 2;

  mesh::Packet* p = d.obtainNewPacket();
  ASSERT_NE(p, nullptr);
  p->header = (PAYLOAD_TYPE_ADVERT << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD;
  p->setTxProfile(RADIO_PROFILE_SECONDARY);
  d.sendPacket(p, 1, 0);
  d.loop();

  EXPECT_EQ(radio.selected, RADIO_PROFILE_SECONDARY);
  EXPECT_TRUE(radio.started);
  auto select = std::find(radio.order.begin(), radio.order.end(), 1);
  auto airtime = std::find(radio.order.begin(), radio.order.end(), 2);
  auto cad = std::find(radio.order.begin(), radio.order.end(), 3);
  ASSERT_NE(select, radio.order.end());
  ASSERT_NE(airtime, radio.order.end());
  ASSERT_NE(cad, radio.order.end());
  EXPECT_LT(select, airtime);
  EXPECT_LT(airtime, cad);

  d.loop();   // complete and release the outbound packet
}

TEST(DispatcherProfiles, SelectFailureLeavesPacketQueued) {
  OrderRadio radio;
  FixedClock clock;
  StaticPoolPacketManager mgr(4);
  TestDispatcher d(radio, clock, mgr);
  d.begin();
  clock.now = 2;

  mesh::Packet* p = d.obtainNewPacket();
  ASSERT_NE(p, nullptr);
  p->header = (PAYLOAD_TYPE_ADVERT << PH_TYPE_SHIFT) | ROUTE_TYPE_FLOOD;
  p->setTxProfile(RADIO_PROFILE_SECONDARY);
  d.sendPacket(p, 1, 0);

  radio.select_ok = false;
  d.loop();
  EXPECT_FALSE(radio.started);
  EXPECT_EQ(mgr.getOutboundTotal(), 1);
}

TEST(DispatcherProfiles, ObtainNewPacketUsesDefault) {
  OrderRadio radio;
  FixedClock clock;
  StaticPoolPacketManager mgr(4);
  TestDispatcher d(radio, clock, mgr);
  d.setDefaultTxProfile(RADIO_PROFILE_SECONDARY);

  mesh::Packet* p = d.obtainNewPacket();
  ASSERT_NE(p, nullptr);
  EXPECT_EQ(p->getTxProfile(), RADIO_PROFILE_SECONDARY);
  EXPECT_EQ(p->getRxProfile(), RADIO_PROFILE_UNKNOWN);
  d.releasePacket(p);
}

TEST(PacketPool, FreeResetsProfileByte) {
  StaticPoolPacketManager mgr(1);
  mesh::Packet* p = mgr.allocNew();
  ASSERT_NE(p, nullptr);
  p->setTxProfile(RADIO_PROFILE_SECONDARY);
  p->setRxProfile(RADIO_PROFILE_SECONDARY);
  mgr.free(p);

  mesh::Packet* again = mgr.allocNew();
  ASSERT_EQ(again, p);
  EXPECT_EQ(again->radio_profiles, 0);
  mgr.free(again);
}

// ---------------------------------------------------------------- policy

TEST(Radio2Policy, MatrixTargets) {
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);

  mesh::Packet advert = makePacket(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD);
  r.setPolicy(RADIO2_ACTION_SAME, RADIO2_ACTION_SAME);
  EXPECT_EQ(r.actionFor(RADIO_PROFILE_PRIMARY, &advert), RADIO2_ACTION_SAME);
  EXPECT_EQ(r.targetFor(RADIO2_ACTION_SAME, RADIO_PROFILE_PRIMARY), RADIO_PROFILE_PRIMARY);
  EXPECT_EQ(r.targetFor(RADIO2_ACTION_SAME, RADIO_PROFILE_SECONDARY), RADIO_PROFILE_SECONDARY);

  r.setPolicy(RADIO2_ACTION_OTHER, RADIO2_ACTION_OTHER);
  EXPECT_EQ(r.targetFor(RADIO2_ACTION_OTHER, RADIO_PROFILE_PRIMARY), RADIO_PROFILE_SECONDARY);
  EXPECT_EQ(r.targetFor(RADIO2_ACTION_OTHER, RADIO_PROFILE_SECONDARY), RADIO_PROFILE_PRIMARY);

  r.setPolicy(RADIO2_ACTION_BOTH, RADIO2_ACTION_BOTH);
  EXPECT_EQ(r.targetFor(RADIO2_ACTION_BOTH, RADIO_PROFILE_PRIMARY), RADIO_PROFILE_PRIMARY);

  r.setPolicy(RADIO2_ACTION_DROP, RADIO2_ACTION_DROP);
  EXPECT_EQ(r.actionFor(RADIO_PROFILE_PRIMARY, &advert), RADIO2_ACTION_DROP);
}

TEST(Radio2Policy, OnlyBroadcastFloodCrosses) {
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  r.setPolicy(RADIO2_ACTION_OTHER, RADIO2_ACTION_OTHER);

  mesh::Packet advert = makePacket(PAYLOAD_TYPE_ADVERT, ROUTE_TYPE_FLOOD);
  mesh::Packet grp_txt = makePacket(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_FLOOD);
  mesh::Packet grp_data = makePacket(PAYLOAD_TYPE_GRP_DATA, ROUTE_TYPE_FLOOD);
  EXPECT_EQ(r.actionFor(RADIO_PROFILE_PRIMARY, &advert), RADIO2_ACTION_OTHER);
  EXPECT_EQ(r.actionFor(RADIO_PROFILE_PRIMARY, &grp_txt), RADIO2_ACTION_OTHER);
  EXPECT_EQ(r.actionFor(RADIO_PROFILE_PRIMARY, &grp_data), RADIO2_ACTION_OTHER);

  mesh::Packet direct_grp = makePacket(PAYLOAD_TYPE_GRP_TXT, ROUTE_TYPE_DIRECT);
  mesh::Packet unicast_flood = makePacket(PAYLOAD_TYPE_TXT_MSG, ROUTE_TYPE_FLOOD);
  mesh::Packet ack = makePacket(PAYLOAD_TYPE_ACK, ROUTE_TYPE_FLOOD);
  EXPECT_EQ(r.actionFor(RADIO_PROFILE_PRIMARY, &direct_grp), RADIO2_ACTION_SAME);
  EXPECT_EQ(r.actionFor(RADIO_PROFILE_PRIMARY, &unicast_flood), RADIO2_ACTION_SAME);
  EXPECT_EQ(r.actionFor(RADIO_PROFILE_PRIMARY, &ack), RADIO2_ACTION_SAME);

  // A non-crossable packet keeps an explicit drop.
  r.setPolicy(RADIO2_ACTION_DROP, RADIO2_ACTION_DROP);
  EXPECT_EQ(r.actionFor(RADIO_PROFILE_PRIMARY, &direct_grp), RADIO2_ACTION_DROP);
}

TEST(Radio2Policy, RepliesFollowOrigin) {
  FakeDevice dev;
  Radio2 r(dev);
  EXPECT_EQ(r.replyProfile(RADIO_PROFILE_PRIMARY), RADIO_PROFILE_PRIMARY);
  EXPECT_EQ(r.replyProfile(RADIO_PROFILE_SECONDARY), RADIO_PROFILE_SECONDARY);
  EXPECT_EQ(r.replyProfile(RADIO_PROFILE_UNKNOWN), RADIO_PROFILE_PRIMARY);
}

// ---------------------------------------------------------------- timing

TEST(Radio2Timing, KnownPresetMath) {
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  ASSERT_EQ(r.setPreset(915.0f, 125.0f, 10, 5), RADIO2_SET_OK);

  Radio2Timing t;
  ASSERT_TRUE(r.timing(&t));
  EXPECT_EQ(t.primary_capture_us, 49152u);
  EXPECT_EQ(t.secondary_capture_us, 49152u);
  EXPECT_EQ(t.primary_bound_us, 73920u);
  EXPECT_EQ(t.secondary_bound_us, 73920u);
  EXPECT_TRUE(t.usable);
}

TEST(Radio2Timing, UserPairOnSx126x) {
  FakeDevice dev;   // SX126x: 6-symbol capture cost
  Radio2 r(dev);
  r.setPrimary(910.525f, 62.5f, 7, 8);

  ASSERT_EQ(r.setPreset(911.5f, 500.0f, 10, 8), RADIO2_SET_OK);
  Radio2Timing t;
  ASSERT_TRUE(r.timing(&t));
  EXPECT_EQ(t.primary_capture_us, 12288u);
  EXPECT_EQ(t.secondary_bound_us, 12480u);
}

TEST(Radio2Timing, UserPairRejectedOnLr11x0) {
  FakeDevice dev;
  dev.capture = 8.0f;   // LR11x0
  Radio2 r(dev);
  r.setPrimary(910.525f, 62.5f, 7, 8);

  EXPECT_EQ(r.setPreset(911.5f, 500.0f, 10, 8), RADIO2_SET_UNSCANNABLE);
}

TEST(Radio2Timing, ReportsWhichBoundFailed) {
  NativeFS fs;
  FakeDevice dev;
  dev.capture = 8.0f;   // LR11x0
  Radio2 r(dev);

  // A persisted pair is only range-checked on load; the timing check happens
  // against the current radio and primary, so this can still be unusable.
  Radio2CfgRecord rec;
  memset(&rec, 0, sizeof(rec));
  memcpy(rec.magic, RADIO2_CFG_MAGIC, 4);
  rec.version = RADIO2_CFG_VERSION;
  rec.enabled = 1;
  rec.policy_primary = RADIO2_ACTION_SAME;
  rec.policy_secondary = RADIO2_ACTION_SAME;
  rec.advert = RADIO2_ADVERT_PRIMARY;
  rec.sf = 10;
  rec.cr = 8;
  rec.freq_mhz = 911.5f;
  rec.bw_khz = 500.0f;
  writeRadio2Config(fs, rec);
  r.load(&fs);
  ASSERT_EQ(r.getSecondaryProfile().freq, 911.5f);
  r.setPrimary(910.525f, 62.5f, 7, 8);

  char reply[CLI_REPLY_MAX];
  radio2CLI(r, false, false, "timing", reply);
  EXPECT_STREQ(reply, "unusable: primary capture 16ms > secondary-safe 8ms");
}

TEST(Radio2Timing, ReportsNoPreset) {
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);

  char reply[CLI_REPLY_MAX];
  radio2CLI(r, false, false, "timing", reply);
  EXPECT_STREQ(reply, "unusable: no preset configured");
}

TEST(Radio2Timing, ActiveAndIdleShares) {
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  ASSERT_EQ(r.setPreset(915.0f, 125.0f, 10, 5), RADIO2_SET_OK);
  ASSERT_TRUE(r.start(false, false));   // active mode from boot

  Radio2Timing t;
  ASSERT_TRUE(r.timing(&t));
  EXPECT_TRUE(t.active_mode);
  EXPECT_EQ(t.secondary_visit_us, 49152u);
  EXPECT_EQ(t.primary_visit_us, 4 * 49152u);
  EXPECT_TRUE(t.secondary_lossy);

  g_mock_millis += RADIO2_ACTIVE_HOLD_MS + 1;
  ASSERT_TRUE(r.timing(&t));
  EXPECT_FALSE(t.active_mode);
  EXPECT_EQ(t.primary_visit_us, 49152u);
  EXPECT_EQ(t.secondary_visit_us, t.primary_bound_us);
}

TEST(Radio2Timing, UnsafePairRejected) {
  FakeDevice dev;
  Radio2 r(dev);
  r.setPrimary(910.0f, 500.0f, 5, 5);   // fast, wide: tiny primary bound
  EXPECT_EQ(r.setPreset(915.0f, 125.0f, 12, 5), RADIO2_SET_UNSCANNABLE);

  // The refused preset was not committed, so the live pair is still unset.
  Radio2Timing t;
  EXPECT_FALSE(r.timing(&t));
  EXPECT_EQ(r.getSecondaryProfile().freq, 0.0f);
}

TEST(Radio2Timing, OverlapRejected) {
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  EXPECT_EQ(r.setPreset(910.6f, 125.0f, 10, 5), RADIO2_SET_OVERLAP);
  // A rejected preset leaves the previous one unchanged.
  EXPECT_EQ(r.getSecondaryProfile().freq, 0.0f);
}

TEST(Radio2Timing, SleepDeadline) {
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  r.setPreset(915.0f, 125.0f, 10, 5);
  r.start(false, false);

  EXPECT_GT(r.getSleepMs(), 0u);
  EXPECT_LE(r.getSleepMs(), RADIO2_SLEEP_MAX_MS);

  dev.completed = true;
  EXPECT_EQ(r.getSleepMs(), 1u);
  dev.completed = false;

  r.setEnabled(false);
  EXPECT_EQ(r.getSleepMs(), 0u);
}

// ---------------------------------------------------------------- scheduler

TEST(Radio2Isolation, DisabledIsInert) {
  g_mock_millis = 0;
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);

  EXPECT_FALSE(r.needsService());
  EXPECT_FALSE(r.start(false, false));
  EXPECT_FALSE(r.needsService());
  EXPECT_FALSE(r.isScanning());
  EXPECT_EQ(r.getSleepMs(), 0u);

  // A secondary-target packet falls back to primary without any retune.
  EXPECT_TRUE(r.selectTxProfile(RADIO_PROFILE_SECONDARY));
  EXPECT_EQ(dev.retunes, 0);

  r.loop(&scheduler_fs, true);
  EXPECT_EQ(dev.retunes, 0);
}

TEST(Radio2Isolation, DisableSettlesBackToIdle) {
  g_mock_millis = 0;
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  ASSERT_EQ(r.setPreset(915.0f, 125.0f, 10, 5), RADIO2_SET_OK);
  g_mock_millis += RADIO2_APPLY_DELAY_MS;
  r.loop(&scheduler_fs, true);
  ASSERT_TRUE(r.isScanning());

  r.setEnabled(false);
  g_mock_millis += RADIO2_APPLY_DELAY_MS;
  r.loop(&scheduler_fs, true);
  EXPECT_FALSE(r.isScanning());
  EXPECT_EQ(r.currentProfile(), RADIO_PROFILE_PRIMARY);

  g_mock_millis += LAZY_SAVE_DELAY_MS + 1;   // let the config write finish
  r.loop(&scheduler_fs, true);
  EXPECT_FALSE(r.needsService());   // nothing left to service while off
}

TEST(Radio2Isolation, PausedDoesNotAdvertiseOnBoth) {
  g_mock_millis = 0;
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  ASSERT_EQ(r.setPreset(915.0f, 125.0f, 10, 5), RADIO2_SET_OK);
  g_mock_millis += RADIO2_APPLY_DELAY_MS;
  r.loop(&scheduler_fs, true);
  r.setAdvert(RADIO2_ADVERT_BOTH);
  ASSERT_TRUE(r.advertBoth());

  r.pauseForPrimaryChange();
  EXPECT_FALSE(r.advertBoth());
  EXPECT_TRUE(r.needsService());
}

TEST(Radio2Scheduler, DisabledDoesNotStart) {
  g_mock_millis = 0;
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  ASSERT_EQ(r.setPreset(915.0f, 125.0f, 10, 5), RADIO2_SET_OK);
  r.setAdvert(RADIO2_ADVERT_BOTH);
  r.setEnabled(false);

  EXPECT_FALSE(r.start(false, false));
  EXPECT_FALSE(r.isScanning());
  EXPECT_FALSE(r.advertBoth());   // a disabled scanner must not duplicate adverts
  r.loop(&scheduler_fs, true);
  EXPECT_FALSE(r.isScanning());
}

TEST(Radio2Scheduler, AlternatesProfiles) {
  g_mock_millis = 0;
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  r.setPreset(915.0f, 125.0f, 10, 5);
  ASSERT_TRUE(r.start(false, false));

  g_mock_millis += 200;   // past the active primary visit
  r.loop(&scheduler_fs, true);
  EXPECT_EQ(r.currentProfile(), RADIO_PROFILE_SECONDARY);
  EXPECT_EQ(dev.retunes, 1);

  g_mock_millis += 50;    // past the active secondary visit
  r.loop(&scheduler_fs, true);
  EXPECT_EQ(r.currentProfile(), RADIO_PROFILE_PRIMARY);
  EXPECT_EQ(dev.retunes, 2);
}

TEST(Radio2Scheduler, PrimaryInProgressDefers) {
  g_mock_millis = 0;
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  r.setPreset(915.0f, 125.0f, 10, 5);
  ASSERT_TRUE(r.start(false, false));

  dev.in_progress = true;
  g_mock_millis += 200;
  r.loop(&scheduler_fs, true);
  EXPECT_EQ(r.currentProfile(), RADIO_PROFILE_PRIMARY);

  dev.in_progress = false;
  g_mock_millis += 2;
  r.loop(&scheduler_fs, true);
  EXPECT_EQ(r.currentProfile(), RADIO_PROFILE_SECONDARY);
}

TEST(Radio2Scheduler, ActiveAbortsSecondaryReceive) {
  g_mock_millis = 0;
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  r.setPreset(915.0f, 125.0f, 10, 5);
  ASSERT_TRUE(r.start(false, false));

  g_mock_millis += 200;
  r.loop(&scheduler_fs, true);
  ASSERT_EQ(r.currentProfile(), RADIO_PROFILE_SECONDARY);

  dev.in_progress = true;
  g_mock_millis += 50;
  r.loop(&scheduler_fs, true);
  EXPECT_EQ(r.currentProfile(), RADIO_PROFILE_PRIMARY);
  EXPECT_EQ(r.getAborts(), 1u);
}

TEST(Radio2Scheduler, IdleHoldsSecondaryReceive) {
  g_mock_millis = 0;
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  r.setPreset(915.0f, 125.0f, 10, 5);
  ASSERT_TRUE(r.start(false, false));

  g_mock_millis += RADIO2_ACTIVE_HOLD_MS + 1;   // idle
  r.loop(&scheduler_fs, true);                        // primary -> secondary
  ASSERT_EQ(r.currentProfile(), RADIO_PROFILE_SECONDARY);

  dev.in_progress = true;
  g_mock_millis += 100;
  r.loop(&scheduler_fs, true);
  EXPECT_EQ(r.currentProfile(), RADIO_PROFILE_SECONDARY);   // held
  EXPECT_EQ(r.getAborts(), 0u);
}

TEST(Radio2Scheduler, TxBlocksSwitching) {
  g_mock_millis = 0;
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  r.setPreset(915.0f, 125.0f, 10, 5);
  ASSERT_TRUE(r.start(false, false));

  dev.in_recv = false;   // TX in progress
  g_mock_millis += 200;
  r.loop(&scheduler_fs, true);
  EXPECT_EQ(r.currentProfile(), RADIO_PROFILE_PRIMARY);
  EXPECT_EQ(dev.retunes, 0);
}

TEST(Radio2Scheduler, SwitchFailureDisablesScanning) {
  g_mock_millis = 0;
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  r.setPreset(915.0f, 125.0f, 10, 5);
  ASSERT_TRUE(r.start(false, false));

  dev.retune_ok = false;
  for (uint32_t i = 0; i < RADIO2_MAX_SWITCH_FAILURES + 2; i++) {
    g_mock_millis += RADIO2_SWITCH_RETRY_MS + 100;
    r.loop(&scheduler_fs, true);
  }
  EXPECT_FALSE(r.isScanning());
  EXPECT_EQ(r.currentProfile(), RADIO_PROFILE_PRIMARY);
  EXPECT_EQ(r.getSwitchErrors(), RADIO2_MAX_SWITCH_FAILURES);
}

TEST(Radio2Scheduler, PrimaryPacketStartsActiveMode) {
  g_mock_millis = 0;
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  r.setPreset(915.0f, 125.0f, 10, 5);
  ASSERT_TRUE(r.start(false, false));

  g_mock_millis += RADIO2_ACTIVE_HOLD_MS + 1;
  Radio2Timing t;
  r.timing(&t);
  EXPECT_FALSE(t.active_mode);
  r.notePrimaryPacket();
  r.timing(&t);
  EXPECT_TRUE(t.active_mode);
}

// ---------------------------------------------------------------- persistence

TEST(Radio2Persistence, Roundtrip) {
  NativeFS fs;
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  ASSERT_EQ(r.setPreset(915.0f, 125.0f, 10, 5), RADIO2_SET_OK);
  r.setPolicy(RADIO2_ACTION_OTHER, RADIO2_ACTION_DROP);
  r.setAdvert(RADIO2_ADVERT_BOTH);
  r.save(&fs);

  Radio2 loaded(dev);
  loaded.begin(&fs);
  EXPECT_TRUE(loaded.isEnabled());
  EXPECT_EQ(loaded.getSecondaryProfile().freq, 915.0f);
  EXPECT_EQ(loaded.getSecondaryProfile().bw, 125.0f);
  EXPECT_EQ(loaded.getSecondaryProfile().sf, 10);
  EXPECT_EQ(loaded.getSecondaryProfile().cr, 5);
  EXPECT_EQ(loaded.getPolicyPrimary(), RADIO2_ACTION_OTHER);
  EXPECT_EQ(loaded.getPolicySecondary(), RADIO2_ACTION_DROP);
  EXPECT_EQ(loaded.getAdvert(), RADIO2_ADVERT_BOTH);
}

TEST(Radio2Persistence, DisabledKeepsPreset) {
  NativeFS fs;
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  ASSERT_EQ(r.setPreset(915.0f, 125.0f, 10, 5), RADIO2_SET_OK);
  r.setEnabled(false);
  r.save(&fs);

  Radio2 loaded(dev);
  loaded.begin(&fs);
  EXPECT_FALSE(loaded.isEnabled());
  EXPECT_EQ(loaded.getSecondaryProfile().freq, 915.0f);
}

TEST(Radio2Persistence, TruncatedRejected) {
  NativeFS fs;
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  ASSERT_EQ(r.setPreset(915.0f, 125.0f, 10, 5), RADIO2_SET_OK);
  r.save(&fs);

  fs.files[RADIO2_CFG_FILE].resize(10);
  Radio2 loaded(dev);
  loaded.begin(&fs);
  EXPECT_FALSE(loaded.isEnabled());
  EXPECT_EQ(loaded.getSecondaryProfile().freq, 0.0f);
}

TEST(Radio2Persistence, BadCrcRejected) {
  NativeFS fs;
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  ASSERT_EQ(r.setPreset(915.0f, 125.0f, 10, 5), RADIO2_SET_OK);
  r.save(&fs);

  fs.files[RADIO2_CFG_FILE][5] ^= 0xFF;
  Radio2 loaded(dev);
  loaded.begin(&fs);
  EXPECT_FALSE(loaded.isEnabled());
}

TEST(Radio2Persistence, BackupRecovery) {
  NativeFS fs;
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  ASSERT_EQ(r.setPreset(915.0f, 125.0f, 10, 5), RADIO2_SET_OK);

  r.save(&fs);                        // canonical: 915
  ASSERT_EQ(r.setPreset(916.0f, 125.0f, 10, 5), RADIO2_SET_OK);
  r.save(&fs);                        // canonical: 916, backup: 915
  ASSERT_TRUE(fs.exists("/radio2_cfg.bak"));

  fs.files[RADIO2_CFG_FILE].resize(3);
  Radio2 loaded(dev);
  loaded.begin(&fs);
  EXPECT_TRUE(loaded.isEnabled());
  EXPECT_EQ(loaded.getSecondaryProfile().freq, 915.0f);
}

// ---------------------------------------------------------------- CLI

TEST(Radio2Cli, StatusOff) {
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  char reply[CLI_REPLY_MAX];
  radio2CLI(r, false, false, "", reply);
  EXPECT_STREQ(reply, "off");
}

TEST(Radio2Cli, SetPresetAndStatus) {
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  char reply[CLI_REPLY_MAX];

  radio2CLI(r, false, false, "915,125,10,5", reply);
  EXPECT_STREQ(reply, "OK - radio2 915,125,10,5");
  EXPECT_TRUE(r.isEnabled());

  radio2CLI(r, false, false, "status", reply);
  EXPECT_NE(strstr(reply, "apply=pending"), nullptr);

  g_mock_millis += RADIO2_APPLY_DELAY_MS;
  r.loop(&scheduler_fs, true);
  EXPECT_TRUE(r.isScanning());

  radio2CLI(r, false, false, "status", reply);
  EXPECT_NE(strstr(reply, "on; "), nullptr);
  EXPECT_NE(strstr(reply, "at="), nullptr);
  EXPECT_LE(strlen(reply), (size_t)CLI_REPLY_MAX - 1);
}

TEST(Radio2Cli, PolicyAndAdvert) {
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  char reply[CLI_REPLY_MAX];

  radio2CLI(r, false, false, "policy", reply);
  EXPECT_STREQ(reply, "> primary=same,secondary=same");

  radio2CLI(r, false, false, "policy other", reply);
  EXPECT_STREQ(reply, "OK - radio2 policy primary=other,secondary=other");

  radio2CLI(r, false, false, "policy other drop", reply);
  EXPECT_STREQ(reply, "OK - radio2 policy primary=other,secondary=drop");

  radio2CLI(r, false, false, "policy nope", reply);
  EXPECT_STREQ(reply, "Err - radio2 policy must be same|other|both|drop");

  radio2CLI(r, false, false, "advert", reply);
  EXPECT_STREQ(reply, "> primary");
  radio2CLI(r, false, false, "advert both", reply);
  EXPECT_STREQ(reply, "OK - radio2 advert both");
  radio2CLI(r, false, false, "advert nope", reply);
  EXPECT_STREQ(reply, "Err - radio2 advert must be primary|both");
}

TEST(Radio2Cli, Off) {
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  char reply[CLI_REPLY_MAX];

  radio2CLI(r, false, false, "915,125,10,5", reply);
  radio2CLI(r, false, false, "off", reply);
  EXPECT_STREQ(reply, "OK - radio2 off");
  EXPECT_FALSE(r.isEnabled());
  g_mock_millis += RADIO2_APPLY_DELAY_MS;
  r.loop(&scheduler_fs, true);
  EXPECT_FALSE(r.isScanning());
}

TEST(Radio2Cli, Errors) {
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  char reply[CLI_REPLY_MAX];

  radio2CLI(r, false, false, "915,125", reply);
  EXPECT_STREQ(reply, "Err - radio2 preset must be <freq>,<bw>,<sf>,<cr>");

  radio2CLI(r, false, false, "915,125,10,5", reply);
  EXPECT_STREQ(reply, "OK - radio2 915,125,10,5");

  radio2CLI(r, false, false, "910.6,125,10,5", reply);
  EXPECT_STREQ(reply, "Err - radio2 overlaps radio");

  radio2CLI(r, true, false, "916,125,10,5", reply);
  EXPECT_STREQ(reply, "Err - radio.rxps is on; set radio.rxps off first");

  radio2CLI(r, false, true, "916,125,10,5", reply);
  EXPECT_STREQ(reply, "Err - radio2 conflicts with extra.sf; clear extra SF first");

  radio2CLI(r, false, false, "status junk", reply);
  EXPECT_EQ(strncmp(reply, "Err - usage:", 12), 0);
  EXPECT_LE(strlen(reply), (size_t)CLI_REPLY_MAX - 1);
}

TEST(Radio2Cli, Timing) {
  FakeDevice dev;
  Radio2 r(dev);
  setPrimary(r);
  char reply[CLI_REPLY_MAX];
  radio2CLI(r, false, false, "915,125,10,5", reply);
  radio2CLI(r, false, false, "timing", reply);
  EXPECT_NE(strstr(reply, "primary="), nullptr);
  EXPECT_NE(strstr(reply, "secondary="), nullptr);
  EXPECT_LE(strlen(reply), (size_t)CLI_REPLY_MAX - 1);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
