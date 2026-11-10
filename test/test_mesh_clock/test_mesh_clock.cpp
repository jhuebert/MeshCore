// Native tests for mesh clock sync (MeshClockSync): batch collection, the
// median/quorum decision, wrap safety, the enabled-flag persistence and the CLI.
// Reach the feature only through its public API exactly as MyMesh would.

#include <gtest/gtest.h>

#include <vector>
#include <stddef.h>

#include <Arduino.h>        // g_mock_millis (NativeShim.h is force-included)
#include <string.h>

#include "MeshClockSync.h"
#include "CliUtil.h"

// Two plausible-but-wrong clocks: the node's own is far behind, and the true
// time carried by adverts is well above any build floor.
static const uint32_t TRUE_NOW  = 1800000000UL;   // 2027-01-15 UTC
static const uint32_t WRONG_NOW = 1700000000UL;   // 2023-11-14 UTC

// the node time the CLI is handed, already formatted as an ISO 8601 UTC
// timestamp ("YYYY-MM-DDTHH:MM:SSZ")
static const char* NOW_TEXT = "2026-11-01T14:05:00Z";

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

// Hand-craft a v1 /meshclock record (enabled flag only, as the shipped
// firmware wrote it). `path` lets a test place one at the backup path.
static void writeV1File(NativeFS& fs, uint8_t enabled, const char* path) {
  std::vector<uint8_t> rec = { 1, enabled, 0, 0 };
  uint16_t crc = crc16_ccitt(rec.data(), 2);
  memcpy(&rec[2], &crc, 2);
  fs.files[path] = rec;
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
  fs.files[MESHCLOCK_CFG_FILE][9] ^= 0x01;   // CRC high byte
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
  uint16_t crc = crc16_ccitt(rec.data(), offsetof(MeshClockCfg, crc));
  memcpy(&rec[8], &crc, 2);
  MeshClockSync loaded;
  loaded.begin(&fs);
  EXPECT_FALSE(loaded.isEnabled());
}

TEST_F(ClockTest, TruncatedFileRejected) {
  clock.setEnabled(true);
  flushSave();
  fs.files[MESHCLOCK_CFG_FILE].resize(2);   // short of the 10-byte record
  MeshClockSync loaded;
  loaded.begin(&fs);
  EXPECT_FALSE(loaded.isEnabled());
}

TEST_F(ClockTest, OverlongFileRejected) {
  clock.setEnabled(true);
  flushSave();
  fs.files[MESHCLOCK_CFG_FILE].push_back(0);   // record must be exactly 10 bytes
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

  meshClockCLI(clock, NOW_TEXT, "", reply);
  EXPECT_STREQ(reply, "off slots=33/22 tol=60/60 now=2026-11-01T14:05:00Z");

  meshClockCLI(clock, NOW_TEXT, "on", reply);
  EXPECT_STREQ(reply, "OK - mesh clock sync on");
  EXPECT_TRUE(clock.isEnabled());

  meshClockCLI(clock, NOW_TEXT, "", reply);
  EXPECT_STREQ(reply, "on samples=0/33 last=idle slots=33/22 tol=60/60 now=2026-11-01T14:05:00Z");

  meshClockCLI(clock, NOW_TEXT, "bogus", reply);
  EXPECT_STREQ(reply, "Err - clock mesh takes on|off|slots|tolerance");

  meshClockCLI(clock, NOW_TEXT, "on extra", reply);
  EXPECT_STREQ(reply, "Err - clock mesh takes on|off|slots|tolerance");
  EXPECT_TRUE(clock.isEnabled());   // a refused command changed nothing

  meshClockCLI(clock, NOW_TEXT, "off", reply);
  EXPECT_STREQ(reply, "OK - mesh clock sync off");
  EXPECT_FALSE(clock.isEnabled());
}

TEST_F(ClockTest, CliStatusShowsLastResult) {
  clock.setEnabled(true);
  for (int i = 0; i < MESHCLOCK_SLOTS; i++) feed(clock, i, TRUE_NOW, WRONG_NOW);
  char reply[CLI_REPLY_MAX];
  meshClockCLI(clock, NOW_TEXT, "", reply);
  EXPECT_STREQ(reply, "on samples=0/33 last=set(+100000000s) slots=33/22 tol=60/60 now=2026-11-01T14:05:00Z");
}

// ---------------------------------------------------- runtime tuning knobs

TEST_F(ClockTest, DefaultsAreCompileTimeValues) {
  EXPECT_EQ(clock.getSlots(), MESHCLOCK_SLOTS);
  EXPECT_EQ(clock.getMinAgree(), MESHCLOCK_MIN_AGREE);
  EXPECT_EQ(clock.getToleranceSecs(), MESHCLOCK_TOLERANCE_SECS);
  EXPECT_EQ(clock.getMinDiffSecs(), MESHCLOCK_MIN_DIFF_SECS);
}

TEST_F(ClockTest, SlotsRoundTrips) {
  clock.setSlots(15, 10);
  EXPECT_EQ(clock.getSlots(), 15);
  EXPECT_EQ(clock.getMinAgree(), 10);
  flushSave();

  MeshClockSync loaded;
  loaded.begin(&fs);
  EXPECT_EQ(loaded.getSlots(), 15);
  EXPECT_EQ(loaded.getMinAgree(), 10);
  // a knob that was never touched survives with its compile-time default
  EXPECT_EQ(loaded.getToleranceSecs(), MESHCLOCK_TOLERANCE_SECS);
}

TEST_F(ClockTest, ToleranceRoundTrips) {
  clock.setTolerance(300, 200);
  EXPECT_EQ(clock.getToleranceSecs(), 300);
  EXPECT_EQ(clock.getMinDiffSecs(), 200);
  flushSave();

  MeshClockSync loaded;
  loaded.begin(&fs);
  EXPECT_EQ(loaded.getToleranceSecs(), 300);
  EXPECT_EQ(loaded.getMinDiffSecs(), 200);
  EXPECT_EQ(loaded.getSlots(), MESHCLOCK_SLOTS);
}

TEST_F(ClockTest, SlotsRejectsBadValues) {
  EXPECT_FALSE(clock.setSlots(MESHCLOCK_SLOTS_MIN - 1, 2));   // below the floor
  EXPECT_FALSE(clock.setSlots(MESHCLOCK_SLOTS + 1, 2));       // above the array
  EXPECT_FALSE(clock.setSlots(15, 1));                        // quorum too small
  EXPECT_FALSE(clock.setSlots(15, 16));                       // quorum exceeds slots
  EXPECT_EQ(clock.getSlots(), MESHCLOCK_SLOTS);               // unchanged
  EXPECT_EQ(clock.getMinAgree(), MESHCLOCK_MIN_AGREE);
  // a same-value "set" is a no-op, not an error, and writes nothing
  flushSave();
  EXPECT_FALSE(fs.exists(MESHCLOCK_CFG_FILE));
}

TEST_F(ClockTest, ToleranceRejectsBadValues) {
  EXPECT_FALSE(clock.setTolerance(0, 60));   // zero tolerance deadlocks consensus
  EXPECT_TRUE(clock.setTolerance(1, 0));     // a 0 deadband is meaningful: always step
  EXPECT_EQ(clock.getToleranceSecs(), 1);
  EXPECT_EQ(clock.getMinDiffSecs(), 0);
}

TEST_F(ClockTest, SlotsChangeDropsInflightBatch) {
  clock.setEnabled(true);
  feed(clock, 0, TRUE_NOW, WRONG_NOW);
  feed(clock, 1, TRUE_NOW, WRONG_NOW);
  ASSERT_EQ(clock.getSampleCount(), 2);
  clock.setSlots(15, 10);
  EXPECT_EQ(clock.getSampleCount(), 0);
}

TEST_F(ClockTest, ToleranceChangeDropsInflightBatch) {
  clock.setEnabled(true);
  feed(clock, 0, TRUE_NOW, WRONG_NOW);
  ASSERT_EQ(clock.getSampleCount(), 1);
  clock.setTolerance(300, 300);
  EXPECT_EQ(clock.getSampleCount(), 0);
}

TEST_F(ClockTest, LoweredCapFillsAndDecides) {
  clock.setEnabled(true);
  clock.setSlots(3, 2);
  feed(clock, 0, TRUE_NOW, WRONG_NOW);
  feed(clock, 1, TRUE_NOW, WRONG_NOW);
  EXPECT_EQ(set_calls, 0u);   // decides only when the lowered cap fills

  feed(clock, 2, TRUE_NOW, WRONG_NOW);
  EXPECT_EQ(set_calls, 1u);
  EXPECT_EQ(set_epoch, TRUE_NOW);
  EXPECT_EQ(clock.getLastResult(), MeshClockSync::SET_CLOCK);
}

TEST_F(ClockTest, ToleranceGatesInlierCount) {
  clock.setEnabled(true);
  clock.setSlots(5, 4);
  clock.setTolerance(5, 0);
  for (int i = 0; i < 2; i++) feed(clock, i, TRUE_NOW, WRONG_NOW);
  for (int i = 2; i < 5; i++) feed(clock, i, TRUE_NOW + 100, WRONG_NOW);
  // tight window: only the three T+100 sources count as inliers on the median T+100
  EXPECT_EQ(set_calls, 0u);
  EXPECT_EQ(clock.getLastResult(), MeshClockSync::NO_CONSENSUS);
  EXPECT_EQ(clock.getSampleCount(), 0);

  // loose window: all five count as inliers and the quorum is met
  clock.setTolerance(200, 0);   // also drops the batch; re-feed
  for (int i = 0; i < 2; i++) feed(clock, i, TRUE_NOW, WRONG_NOW);
  for (int i = 2; i < 5; i++) feed(clock, i, TRUE_NOW + 100, WRONG_NOW);
  EXPECT_EQ(set_calls, 1u);
  EXPECT_EQ(set_epoch, TRUE_NOW + 100);   // the median of the five
}

TEST_F(ClockTest, DeadbandZeroAlwaysSteps) {
  clock.setEnabled(true);
  clock.setTolerance(60, 0);   // deadband 0: any difference is out of sync
  for (int i = 0; i < MESHCLOCK_SLOTS; i++) feed(clock, i, TRUE_NOW + 5, TRUE_NOW);
  EXPECT_EQ(set_calls, 1u);
  EXPECT_EQ(set_epoch, TRUE_NOW + 5);
}

// ---------------------------------------------------- v1 -> v2 migration

TEST_F(ClockTest, V1ConfigMigrates) {
  writeV1File(fs, /*enabled=*/1, MESHCLOCK_CFG_FILE);
  clock.begin(&fs);
  EXPECT_TRUE(clock.isEnabled());
  EXPECT_EQ(clock.getSlots(), MESHCLOCK_SLOTS);   // knobs take the defaults
  EXPECT_EQ(clock.getMinAgree(), MESHCLOCK_MIN_AGREE);
  EXPECT_EQ(clock.getToleranceSecs(), MESHCLOCK_TOLERANCE_SECS);
  EXPECT_EQ(clock.getMinDiffSecs(), MESHCLOCK_MIN_DIFF_SECS);

  flushSave();   // the migration save writes v2
  ASSERT_EQ(fs.files[MESHCLOCK_CFG_FILE].size(), 10u);
  EXPECT_EQ(fs.files[MESHCLOCK_CFG_FILE][0], MESHCLOCK_CFG_VERSION);

  MeshClockSync loaded;
  loaded.begin(&fs);
  EXPECT_TRUE(loaded.isEnabled());   // the migrated record loads as v2
}

TEST_F(ClockTest, V1DisabledFlagCarries) {
  writeV1File(fs, /*enabled=*/0, MESHCLOCK_CFG_FILE);
  clock.begin(&fs);
  EXPECT_FALSE(clock.isEnabled());
  flushSave();
  ASSERT_EQ(fs.files[MESHCLOCK_CFG_FILE].size(), 10u);
  EXPECT_EQ(fs.files[MESHCLOCK_CFG_FILE][1], 0);
}

TEST_F(ClockTest, CorruptV1CanonicalFallsBackToV1Backup) {
  // the v1 path on its own: canonical v1 fails the CRC check, the backup is a
  // good v1 — same recovery the v2 path gets, through the same machinery
  std::vector<uint8_t> bad = { 1, 1, 0, 0 };
  uint16_t crc = crc16_ccitt(bad.data(), 2);
  memcpy(&bad[2], &crc, 2);
  bad[3] ^= 0xFF;   // break the CRC
  fs.files[MESHCLOCK_CFG_FILE] = bad;
  writeV1File(fs, /*enabled=*/1, MESHCLOCK_CFG_FILE ".bak");

  MeshClockSync loaded;
  loaded.begin(&fs);
  EXPECT_TRUE(loaded.isEnabled());   // recovered from the v1 backup
}

TEST_F(ClockTest, CorruptV2FallsBackToV1Backup) {
  writeV1File(fs, /*enabled=*/1, MESHCLOCK_CFG_FILE);
  clock.begin(&fs);   // loads v1, marks dirty for the migration save
  flushSave();        // v2 canonical promoted; the v1 bytes move to the backup
  ASSERT_EQ(fs.files[MESHCLOCK_CFG_FILE].size(), 10u);

  // corrupt the canonical with a CRC-valid but impossible knob pair
  std::vector<uint8_t>& rec = fs.files[MESHCLOCK_CFG_FILE];
  rec[2] = 5;   // slots
  rec[3] = 9;   // min_agree > slots
  uint16_t crc = crc16_ccitt(rec.data(), 8);
  memcpy(&rec[8], &crc, 2);

  MeshClockSync loaded;
  loaded.begin(&fs);
  EXPECT_TRUE(loaded.isEnabled());   // recovered from the v1 backup
}

TEST_F(ClockTest, InvalidV2RecordRejected) {
  clock.setEnabled(true);
  clock.setSlots(5, 3);
  flushSave();   // no backup existed before this first save
  ASSERT_EQ(fs.files[MESHCLOCK_CFG_FILE].size(), 10u);

  std::vector<uint8_t>& rec = fs.files[MESHCLOCK_CFG_FILE];
  rec[3] = 9;   // min_agree 9 > slots 5, with a matching CRC
  uint16_t crc = crc16_ccitt(rec.data(), 8);
  memcpy(&rec[8], &crc, 2);

  MeshClockSync loaded;
  loaded.begin(&fs);
  EXPECT_FALSE(loaded.isEnabled());   // nothing usable: defaults
  EXPECT_EQ(loaded.getSlots(), MESHCLOCK_SLOTS);
}

// --------------------------------------------------------------- CLI knobs

TEST_F(ClockTest, CliSlotsSetsAndPersists) {
  char reply[CLI_REPLY_MAX];
  meshClockCLI(clock, NOW_TEXT, "slots 15 10", reply);
  EXPECT_STREQ(reply, "OK - mesh clock slots 15/10");
  EXPECT_EQ(clock.getSlots(), 15);
  EXPECT_EQ(clock.getMinAgree(), 10);

  flushSave();
  MeshClockSync loaded;
  loaded.begin(&fs);
  EXPECT_EQ(loaded.getSlots(), 15);
  EXPECT_EQ(loaded.getMinAgree(), 10);

  // the status line shows the configured pair
  meshClockCLI(loaded, NOW_TEXT, "on", reply);
  meshClockCLI(loaded, NOW_TEXT, "", reply);
  EXPECT_STREQ(reply, "on samples=0/15 last=idle slots=15/10 tol=60/60 now=2026-11-01T14:05:00Z");
}

TEST_F(ClockTest, CliSlotsValidation) {
  char reply[CLI_REPLY_MAX];
  meshClockCLI(clock, NOW_TEXT, "slots", reply);
  EXPECT_STREQ(reply, "Err - clock mesh takes on|off|slots|tolerance");
  meshClockCLI(clock, NOW_TEXT, "slots 15", reply);
  EXPECT_STREQ(reply, "Err - clock mesh takes on|off|slots|tolerance");
  meshClockCLI(clock, NOW_TEXT, "slots 15 10 3", reply);
  EXPECT_STREQ(reply, "Err - clock mesh takes on|off|slots|tolerance");
  meshClockCLI(clock, NOW_TEXT, "slots 2 2", reply);
  EXPECT_STREQ(reply, "Err - slots must be 3..33");
  meshClockCLI(clock, NOW_TEXT, "slots 34 22", reply);
  EXPECT_STREQ(reply, "Err - slots must be 3..33");
  meshClockCLI(clock, NOW_TEXT, "slots 15 16", reply);
  EXPECT_STREQ(reply, "Err - min-agree must be 2..15");
  meshClockCLI(clock, NOW_TEXT, "slots 15 1", reply);
  EXPECT_STREQ(reply, "Err - min-agree must be 2..15");
  meshClockCLI(clock, NOW_TEXT, "slots banana 10", reply);
  EXPECT_STREQ(reply, "Err - clock mesh takes on|off|slots|tolerance");
  meshClockCLI(clock, NOW_TEXT, "slots 15 banana", reply);
  EXPECT_STREQ(reply, "Err - clock mesh takes on|off|slots|tolerance");
  EXPECT_EQ(clock.getSlots(), MESHCLOCK_SLOTS);   // nothing was applied
}

TEST_F(ClockTest, CliToleranceSetsAndPersists) {
  char reply[CLI_REPLY_MAX];
  meshClockCLI(clock, NOW_TEXT, "tolerance 300 0", reply);
  EXPECT_STREQ(reply, "OK - mesh clock tolerance 300/0");
  EXPECT_EQ(clock.getToleranceSecs(), 300);
  EXPECT_EQ(clock.getMinDiffSecs(), 0);

  flushSave();
  MeshClockSync loaded;
  loaded.begin(&fs);
  EXPECT_EQ(loaded.getToleranceSecs(), 300);
  EXPECT_EQ(loaded.getMinDiffSecs(), 0);
}

TEST_F(ClockTest, CliToleranceValidation) {
  char reply[CLI_REPLY_MAX];
  meshClockCLI(clock, NOW_TEXT, "tolerance", reply);
  EXPECT_STREQ(reply, "Err - clock mesh takes on|off|slots|tolerance");
  meshClockCLI(clock, NOW_TEXT, "tolerance 300", reply);
  EXPECT_STREQ(reply, "Err - clock mesh takes on|off|slots|tolerance");
  meshClockCLI(clock, NOW_TEXT, "tolerance 0 60", reply);
  EXPECT_STREQ(reply, "Err - tolerance must be 1..65535 seconds");
  meshClockCLI(clock, NOW_TEXT, "tolerance 65536 60", reply);
  EXPECT_STREQ(reply, "Err - tolerance must be 1..65535 seconds");
  meshClockCLI(clock, NOW_TEXT, "tolerance 60 65536", reply);
  EXPECT_STREQ(reply, "Err - min-diff must be 0..65535 seconds");
  meshClockCLI(clock, NOW_TEXT, "tolerance 300 300 1", reply);
  EXPECT_STREQ(reply, "Err - clock mesh takes on|off|slots|tolerance");
  meshClockCLI(clock, NOW_TEXT, "tolerance banana 300", reply);
  EXPECT_STREQ(reply, "Err - clock mesh takes on|off|slots|tolerance");
  EXPECT_EQ(clock.getToleranceSecs(), MESHCLOCK_TOLERANCE_SECS);   // untouched
}

TEST_F(ClockTest, CliStatusShowsKnobsWhenOff) {
  char reply[CLI_REPLY_MAX];
  meshClockCLI(clock, NOW_TEXT, "slots 15 10", reply);
  meshClockCLI(clock, NOW_TEXT, "", reply);
  EXPECT_STREQ(reply, "off slots=15/10 tol=60/60 now=2026-11-01T14:05:00Z");
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
