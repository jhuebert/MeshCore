// BatteryGate.cpp — see BatteryGate.h for the design overview.

#include "BatteryGate.h"
#include "CliUtil.h"

#define BATT_CFG_FILE            "/batt_cfg"
#define BATT_CFG_VERSION         1
#define BATT_CFG_RECORD_BYTES    6      // ver, enabled, suspend_mV(2), resume_mV(2)

#define BATT_SAMPLE_INTERVAL_MS  30000  // sample every 30 s
#define BATT_DEBOUNCE_READINGS   2      // consecutive low readings to suspend
#define BATT_RESUME_MARGIN_MV    200    // default resume = suspend + this
#define BATT_USAGE "Err - usage: battery | battery off | battery <suspend-mV> [resume-mV]"

// One definition of a usable pair of thresholds, shared by setThresholds(),
// the CLI and load(), so a value the CLI refuses can never be applied by
// another route. Both ends live in 1..10000 mV and resume must exceed suspend.
static bool validThresholds(uint16_t suspend, uint16_t resume) {
  if (suspend < BATT_MV_MIN || suspend > BATT_MV_MAX) return false;
  if (resume < BATT_MV_MIN || resume > BATT_MV_MAX) return false;
  return resume > suspend;
}

// The state a node starts in, and the baseline load() resets to before it
// reads the file: gate off, no thresholds, nothing suspended. One definition,
// so construction and reload cannot drift apart.
void BatteryGate::resetToDefaults() {
  enabled = false;
  suspended = false;
  suspend_mV = 0;
  resume_mV = 0;
  low_count = 0;
  drops = 0;
  last_sample_ms = millis();
  sample_pending = true;   // sample immediately on the first loop()
}

BatteryGate::BatteryGate() {
  resetToDefaults();
}

void BatteryGate::begin(FILESYSTEM* fs) {
  load(fs);
}

void BatteryGate::setEnabled(bool on) {
  if (enabled != on) {
    enabled = on;
    markDirty();
  }
  if (!on) {
    suspended = false;
    low_count = 0;
    sample_pending = false;   // disarmed; re-armed for the next enable
  } else {
    sample_pending = true;    // evaluate the restored thresholds promptly
  }
}

bool BatteryGate::setThresholds(uint16_t suspend, uint16_t resume) {
  if (!validThresholds(suspend, resume)) return false;

  suspend_mV = suspend;
  resume_mV = resume;
  low_count = 0;
  sample_pending = true;   // re-evaluate immediately on the next loop()
  enabled = true;
  markDirty();
  return true;
}

void BatteryGate::sample(mesh::MainBoard& board) {
  uint16_t mV = board.getBattMilliVolts();

  if (suspended) {
    if (mV >= resume_mV) {
      suspended = false;   // hysteresis: recovered past the resume threshold
    }
    low_count = 0;
  } else if (mV < suspend_mV) {
    if (++low_count >= BATT_DEBOUNCE_READINGS) {
      suspended = true;    // two consecutive low readings: stop repeating
      low_count = 0;
    }
  } else {
    low_count = 0;
  }
}

void BatteryGate::loop(FILESYSTEM* fs, mesh::MainBoard& board) {
  if (enabled) {
    const uint32_t now = millis();
    // one sample per loop, never two to catch up missed intervals
    if (sample_pending || (uint32_t)(now - last_sample_ms) >= BATT_SAMPLE_INTERVAL_MS) {
      last_sample_ms = now;
      sample_pending = false;
      sample(board);
    }
  }
  if (save_flag.due()) save(fs);
}

void BatteryGate::load(FILESYSTEM* fs) {
  resetToDefaults();   // the file decides everything below; nothing survives from before
  save_flag.reset();   // a reload supersedes any edit still waiting to be written

  if (!fs->exists(BATT_CFG_FILE)) return;
  File file = fsOpenRead(fs, BATT_CFG_FILE);
  if (file) {
    uint8_t rec[BATT_CFG_RECORD_BYTES];
    if (file.read(rec, BATT_CFG_RECORD_BYTES) == BATT_CFG_RECORD_BYTES &&
        rec[0] == BATT_CFG_VERSION) {
      uint16_t s, r;
      memcpy(&s, &rec[2], 2);
      memcpy(&r, &rec[4], 2);
      // A disabled gate may legitimately keep 0/0 (the shipped default) or
      // retain real thresholds; anything else is a corrupt record. Validate the
      // whole record on locals first, so a bad one never assigns live settings.
      bool ok = (rec[1] == 0 && s == 0 && r == 0) ||
                ((rec[1] == 0 || rec[1] == 1) && validThresholds(s, r));
      if (!ok) {
        enabled = false;
        suspend_mV = 0;
        resume_mV = 0;
      } else {
        enabled = (rec[1] == 1);
        suspend_mV = s;
        resume_mV = r;
      }
    }
    file.close();
  }
}

void BatteryGate::save(FILESYSTEM* fs) {
  File file = fsOpenWrite(fs, BATT_CFG_FILE);
  if (!file) { save_flag.retryLater(); return; }   // wait out another delay before retrying
  uint8_t rec[BATT_CFG_RECORD_BYTES];
  rec[0] = BATT_CFG_VERSION;
  rec[1] = enabled ? 1 : 0;
  memcpy(&rec[2], &suspend_mV, 2);
  memcpy(&rec[4], &resume_mV, 2);
  bool ok = (file.write(rec, BATT_CFG_RECORD_BYTES) == BATT_CFG_RECORD_BYTES);
  file.close();
  // only once the config is actually on disk: a failed write stays pending, but
  // backs off a full delay instead of retrying on every loop
  if (ok) save_flag.clear();
  else save_flag.retryLater();
}

// ---------------------------------------------------------------- CLI

static bool parseMilliVolts(const char* tok, uint16_t* out) {
  long v;
  if (!parseIntRange(tok, BATT_MV_MIN, BATT_MV_MAX, &v)) return false;
  *out = (uint16_t)v;
  return true;
}

// why a millivolt token was refused: all digits but out of range, or not a
// number at all (which is a usage mistake, not a value mistake)
static void badMilliVolts(const char* tok, char* reply) {
  bool numeric = (tok != NULL && tok[0] != 0);
  for (const char* q = tok; numeric && *q; q++) {
    if (*q < '0' || *q > '9') numeric = false;
  }
  if (numeric) {
    snprintf(reply, CLI_REPLY_MAX, "Err - millivolts must be %d..%d", BATT_MV_MIN, BATT_MV_MAX);
  } else {
    snprintf(reply, CLI_REPLY_MAX, "%s", BATT_USAGE);
  }
}

static void cliStatus(BatteryGate& gate, mesh::MainBoard& board, char* reply) {
  char* out = reply;
  int remain = CLI_REPLY_MAX;
  radd(&out, &remain, "batt %umV; gate %s", board.getBattMilliVolts(),
       gate.isEnabled() ? "on" : "off");
  if (gate.isEnabled()) {
    radd(&out, &remain, "; %s", gate.isSuspended() ? "SUSPENDED" : "forwarding");
  }
  if (gate.getSuspendMilliVolts() != 0) {
    radd(&out, &remain, "; suspend <%umV; resume >=%umV",
         gate.getSuspendMilliVolts(), gate.getResumeMilliVolts());
  }
  radd(&out, &remain, "; drops %lu", (unsigned long)gate.getDropCount());
}

void batteryCLI(BatteryGate& gate, mesh::MainBoard& board, const char* command, char* reply) {
  char buf[MAX_PACKET_PAYLOAD + 1];
  cliCopyCommand(buf, sizeof(buf), command);
  char* p = buf;
  char* cmd = nextToken(&p);

  if (cmd == NULL) {
    cliStatus(gate, board, reply);
  } else if (strcmp(cmd, "off") == 0) {
    gate.setEnabled(false);
    snprintf(reply, CLI_REPLY_MAX, "OK - battery gate off");
  } else {
    uint16_t susp, res;
    if (!parseMilliVolts(cmd, &susp)) {
      badMilliVolts(cmd, reply);
      return;
    }
    char* tok = nextToken(&p);
    if (tok != NULL) {
      if (!parseMilliVolts(tok, &res)) {
        badMilliVolts(tok, reply);
        return;
      }
      if (res <= susp) {
        snprintf(reply, CLI_REPLY_MAX, "Err - resume must be greater than suspend");
        return;
      }
      if (nextToken(&p) != NULL) {
        snprintf(reply, CLI_REPLY_MAX, "%s", BATT_USAGE);
        return;
      }
    } else {
      // default hysteresis margin, computed wide enough not to wrap, and
      // rejected if it would fall outside the accepted range — never clamped
      long implicit_resume = (long)susp + BATT_RESUME_MARGIN_MV;
      if (implicit_resume > BATT_MV_MAX) {
        snprintf(reply, CLI_REPLY_MAX, "Err - default resume exceeds %umV; specify resume", BATT_MV_MAX);
        return;
      }
      res = (uint16_t)implicit_resume;
    }
    if (!gate.setThresholds(susp, res)) {   // no path may report a rejected config as applied
      badMilliVolts(cmd, reply);
      return;
    }
    snprintf(reply, CLI_REPLY_MAX, "OK - gate on; suspend <%umV; resume >=%umV", susp, res);
  }
}
