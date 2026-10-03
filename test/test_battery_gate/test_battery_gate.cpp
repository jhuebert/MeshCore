// test_battery_gate.cpp — native unit tests for the simple_repeater battery
// gate (examples/simple_repeater/BatteryGate*). Built by
// [env:native_battery_gate] in platformio.ini; see that env for the shim and
// stub notes.
//
// Sections: hysteresis, debounce, sampling cadence, config/CLI parsing,
// persistence, and the drop counter.

#include <gtest/gtest.h>

#include <string>

#include <Arduino.h>      // g_mock_millis (NativeShim.h is force-included first)
#include "BatteryGate.h"

// mirrors BATT_SAVE_DELAY_MS in BatteryGate.cpp
static constexpr unsigned long CFG_SAVE_DELAY_MS = 3000;
// mirrors BATT_SAMPLE_INTERVAL_MS in BatteryGate.cpp
static constexpr unsigned long SAMPLE_INTERVAL_MS = 30000;

#define BATT_CFG_FILE "/batt_cfg"
// mirrors BATT_CFG_RECORD_BYTES / BATT_CFG_VERSION in BatteryGate.cpp
static constexpr size_t BATT_CFG_RECORD_BYTES = 6;
static constexpr uint8_t BATT_CFG_VERSION = 1;

// Board stand-in whose voltage the tests inject.
struct MockBoard : public mesh::MainBoard {
  uint16_t mV = 4200;
  uint16_t getBattMilliVolts() override { return mV; }
  const char* getManufacturerName() const override { return "mock-board"; }
  void reboot() override {}
  uint8_t getStartupReason() const override { return 0; }
};

// Run one CLI command (with the "battery" prefix already stripped) and return
// the reply.
static std::string cli(BatteryGate& gate, MockBoard& board, const char* command) {
  char reply[160];
  reply[0] = 0;
  batteryCLI(gate, board, command, reply);
  return std::string(reply);
}

// Shared fixture: fresh gate, no persisted config.
class BatteryGateTest : public ::testing::Test {
protected:
  NativeFS fs;
  MockBoard board;
  BatteryGate gate;

  void SetUp() override {
    g_mock_millis = 0;
    gate.begin(&fs);
  }
};

// ============================================================
// HYSTERESIS
// ============================================================

TEST_F(BatteryGateTest, SuspendsBelowSuspendThreshold) {
  gate.setThresholds(3400, 3600);
  board.mV = 3300;
  gate.loop(&fs, board);   // immediate first sample
  gate.loop(&fs, board);   // ...needs a second sample to pass debounce
  g_mock_millis += SAMPLE_INTERVAL_MS;
  gate.loop(&fs, board);
  EXPECT_TRUE(gate.isSuspended());
}

TEST_F(BatteryGateTest, StaysSuspendedBetweenThresholds) {
  gate.setThresholds(3400, 3600);
  board.mV = 3300;
  gate.loop(&fs, board);
  g_mock_millis += SAMPLE_INTERVAL_MS;
  gate.loop(&fs, board);
  EXPECT_TRUE(gate.isSuspended());

  board.mV = 3500;   // above suspend, below resume
  g_mock_millis += SAMPLE_INTERVAL_MS;
  gate.loop(&fs, board);
  EXPECT_TRUE(gate.isSuspended());
}

TEST_F(BatteryGateTest, ResumesAtResumeThresholdInclusive) {
  gate.setThresholds(3400, 3600);
  board.mV = 3300;
  gate.loop(&fs, board);
  g_mock_millis += SAMPLE_INTERVAL_MS;
  gate.loop(&fs, board);
  EXPECT_TRUE(gate.isSuspended());

  board.mV = 3599;   // still below resume
  g_mock_millis += SAMPLE_INTERVAL_MS;
  gate.loop(&fs, board);
  EXPECT_TRUE(gate.isSuspended());

  board.mV = 3600;   // boundary: resume is inclusive
  g_mock_millis += SAMPLE_INTERVAL_MS;
  gate.loop(&fs, board);
  EXPECT_FALSE(gate.isSuspended());
}

TEST_F(BatteryGateTest, SuspendThresholdBoundaryInclusive) {
  gate.setThresholds(3400, 3600);
  board.mV = 3400;   // "below" is strict: exactly at the threshold is OK
  gate.loop(&fs, board);
  g_mock_millis += SAMPLE_INTERVAL_MS;
  gate.loop(&fs, board);
  EXPECT_FALSE(gate.isSuspended());
}

TEST_F(BatteryGateTest, DefaultResumeIsSuspendPlus200mV) {
  std::string reply = cli(gate, board, "3400");
  EXPECT_EQ(gate.getSuspendMilliVolts(), 3400);
  EXPECT_EQ(gate.getResumeMilliVolts(), 3600);
  EXPECT_NE(reply.find("3600"), std::string::npos);
}

// ============================================================
// DEBOUNCE
// ============================================================

TEST_F(BatteryGateTest, OneLowReadingDoesNotSuspend) {
  gate.setThresholds(3400, 3600);
  board.mV = 3300;
  gate.loop(&fs, board);   // first sample: low
  EXPECT_FALSE(gate.isSuspended());
}

TEST_F(BatteryGateTest, TwoConsecutiveLowReadingsSuspend) {
  gate.setThresholds(3400, 3600);
  board.mV = 3300;
  gate.loop(&fs, board);
  g_mock_millis += SAMPLE_INTERVAL_MS;
  gate.loop(&fs, board);
  EXPECT_TRUE(gate.isSuspended());
}

TEST_F(BatteryGateTest, InterveningGoodReadingResetsDebounce) {
  gate.setThresholds(3400, 3600);
  board.mV = 3300;
  gate.loop(&fs, board);   // low #1
  board.mV = 4000;
  g_mock_millis += SAMPLE_INTERVAL_MS;
  gate.loop(&fs, board);   // good: counter resets
  board.mV = 3300;
  g_mock_millis += SAMPLE_INTERVAL_MS;
  gate.loop(&fs, board);   // low #1 again
  EXPECT_FALSE(gate.isSuspended());
}

// ============================================================
// SAMPLING CADENCE
// ============================================================

TEST_F(BatteryGateTest, FirstSampleIsImmediate) {
  gate.setThresholds(3400, 3600);
  board.mV = 3300;
  gate.loop(&fs, board);   // first loop() after enabling samples immediately
  EXPECT_FALSE(gate.isSuspended());   // but one low reading alone doesn't suspend
}

// Between intervals the gate must not re-sample: set an absurdly low voltage
// that is never observed at t=29 999 ms, then observed at exactly 30 s.
TEST_F(BatteryGateTest, NoResampleBeforeInterval) {
  gate.setThresholds(3400, 3600);
  board.mV = 3300;
  gate.loop(&fs, board);                       // t=0: low #1
  board.mV = 100;
  g_mock_millis += SAMPLE_INTERVAL_MS - 1;
  gate.loop(&fs, board);                       // t=29999: interval not elapsed
  EXPECT_FALSE(gate.isSuspended());            // (a sample here would be low #2)

  g_mock_millis += 1;
  gate.loop(&fs, board);                       // t=30000: samples 100 mV -> low #2
  EXPECT_TRUE(gate.isSuspended());
}

// isSuspended() reports the cached flag: no ADC read happens outside loop()'s
// sampling cadence. Verified by flipping the board voltage without advancing
// time — the gate must not notice until the next sample.
TEST_F(BatteryGateTest, NoResampleUsesCachedFlag) {
  gate.setThresholds(3400, 3600);
  board.mV = 3300;
  gate.loop(&fs, board);   // low #1
  board.mV = 100;          // wildly low, but not sampled yet
  EXPECT_FALSE(gate.checkForward() == false);   // still forwarding: flag is cached, not live

  g_mock_millis += SAMPLE_INTERVAL_MS;
  gate.loop(&fs, board);   // now it samples 100 mV (low #2)
  EXPECT_TRUE(gate.isSuspended());
  EXPECT_FALSE(gate.checkForward());
}

TEST_F(BatteryGateTest, DisabledGateNeverSamplesOrSuspends) {
  board.mV = 100;
  for (int i = 0; i < 5; i++) {
    gate.loop(&fs, board);
    g_mock_millis += SAMPLE_INTERVAL_MS;
  }
  EXPECT_FALSE(gate.isEnabled());
  EXPECT_FALSE(gate.isSuspended());
  EXPECT_TRUE(gate.checkForward());
}

// ============================================================
// CONFIG / CLI
// ============================================================

TEST_F(BatteryGateTest, BareCommandShowsStatus) {
  gate.setThresholds(3400, 3600);
  board.mV = 3950;
  std::string reply = cli(gate, board, "");
  EXPECT_NE(reply.find("3950"), std::string::npos);
  EXPECT_NE(reply.find("gate on"), std::string::npos);
  EXPECT_NE(reply.find("forwarding"), std::string::npos);
  EXPECT_NE(reply.find("suspend <3400"), std::string::npos);
  EXPECT_NE(reply.find("resume >=3600"), std::string::npos);
  EXPECT_LE(reply.size(), 160);   // CLI reply buffer
}

TEST_F(BatteryGateTest, StatusShowsSuspendedAndDrops) {
  gate.setThresholds(3400, 3600);
  board.mV = 3300;
  gate.loop(&fs, board);
  g_mock_millis += SAMPLE_INTERVAL_MS;
  gate.loop(&fs, board);
  gate.checkForward();

  std::string reply = cli(gate, board, "");
  EXPECT_NE(reply.find("SUSPENDED"), std::string::npos);
  EXPECT_NE(reply.find("drops 1"), std::string::npos);
  EXPECT_LE(reply.size(), 160);
}

TEST_F(BatteryGateTest, OffDisablesAndLiftsSuspension) {
  gate.setThresholds(3400, 3600);
  board.mV = 3300;
  gate.loop(&fs, board);
  g_mock_millis += SAMPLE_INTERVAL_MS;
  gate.loop(&fs, board);
  EXPECT_TRUE(gate.isSuspended());

  std::string reply = cli(gate, board, "off");
  EXPECT_EQ(reply, "OK - battery gate off");
  EXPECT_FALSE(gate.isEnabled());
  EXPECT_FALSE(gate.isSuspended());
  EXPECT_TRUE(gate.checkForward());
}

TEST_F(BatteryGateTest, SetSuspendAndResume) {
  std::string reply = cli(gate, board, "3400 3700");
  EXPECT_EQ(reply.substr(0, 3), "OK ");
  EXPECT_TRUE(gate.isEnabled());
  EXPECT_EQ(gate.getSuspendMilliVolts(), 3400);
  EXPECT_EQ(gate.getResumeMilliVolts(), 3700);
}

TEST_F(BatteryGateTest, RejectsNonNumeric) {
  EXPECT_EQ(cli(gate, board, "abc"), "Err - usage: battery | battery off | battery <suspend-mV> [resume-mV]");
  EXPECT_EQ(cli(gate, board, "3400 low"), "Err - usage: battery | battery off | battery <suspend-mV> [resume-mV]");
  EXPECT_FALSE(gate.isEnabled());
}

TEST_F(BatteryGateTest, RejectsResumeNotGreaterThanSuspend) {
  EXPECT_EQ(cli(gate, board, "3400 3400"), "Err - resume must be greater than suspend");
  EXPECT_EQ(cli(gate, board, "3400 3300"), "Err - resume must be greater than suspend");
  EXPECT_FALSE(gate.isEnabled());
}

// Without an explicit resume the CLI derives suspend + 200 mV. Near the top of
// the range that derived value leaves the accepted band: it must be refused, not
// clamped, and a previously configured gate must survive the refusal.
TEST_F(BatteryGateTest, ImplicitResumeRefusesInsteadOfClamping) {
  EXPECT_EQ(cli(gate, board, "9800"), "OK - gate on; suspend <9800mV; resume >=10000mV");
  EXPECT_EQ(gate.getSuspendMilliVolts(), 9800);
  EXPECT_EQ(gate.getResumeMilliVolts(), 10000);

  for (const char* cmd : { "9801", "9900", "10000" }) {
    std::string reply = cli(gate, board, cmd);
    EXPECT_EQ(reply, "Err - default resume exceeds 10000mV; specify resume") << cmd;
  }
  // the refused commands changed nothing
  EXPECT_EQ(gate.getSuspendMilliVolts(), 9800);
  EXPECT_EQ(gate.getResumeMilliVolts(), 10000);
  EXPECT_TRUE(gate.isEnabled());

  // ... and an explicit resume in range is still accepted
  EXPECT_EQ(cli(gate, board, "9900 10000").substr(0, 3), "OK ");
  EXPECT_EQ(gate.getSuspendMilliVolts(), 9900);
}

TEST_F(BatteryGateTest, InvalidThresholdPairIsRejectedByTheApiToo) {
  EXPECT_FALSE(gate.setThresholds(0, 100));
  EXPECT_FALSE(gate.setThresholds(3400, 3400));
  EXPECT_FALSE(gate.setThresholds(3400, 3300));
  EXPECT_FALSE(gate.setThresholds(3400, 10001));
  EXPECT_FALSE(gate.setThresholds(10001, 10002));
  EXPECT_TRUE(gate.setThresholds(3400, 3700));
  EXPECT_EQ(gate.getSuspendMilliVolts(), 3400);
}

TEST_F(BatteryGateTest, RejectsOutOfRange) {
  EXPECT_EQ(cli(gate, board, "0"), "Err - millivolts must be 1..10000");
  EXPECT_EQ(cli(gate, board, "10001"), "Err - millivolts must be 1..10000");
  EXPECT_EQ(cli(gate, board, "3400 10001"), "Err - millivolts must be 1..10000");
  EXPECT_FALSE(gate.isEnabled());
}

// The schedule must not depend on an absolute deadline that a 32-bit millis()
// wrap could collide with, and must take exactly one sample per loop.
TEST_F(BatteryGateTest, FirstSampleHappensOnceNotOncePerLoop) {
  board.mV = 100;   // very low: first sample is low #1
  gate.setThresholds(3400, 3600);
  gate.loop(&fs, board);
  EXPECT_FALSE(gate.isSuspended());   // one reading is not enough

  // looping again in the same instant must NOT immediately take low #2: the
  // gate waits out the interval rather than sampling twice to catch up
  gate.loop(&fs, board);
  EXPECT_FALSE(gate.isSuspended());
  EXPECT_EQ(gate.getDropCount(), 0u);
}

TEST_F(BatteryGateTest, SamplingSurvivesAMillisWrap) {
  gate.setThresholds(3400, 3600);
  g_mock_millis = 0xFFFFF000u;   // ~4 s before UINT32_MAX
  board.mV = 100;
  gate.loop(&fs, board);         // immediate sample: low #1

  g_mock_millis = 0xFFFFFFFFu;
  board.mV = 4000;
  gate.loop(&fs, board);         // no interval elapsed: no sample, so low #1 stands

  g_mock_millis += 1;            // wrapped to 0
  gate.loop(&fs, board);
  EXPECT_FALSE(gate.isSuspended());

  // 30 s measured across the wrap: the good reading lands as low #2, which
  // resets the debounce rather than suspending
  g_mock_millis += SAMPLE_INTERVAL_MS;
  board.mV = 4000;
  gate.loop(&fs, board);
  EXPECT_FALSE(gate.isSuspended());

  // ... and two lows in a row across the wrap still suspend
  g_mock_millis += SAMPLE_INTERVAL_MS;
  board.mV = 100;
  gate.loop(&fs, board);
  g_mock_millis += SAMPLE_INTERVAL_MS;
  gate.loop(&fs, board);
  EXPECT_TRUE(gate.isSuspended());
}

TEST_F(BatteryGateTest, OffDisarmsSamplingAndEnableRearmsIt) {
  gate.setThresholds(3400, 3600);
  EXPECT_TRUE(gate.isEnabled());

  gate.setEnabled(false);
  board.mV = 100;
  for (int i = 0; i < 5; i++) { g_mock_millis += SAMPLE_INTERVAL_MS; gate.loop(&fs, board); }
  EXPECT_FALSE(gate.isSuspended());   // disarmed: no sampling while off

  // re-enabling re-arms and evaluates promptly, without waiting out an interval
  gate.setEnabled(true);
  gate.loop(&fs, board);              // low #1
  EXPECT_FALSE(gate.isSuspended());
  g_mock_millis += SAMPLE_INTERVAL_MS;
  gate.loop(&fs, board);              // low #2
  EXPECT_TRUE(gate.isSuspended());
}

TEST_F(BatteryGateTest, ReArmingThresholdsClearsSuspensionAndDebounce) {
  board.mV = 100;
  gate.setThresholds(3400, 3600);
  gate.loop(&fs, board);
  g_mock_millis += SAMPLE_INTERVAL_MS;
  gate.loop(&fs, board);
  ASSERT_TRUE(gate.isSuspended());

  // the battery recovers and the thresholds are re-armed around a healthy
  // voltage: the gate must be re-evaluated on the next sample rather than left
  // suspended until the next interval
  board.mV = 4000;
  EXPECT_EQ(cli(gate, board, "1000 2000").substr(0, 3), "OK ");
  EXPECT_TRUE(gate.isSuspended());   // not cleared by the command alone...
  gate.loop(&fs, board);
  EXPECT_FALSE(gate.isSuspended());  // ...the next sample confirms recovery

  // ... and a low reading now trips again, needing two consecutive readings as
  // before, with the old debounce state cleared by re-arming
  board.mV = 100;
  g_mock_millis += SAMPLE_INTERVAL_MS;
  gate.loop(&fs, board);         // low #1
  EXPECT_FALSE(gate.isSuspended());
  g_mock_millis += SAMPLE_INTERVAL_MS;
  gate.loop(&fs, board);         // low #2
  EXPECT_TRUE(gate.isSuspended());
}

TEST_F(BatteryGateTest, FailedSaveRetriesSlowlyNotEveryLoop) {
  gate.setThresholds(3400, 3700);

  NativeFS broken;
  broken.fail_open = true;   // every open for writing fails
  g_mock_millis = CFG_SAVE_DELAY_MS;
  for (int i = 0; i < 100; i++) gate.loop(&broken, board);

  // 100 loops in the same instant must NOT mean 100 open attempts
  EXPECT_LE(broken.write_open_attempts, 1u);
  EXPECT_FALSE(broken.exists(BATT_CFG_FILE));

  // the edit is still pending, so it lands once the filesystem recovers
  g_mock_millis += CFG_SAVE_DELAY_MS;
  gate.loop(&fs, board);
  EXPECT_TRUE(fs.exists(BATT_CFG_FILE));
}

TEST_F(BatteryGateTest, ReloadCancelsPendingEdits) {
  gate.setThresholds(3400, 3700);
  gate.markDirty();
  g_mock_millis = CFG_SAVE_DELAY_MS;

  gate.load(&fs);   // nothing on disk: defaults, and no resurrected pre-load edit
  gate.loop(&fs, board);
  EXPECT_FALSE(fs.exists(BATT_CFG_FILE));
}

// ============================================================
// STAGED SAVE / CRASH RECOVERY
// ============================================================

TEST_F(BatteryGateTest, SaveLeavesNoScratchOrBackupOnAFreshDevice) {
  gate.setThresholds(3400, 3700);
  g_mock_millis = CFG_SAVE_DELAY_MS;
  gate.loop(&fs, board);

  ASSERT_TRUE(fs.exists(BATT_CFG_FILE));
  EXPECT_FALSE(fs.exists("/batt_cfg.tmp"));
  EXPECT_FALSE(fs.exists("/batt_cfg.bak"));
}

TEST_F(BatteryGateTest, SecondSaveKeepsThePreviousRecordAsBackup) {
  gate.setThresholds(3400, 3700);
  gate.markDirty();
  g_mock_millis = CFG_SAVE_DELAY_MS;
  gate.loop(&fs, board);

  EXPECT_EQ(cli(gate, board, "3500 3800").substr(0, 3), "OK ");
  gate.markDirty();
  g_mock_millis += CFG_SAVE_DELAY_MS;
  gate.loop(&fs, board);

  ASSERT_TRUE(fs.exists("/batt_cfg.bak"));
  EXPECT_FALSE(fs.exists("/batt_cfg.tmp"));

  // canonical holds the new thresholds; the backup holds the old ones
  BatteryGate now;
  now.begin(&fs);
  EXPECT_EQ(now.getSuspendMilliVolts(), 3500);

  fs.files[BATT_CFG_FILE] = fs.files["/batt_cfg.bak"];
  BatteryGate from_bak;
  from_bak.begin(&fs);
  EXPECT_EQ(from_bak.getSuspendMilliVolts(), 3400);
}

TEST_F(BatteryGateTest, RecoveryFromBackupSchedulesARepairSave) {
  gate.setThresholds(3400, 3700);
  g_mock_millis = CFG_SAVE_DELAY_MS;
  gate.loop(&fs, board);
  EXPECT_EQ(cli(gate, board, "3500 3800").substr(0, 3), "OK ");
  g_mock_millis += CFG_SAVE_DELAY_MS;
  gate.loop(&fs, board);

  fs.remove(BATT_CFG_FILE);   // power loss during promotion

  BatteryGate restored;
  restored.begin(&fs);
  ASSERT_EQ(restored.getSuspendMilliVolts(), 3400);   // the whole backup
  EXPECT_FALSE(restored.isSuspended());                // RAM-only, not carried over

  g_mock_millis += CFG_SAVE_DELAY_MS;
  restored.loop(&fs, board);
  EXPECT_TRUE(fs.exists(BATT_CFG_FILE));

  BatteryGate after;
  after.begin(&fs);
  EXPECT_EQ(after.getSuspendMilliVolts(), 3400);
}

TEST_F(BatteryGateTest, FailedScratchWriteLeavesTheOldRecordIntact) {
  gate.setThresholds(3400, 3700);
  gate.markDirty();
  g_mock_millis = CFG_SAVE_DELAY_MS;
  gate.loop(&fs, board);
  std::vector<uint8_t> original = fs.files[BATT_CFG_FILE];

  EXPECT_EQ(cli(gate, board, "3500 3800").substr(0, 3), "OK ");
  NativeFS broken;
  broken.files[BATT_CFG_FILE] = original;
  broken.fail_write = true;
  gate.save(&broken);

  EXPECT_EQ(broken.files[BATT_CFG_FILE].size(), original.size());
  EXPECT_EQ(memcmp(broken.files[BATT_CFG_FILE].data(), original.data(), original.size()), 0);
  EXPECT_FALSE(broken.exists("/batt_cfg.tmp"));
  EXPECT_FALSE(broken.exists("/batt_cfg.bak"));
}

TEST_F(BatteryGateTest, OrphanScratchIsIgnoredOnBoot) {
  gate.setThresholds(3400, 3700);
  g_mock_millis = CFG_SAVE_DELAY_MS;
  gate.loop(&fs, board);

  fs.files["/batt_cfg.tmp"] = fs.files[BATT_CFG_FILE];
  fs.files["/batt_cfg.tmp"].push_back(0xFF);

  BatteryGate restored;
  restored.begin(&fs);
  EXPECT_EQ(restored.getSuspendMilliVolts(), 3400);
}

// ============================================================
// PERSISTENCE
// ============================================================

TEST_F(BatteryGateTest, RoundtripThroughConfigFile) {
  gate.setThresholds(3400, 3700);
  g_mock_millis = CFG_SAVE_DELAY_MS;
  gate.loop(&fs, board);
  ASSERT_TRUE(fs.exists(BATT_CFG_FILE));

  BatteryGate restored;
  restored.begin(&fs);
  EXPECT_TRUE(restored.isEnabled());
  EXPECT_EQ(restored.getSuspendMilliVolts(), 3400);
  EXPECT_EQ(restored.getResumeMilliVolts(), 3700);
}

TEST_F(BatteryGateTest, MissingFileGivesDefaults) {
  BatteryGate fresh;
  fresh.begin(&fs);
  EXPECT_FALSE(fresh.isEnabled());
  EXPECT_FALSE(fresh.isSuspended());
  EXPECT_EQ(fresh.getSuspendMilliVolts(), 0);
  EXPECT_EQ(fresh.getResumeMilliVolts(), 0);
}

TEST_F(BatteryGateTest, LoadResetsStateTheFileCannotCarry) {
  gate.setThresholds(3400, 3700);
  fs.remove(BATT_CFG_FILE);   // nothing to load

  gate.load(&fs);        // a reload must not inherit anything from before
  EXPECT_FALSE(gate.isEnabled());
  EXPECT_EQ(gate.getSuspendMilliVolts(), 0);
  EXPECT_EQ(gate.getResumeMilliVolts(), 0);
}

// The loader must validate a whole record on locals before assigning anything
// live: a record the CLI could never produce is still not adopted.
TEST_F(BatteryGateTest, LoadValidatesTheWholeRecord) {
  auto writeRec = [&](uint8_t version, uint8_t enabled, uint16_t susp, uint16_t res) {
    uint8_t rec[BATT_CFG_RECORD_BYTES];
    rec[0] = version;
    rec[1] = enabled;
    memcpy(&rec[2], &susp, 2);
    memcpy(&rec[4], &res, 2);
    fs.files[BATT_CFG_FILE].assign(rec, rec + BATT_CFG_RECORD_BYTES);
  };

  struct { uint8_t en; uint16_t s, r; bool keep; const char* why; } cases[] = {
    { 0, 0,     0,     true,  "disabled with the shipped 0/0 default" },
    { 0, 3400, 3700, true,  "disabled but retaining valid thresholds" },
    { 1, 3400, 3700, true,  "enabled with valid thresholds" },
    { 1, 0,     0,     false, "enabled with zero thresholds" },
    { 1, 3400, 3400, false, "resume equal to suspend" },
    { 1, 3700, 3400, false, "resume below suspend" },
    { 1, 12000, 13000, false, "both above the accepted range" },
    { 2, 3400, 3700, false, "enabled byte is neither 0 nor 1" },
  };
  for (auto& c : cases) {
    writeRec(BATT_CFG_VERSION, c.en, c.s, c.r);
    BatteryGate g;
    g.begin(&fs);
    g.load(&fs);
    // a valid record is adopted; a disabled one simply stays disabled
    EXPECT_EQ(g.isEnabled(), c.keep && c.en == 1) << c.why;
    if (c.keep) {
      EXPECT_EQ(g.getSuspendMilliVolts(), c.s) << c.why;
      EXPECT_EQ(g.getResumeMilliVolts(), c.r) << c.why;
    } else {
      EXPECT_EQ(g.getSuspendMilliVolts(), 0) << c.why;
      EXPECT_EQ(g.getResumeMilliVolts(), 0) << c.why;
    }
  }

  // an unsupported version byte restores the defaults
  writeRec(BATT_CFG_VERSION + 1, 1, 3400, 3700);
  BatteryGate g;
  g.begin(&fs);
  g.load(&fs);
  EXPECT_FALSE(g.isEnabled());
  EXPECT_EQ(g.getSuspendMilliVolts(), 0);
}

TEST_F(BatteryGateTest, TruncatedFileGivesDefaults) {
  gate.setThresholds(3400, 3700);
  g_mock_millis = CFG_SAVE_DELAY_MS;
  gate.loop(&fs, board);
  ASSERT_TRUE(fs.exists(BATT_CFG_FILE));

  fs.files[BATT_CFG_FILE].resize(3);   // cut inside the record
  BatteryGate restored;
  restored.begin(&fs);
  EXPECT_FALSE(restored.isEnabled());
  EXPECT_EQ(restored.getSuspendMilliVolts(), 0);
  EXPECT_EQ(restored.getResumeMilliVolts(), 0);
}

TEST_F(BatteryGateTest, CorruptedRecordGivesDefaults) {
  gate.setThresholds(3400, 3700);
  g_mock_millis = CFG_SAVE_DELAY_MS;
  gate.loop(&fs, board);
  ASSERT_TRUE(fs.exists(BATT_CFG_FILE));

  // resume := suspend in the persisted record is rejected on load
  // (little-endian host: 3400 = 0x48 0x0D, the bytes already stored for suspend)
  fs.files[BATT_CFG_FILE][4] = 0x48;
  fs.files[BATT_CFG_FILE][5] = 0x0D;

  BatteryGate restored;
  restored.begin(&fs);
  EXPECT_FALSE(restored.isEnabled());
  EXPECT_EQ(restored.getSuspendMilliVolts(), 0);
  EXPECT_EQ(restored.getResumeMilliVolts(), 0);
}

TEST_F(BatteryGateTest, SuspendedFlagIsNotPersisted) {
  gate.setThresholds(3400, 3600);
  board.mV = 3300;
  gate.loop(&fs, board);
  g_mock_millis += SAMPLE_INTERVAL_MS;
  gate.loop(&fs, board);
  EXPECT_TRUE(gate.isSuspended());

  g_mock_millis += CFG_SAVE_DELAY_MS;
  gate.loop(&fs, board);   // lazy save fires while suspended
  ASSERT_TRUE(fs.exists(BATT_CFG_FILE));

  BatteryGate restored;
  restored.begin(&fs);
  EXPECT_TRUE(restored.isEnabled());    // thresholds persist...
  EXPECT_FALSE(restored.isSuspended()); // ...but the suspension is recomputed
}

TEST_F(BatteryGateTest, LazySaveAfterDelay) {
  gate.setThresholds(3400, 3700);   // marks dirty at t=0
  gate.loop(&fs, board);            // t=0: delay not elapsed
  EXPECT_FALSE(fs.exists(BATT_CFG_FILE));

  g_mock_millis = CFG_SAVE_DELAY_MS;
  gate.loop(&fs, board);
  EXPECT_TRUE(fs.exists(BATT_CFG_FILE));
}

TEST_F(BatteryGateTest, OffPersists) {
  gate.setThresholds(3400, 3700);
  g_mock_millis = CFG_SAVE_DELAY_MS;
  gate.loop(&fs, board);

  cli(gate, board, "off");
  g_mock_millis = CFG_SAVE_DELAY_MS * 2;
  gate.loop(&fs, board);

  BatteryGate restored;
  restored.begin(&fs);
  EXPECT_FALSE(restored.isEnabled());
  EXPECT_EQ(restored.getSuspendMilliVolts(), 3400);   // thresholds kept for reference
}

// ============================================================
// DROP COUNTER
// ============================================================

TEST_F(BatteryGateTest, DropCounterCountsGatedForwards) {
  gate.setThresholds(3400, 3600);
  board.mV = 3300;
  gate.loop(&fs, board);
  g_mock_millis += SAMPLE_INTERVAL_MS;
  gate.loop(&fs, board);
  EXPECT_TRUE(gate.isSuspended());

  EXPECT_FALSE(gate.checkForward());
  EXPECT_FALSE(gate.checkForward());
  EXPECT_FALSE(gate.checkForward());
  EXPECT_EQ(gate.getDropCount(), 3);
}

TEST_F(BatteryGateTest, ForwardAllowedWhenNotSuspended) {
  EXPECT_TRUE(gate.checkForward());   // gate off
  gate.setThresholds(3400, 3600);
  board.mV = 4000;
  gate.loop(&fs, board);
  EXPECT_TRUE(gate.checkForward());   // gate on, battery healthy
  EXPECT_EQ(gate.getDropCount(), 0);
}

TEST_F(BatteryGateTest, DropCounterResetsOnReboot) {
  gate.setThresholds(3400, 3600);
  board.mV = 3300;
  gate.loop(&fs, board);
  g_mock_millis += SAMPLE_INTERVAL_MS;
  gate.loop(&fs, board);
  EXPECT_FALSE(gate.checkForward());
  EXPECT_EQ(gate.getDropCount(), 1);

  // a reboot is a fresh object: begin() reloads config, counter starts at zero
  BatteryGate rebooted;
  rebooted.begin(&fs);
  EXPECT_EQ(rebooted.getDropCount(), 0);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
