// Native tests for mesh clock sync (MeshClockSync): batch collection, the
// median/quorum decision, wrap safety, the enabled-flag persistence and the CLI.
// Reach the feature only through its public API exactly as MyMesh would.

#include <gtest/gtest.h>

#include <vector>

#include <Arduino.h>        // g_mock_millis (NativeShim.h is force-included)
#include <string.h>

#include "MeshClockSync.h"
#include "CliUtil.h"

// Two plausible-but-wrong clocks: the node's own is far behind, and the true
// time carried by adverts is well above any build floor.
static const uint32_t TRUE_NOW  = 1800000000UL;   // 2027-01-15 UTC
static const uint32_t WRONG_NOW = 1700000000UL;   // 2023-11-14 UTC

// ------------------------------------------------------------------ helpers

// A distinct advert key per source id: the fingerprint samples pubkey bytes
// 8/14/20/26, so encoding the id there keeps sources distinct.
static void makeKey(uint8_t* key, int id) {
  memset(key, 0, PUB_KEY_SIZE);
  key[8]  = (uint8_t)id;
  key[14] = (uint8_t)(id + 1);
  key[20] = (uint8_t)(id + 2);
  key[26] = (uint8_t)(id + 3);
}

static void feed(MeshClockSync& c, int id, uint32_t advert_ts, uint32_t local_now) {
  uint8_t key[PUB_KEY_SIZE];
  makeKey(key, id);
  c.onAdvert(advert_ts, key, local_now);
}

// ------------------------------------------------------------------ fixture

struct ClockTest : public ::testing::Test {
  NativeFS fs;
  MeshClockSync clock;

  uint32_t set_calls = 0;
  uint32_t set_epoch = 0;

  static void onSet(void* ctx, uint32_t epoch) {
    ClockTest* self = (ClockTest*)ctx;
    self->set_calls++;
    self->set_epoch = epoch;
  }

  void SetUp() override {
    g_mock_millis = 1000;
    clock.setClockFn(onSet, this);
  }
  void TearDown() override { g_mock_millis = 0; }

  // flush a dirty save: edits mark the config dirty, loop() writes it once
  // LAZY_SAVE_DELAY_MS has passed
  void flushSave() {
    g_mock_millis += LAZY_SAVE_DELAY_MS + 1;
    clock.loop(&fs);
  }
};

// ------------------------------------------------------------------ defaults

TEST_F(ClockTest, DefaultsAreDisabledAndEmpty) {
  EXPECT_FALSE(clock.isEnabled());
  EXPECT_EQ(clock.getSampleCount(), 0);
  EXPECT_EQ(clock.getLastResult(), MeshClockSync::IDLE);

  clock.begin(&fs);   // no config file: defaults survive
  EXPECT_FALSE(clock.isEnabled());
  EXPECT_EQ(clock.getSampleCount(), 0);
  EXPECT_EQ(clock.getLastResult(), MeshClockSync::IDLE);
}

// ------------------------------------------------------------ build epoch

TEST(ClockSyncBuildEpoch, ParsesAReasonableDate) {
  // Read the value through buildFloor(), buildEpoch()'s sole production caller.
  // Calling buildEpoch() directly here expands this TU's own __TIME__, which can
  // differ from MeshClockSync.cpp's by a second when the two TUs compile apart
  // (each call is folded per-TU at -O1).
  const uint32_t e = MeshClockSync::buildFloor();
  EXPECT_GT(e, 1735689600UL);   // after 2025-01-01
  EXPECT_LT(e, 4102444800UL);   // before 2100-01-01
}

// ------------------------------------------------------------ persistence

TEST_F(ClockTest, EnableRoundTrips) {
  clock.setEnabled(true);
  EXPECT_TRUE(clock.isEnabled());
  flushSave();

  MeshClockSync loaded;
  loaded.begin(&fs);
  EXPECT_TRUE(loaded.isEnabled());

  loaded.setEnabled(false);
  g_mock_millis += LAZY_SAVE_DELAY_MS + 1;
  loaded.loop(&fs);

  MeshClockSync reloaded;
  reloaded.begin(&fs);
  EXPECT_FALSE(reloaded.isEnabled());
}

TEST_F(ClockTest, SameValueSetIsNoOp) {
  clock.setEnabled(false);   // already disabled: no dirty write
  flushSave();
  EXPECT_FALSE(fs.exists(MESHCLOCK_CFG_FILE));
}

TEST_F(ClockTest, SaveOnlyWhenDirtyAndDue) {
  clock.setEnabled(true);
  clock.loop(&fs);   // dirty but not due yet
  EXPECT_FALSE(fs.exists(MESHCLOCK_CFG_FILE));
  flushSave();
  EXPECT_TRUE(fs.exists(MESHCLOCK_CFG_FILE));
}

TEST_F(ClockTest, CorruptedCrcRejected) {
  clock.setEnabled(true);
  flushSave();
  fs.files[MESHCLOCK_CFG_FILE][3] ^= 0x01;   // CRC high byte
  MeshClockSync loaded;
  loaded.begin(&fs);
  EXPECT_FALSE(loaded.isEnabled());
}

TEST_F(ClockTest, BadVersionRejected) {
  clock.setEnabled(true);
  flushSave();
  fs.files[MESHCLOCK_CFG_FILE][0] = 99;
  MeshClockSync loaded;
  loaded.begin(&fs);
  EXPECT_FALSE(loaded.isEnabled());
}

TEST_F(ClockTest, BadEnabledFlagRejected) {
  clock.setEnabled(true);
  flushSave();
  // patch the flag and recompute the CRC, so the enabled-range check is what
  // rejects the record rather than the checksum
  std::vector<uint8_t>& rec = fs.files[MESHCLOCK_CFG_FILE];
  rec[1] = 2;
  uint16_t crc = crc16_ccitt(rec.data(), 2);
  memcpy(&rec[2], &crc, 2);
  MeshClockSync loaded;
  loaded.begin(&fs);
  EXPECT_FALSE(loaded.isEnabled());
}

TEST_F(ClockTest, TruncatedFileRejected) {
  clock.setEnabled(true);
  flushSave();
  fs.files[MESHCLOCK_CFG_FILE].resize(2);   // short of the 4-byte record
  MeshClockSync loaded;
  loaded.begin(&fs);
  EXPECT_FALSE(loaded.isEnabled());
}

TEST_F(ClockTest, OverlongFileRejected) {
  clock.setEnabled(true);
  flushSave();
  fs.files[MESHCLOCK_CFG_FILE].push_back(0);   // record must be exactly 4 bytes
  MeshClockSync loaded;
  loaded.begin(&fs);
  EXPECT_FALSE(loaded.isEnabled());
}

TEST_F(ClockTest, BackupRecoveredAndRepaired) {
  clock.setEnabled(true);
  flushSave();   // canonical: enabled
  clock.setEnabled(false);
  flushSave();   // canonical: disabled; backup: enabled

  fs.files[MESHCLOCK_CFG_FILE][0] = 99;   // corrupt the canonical
  MeshClockSync loaded;
  loaded.begin(&fs);
  EXPECT_TRUE(loaded.isEnabled());   // recovered from the backup

  std::vector<uint8_t> before = fs.files[MESHCLOCK_CFG_FILE];
  loaded.loop(&fs);
  EXPECT_EQ(fs.files[MESHCLOCK_CFG_FILE], before) << "repair save must wait out the lazy delay";
  g_mock_millis += LAZY_SAVE_DELAY_MS + 1;
  loaded.loop(&fs);
  ASSERT_NE(fs.files[MESHCLOCK_CFG_FILE], before);

  MeshClockSync reloaded;
  reloaded.begin(&fs);
  EXPECT_TRUE(reloaded.isEnabled());
}

// ------------------------------------------------------------ collection

TEST_F(ClockTest, RejectsBeforeBuildFloor) {
  clock.setEnabled(true);
  const uint32_t floor = MeshClockSync::buildFloor();

  feed(clock, 0, floor - 1, TRUE_NOW);   // before the firmware existed
  EXPECT_EQ(clock.getSampleCount(), 0);

  feed(clock, 1, 1715770351UL, TRUE_NOW);   // 15 May 2024 fallback clock
  EXPECT_EQ(clock.getSampleCount(), 0);
}

TEST_F(ClockTest, TimestampAtFloorAccepted) {
  clock.setEnabled(true);
  const uint32_t floor = MeshClockSync::buildFloor();

  feed(clock, 0, floor, TRUE_NOW);
  EXPECT_EQ(clock.getSampleCount(), 1);   // at the floor is not before it
  feed(clock, 1, floor - 1, TRUE_NOW);
  EXPECT_EQ(clock.getSampleCount(), 1);   // rejected
}

TEST_F(ClockTest, OneVotePerSource) {
  clock.setEnabled(true);
  uint8_t key[PUB_KEY_SIZE];
  makeKey(key, 5);
  clock.onAdvert(TRUE_NOW, key, WRONG_NOW);
  clock.onAdvert(TRUE_NOW + 10, key, WRONG_NOW);   // same source: dropped
  EXPECT_EQ(clock.getSampleCount(), 1);
}

TEST_F(ClockTest, DisabledIgnoresAdverts) {
  feed(clock, 0, TRUE_NOW, WRONG_NOW);
  EXPECT_EQ(clock.getSampleCount(), 0);
}

// ------------------------------------------------------------ decisions

TEST_F(ClockTest, FullyAgreeingBatchSetsClock) {
  clock.setEnabled(true);
  for (int i = 0; i < MESHCLOCK_SLOTS - 1; i++) feed(clock, i, TRUE_NOW, WRONG_NOW);
  EXPECT_EQ(clock.getSampleCount(), MESHCLOCK_SLOTS - 1);
  EXPECT_EQ(set_calls, 0u);   // decides only when the batch fills

  feed(clock, MESHCLOCK_SLOTS - 1, TRUE_NOW, WRONG_NOW);
  EXPECT_EQ(set_calls, 1u);
  EXPECT_EQ(set_epoch, TRUE_NOW);
  EXPECT_EQ(clock.getLastResult(), MeshClockSync::SET_CLOCK);
  EXPECT_EQ(clock.getSampleCount(), 0);   // batch cleared after the decision
}

TEST_F(ClockTest, MedianIgnoresMinorityOutliers) {
  clock.setEnabled(true);
  for (int i = 0; i < MESHCLOCK_SLOTS - 3; i++) feed(clock, i, TRUE_NOW, WRONG_NOW);
  for (int i = MESHCLOCK_SLOTS - 3; i < MESHCLOCK_SLOTS; i++) {
    feed(clock, i, TRUE_NOW + 10000000UL, WRONG_NOW);   // wildly wrong minority
  }
  EXPECT_EQ(set_calls, 1u);
  EXPECT_EQ(set_epoch, TRUE_NOW);   // majority value, not dragged to the outliers
}

TEST_F(ClockTest, QuorumNotMetClears) {
  clock.setEnabled(true);
  const int outliers = MESHCLOCK_SLOTS - (MESHCLOCK_MIN_AGREE - 1);
  for (int i = 0; i < MESHCLOCK_MIN_AGREE - 1; i++) feed(clock, i, TRUE_NOW, WRONG_NOW);
  for (int i = 0; i < outliers; i++) {
    feed(clock, MESHCLOCK_MIN_AGREE - 1 + i, TRUE_NOW + 5000000UL, WRONG_NOW);
  }
  EXPECT_EQ(set_calls, 0u);
  EXPECT_EQ(clock.getLastResult(), MeshClockSync::NO_CONSENSUS);
  EXPECT_EQ(clock.getSampleCount(), 0);
}

TEST_F(ClockTest, CloseEnoughIsInSync) {
  clock.setEnabled(true);
  const uint32_t local = TRUE_NOW;
  for (int i = 0; i < MESHCLOCK_SLOTS; i++) feed(clock, i, local + 10, local);
  EXPECT_EQ(set_calls, 0u);
  EXPECT_EQ(clock.getLastResult(), MeshClockSync::IN_SYNC);
  EXPECT_EQ(clock.getLastDeltaSecs(), 10);
  EXPECT_EQ(clock.getSampleCount(), 0);
}

// The deadband is "within 60 s", so a difference of exactly
// MESHCLOCK_MIN_DIFF_SECS is still in sync (the knob is the minimum
// difference that counts as out of sync).
TEST_F(ClockTest, TheDeadbandBoundaryIsInclusive) {
  clock.setEnabled(true);
  const uint32_t local = TRUE_NOW;
  for (int i = 0; i < MESHCLOCK_SLOTS; i++) {
    feed(clock, i, local + MESHCLOCK_MIN_DIFF_SECS, local);
  }
  EXPECT_EQ(set_calls, 0u);
  EXPECT_EQ(clock.getLastResult(), MeshClockSync::IN_SYNC);

  // one second more is out of sync: the clock is stepped
  for (int i = 0; i < MESHCLOCK_SLOTS; i++) {
    feed(clock, i, local + MESHCLOCK_MIN_DIFF_SECS + 1, local);
  }
  EXPECT_EQ(set_calls, 1u);
  EXPECT_EQ(clock.getLastResult(), MeshClockSync::SET_CLOCK);
}

TEST_F(ClockTest, NegativeOffsetReconstructsAcrossWrap) {
  clock.setEnabled(true);
  // within 2^31 of the build floor, so it survives the signed floor check, but
  // local + offset still wraps around UINT32_MAX
  const uint32_t target = 3800000000UL;   // ~2090 UTC
  const uint32_t local = target + 500;    // local clock ahead, so offsets wrap
  for (int i = 0; i < MESHCLOCK_SLOTS; i++) feed(clock, i, target, local);
  EXPECT_EQ(set_calls, 1u);
  EXPECT_EQ(set_epoch, target);
  EXPECT_EQ(clock.getLastDeltaSecs(), -500);
}

TEST_F(ClockTest, ClockStepMidBatchFailsConsensus) {
  clock.setEnabled(true);
  const uint32_t l1 = TRUE_NOW;
  const uint32_t l2 = l1 + 1000;   // external step, larger than the tolerance

  // MIN_AGREE-1 stale samples, then fresh ones: the stale cluster cannot reach
  // quorum (see the documented mid-batch-step limitation)
  for (int i = 0; i < MESHCLOCK_MIN_AGREE - 1; i++) feed(clock, i, TRUE_NOW, l1);
  for (int i = MESHCLOCK_MIN_AGREE - 1; i < MESHCLOCK_SLOTS; i++) feed(clock, i, TRUE_NOW, l2);

  EXPECT_EQ(set_calls, 0u);
  EXPECT_EQ(clock.getLastResult(), MeshClockSync::NO_CONSENSUS);
  EXPECT_EQ(clock.getSampleCount(), 0);
}

TEST_F(ClockTest, ResetClearsBatchAndFlag) {
  clock.setEnabled(true);
  feed(clock, 0, TRUE_NOW, WRONG_NOW);
  EXPECT_EQ(clock.getSampleCount(), 1);
  clock.reset();
  EXPECT_FALSE(clock.isEnabled());
  EXPECT_EQ(clock.getSampleCount(), 0);
  EXPECT_EQ(clock.getLastResult(), MeshClockSync::IDLE);
}

// ------------------------------------------------------------------ CLI

TEST_F(ClockTest, CliSurface) {
  char reply[CLI_REPLY_MAX];

  meshClockCLI(clock, "", reply);
  EXPECT_STREQ(reply, "off");

  meshClockCLI(clock, "on", reply);
  EXPECT_STREQ(reply, "OK - mesh clock sync on");
  EXPECT_TRUE(clock.isEnabled());

  meshClockCLI(clock, "", reply);
  EXPECT_STREQ(reply, "on samples=0/33 last=idle");

  meshClockCLI(clock, "bogus", reply);
  EXPECT_STREQ(reply, "Err - clock mesh takes on|off");

  meshClockCLI(clock, "on extra", reply);
  EXPECT_STREQ(reply, "Err - clock mesh takes on|off");
  EXPECT_TRUE(clock.isEnabled());   // a refused command changed nothing

  meshClockCLI(clock, "off", reply);
  EXPECT_STREQ(reply, "OK - mesh clock sync off");
  EXPECT_FALSE(clock.isEnabled());
}

TEST_F(ClockTest, CliStatusShowsLastResult) {
  clock.setEnabled(true);
  for (int i = 0; i < MESHCLOCK_SLOTS; i++) feed(clock, i, TRUE_NOW, WRONG_NOW);
  char reply[CLI_REPLY_MAX];
  meshClockCLI(clock, "", reply);
  EXPECT_STREQ(reply, "on samples=0/33 last=set(+100000000s)");
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
