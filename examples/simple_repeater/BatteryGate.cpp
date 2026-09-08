// BatteryGate.cpp — see BatteryGate.h for the design overview.

#include "BatteryGate.h"

#define BATT_CFG_FILE            "/batt_cfg"
#define BATT_CFG_VERSION         1
#define BATT_CFG_RECORD_BYTES    6      // ver, enabled, suspend_mV(2), resume_mV(2)

#define BATT_SAMPLE_INTERVAL_MS  30000  // sample every 30 s
#define BATT_DEBOUNCE_READINGS   2      // consecutive low readings to suspend
#define BATT_RESUME_MARGIN_MV    200    // default resume = suspend + this
#define BATT_SAVE_DELAY_MS       3000   // lazy dirty-write delay (like ClientACL)

BatteryGate::BatteryGate() {
  enabled = false;
  suspended = false;
  suspend_mV = 0;
  resume_mV = 0;
  low_count = 0;
  drops = 0;
  next_sample_at = 0;   // sample immediately on the first loop()
  dirty = false;
  dirty_since = 0;
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
    next_sample_at = 0;
  }
}

bool BatteryGate::setThresholds(uint16_t suspend, uint16_t resume) {
  if (suspend < BATT_MV_MIN || suspend > BATT_MV_MAX) return false;
  if (resume < BATT_MV_MIN || resume > BATT_MV_MAX) return false;
  if (resume <= suspend) return false;

  suspend_mV = suspend;
  resume_mV = resume;
  low_count = 0;
  next_sample_at = 0;   // re-evaluate immediately on the next loop()
  if (!enabled) {
    enabled = true;
  }
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
    unsigned long now = millis();
    if (next_sample_at == 0 || (long)(now - next_sample_at) >= 0) {
      next_sample_at = now + BATT_SAMPLE_INTERVAL_MS;
      sample(board);
    }
  }
  if (dirty && millis() - dirty_since >= BATT_SAVE_DELAY_MS) {
    save(fs);
  }
}

void BatteryGate::load(FILESYSTEM* fs) {
  enabled = false;
  suspended = false;
  suspend_mV = 0;
  resume_mV = 0;
  low_count = 0;
  drops = 0;
  next_sample_at = 0;

  if (!fs->exists(BATT_CFG_FILE)) return;
#if defined(RP2040_PLATFORM)
  File file = fs->open(BATT_CFG_FILE, "r");
#else
  File file = fs->open(BATT_CFG_FILE);
#endif
  if (file) {
    uint8_t rec[BATT_CFG_RECORD_BYTES];
    if (file.read(rec, BATT_CFG_RECORD_BYTES) == BATT_CFG_RECORD_BYTES &&
        rec[0] == BATT_CFG_VERSION) {
      enabled = rec[1] != 0;
      memcpy(&suspend_mV, &rec[2], 2);
      memcpy(&resume_mV, &rec[4], 2);
      if (resume_mV <= suspend_mV) {   // defensive: corrupted config
        enabled = false;
        suspend_mV = 0;
        resume_mV = 0;
      }
    }
    file.close();
  }
}

void BatteryGate::save(FILESYSTEM* fs) {
  dirty = false;
  #if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
    fs->remove(BATT_CFG_FILE);
    File file = fs->open(BATT_CFG_FILE, FILE_O_WRITE);
  #elif defined(RP2040_PLATFORM)
    File file = fs->open(BATT_CFG_FILE, "w");
  #else
    File file = fs->open(BATT_CFG_FILE, "w", true);
  #endif
  if (file) {
    uint8_t rec[BATT_CFG_RECORD_BYTES];
    rec[0] = BATT_CFG_VERSION;
    rec[1] = enabled ? 1 : 0;
    memcpy(&rec[2], &suspend_mV, 2);
    memcpy(&rec[4], &resume_mV, 2);
    file.write(rec, BATT_CFG_RECORD_BYTES);
    file.close();
  }
}

// ---------------------------------------------------------------- CLI

static char* nextToken(char** p) {
  char* s = *p;
  while (*s == ' ') s++;
  if (*s == 0) { *p = s; return NULL; }
  char* t = s;
  while (*s && *s != ' ') s++;
  if (*s) { *s = 0; s++; }
  *p = s;
  return t;
}

// bounded reply append (CLI reply buffer is 160 bytes)
static void radd(char** out, int* remain, const char* fmt, ...) {
  if (*remain <= 0) return;
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(*out, *remain, fmt, ap);
  va_end(ap);
  if (n < 0) { *remain = 0; return; }
  if (n >= *remain) { *out += *remain - 1; *remain = 0; return; }
  *out += n;
  *remain -= n;
}

static bool parseMilliVolts(const char* tok, bool* numeric, uint16_t* out) {
  *numeric = false;
  if (tok == NULL || tok[0] == 0) return false;
  for (const char* q = tok; *q; q++) {
    if (*q < '0' || *q > '9') return false;
  }
  *numeric = true;
  long v = strtol(tok, NULL, 10);
  if (v < BATT_MV_MIN || v > BATT_MV_MAX) return false;
  *out = (uint16_t)v;
  return true;
}

static void cliStatus(BatteryGate& gate, mesh::MainBoard& board, char* reply) {
  char* out = reply;
  int remain = MAX_PACKET_PAYLOAD;
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
  strncpy(buf, command, sizeof(buf) - 1);
  buf[sizeof(buf) - 1] = 0;
  char* p = buf;
  char* cmd = nextToken(&p);

  if (cmd == NULL) {
    cliStatus(gate, board, reply);
  } else if (strcmp(cmd, "off") == 0) {
    gate.setEnabled(false);
    strcpy(reply, "OK - battery gate off");
  } else {
    uint16_t susp, res;
    bool numeric;
    if (!parseMilliVolts(cmd, &numeric, &susp)) {
      if (numeric) {
        sprintf(reply, "Err - millivolts must be %d..%d", BATT_MV_MIN, BATT_MV_MAX);
      } else {
        strcpy(reply, "Err - usage: battery | battery off | battery <suspend-mV> [resume-mV]");
      }
      return;
    }
    char* tok = nextToken(&p);
    if (tok != NULL) {
      if (!parseMilliVolts(tok, &numeric, &res)) {
        if (numeric) {
          sprintf(reply, "Err - millivolts must be %d..%d", BATT_MV_MIN, BATT_MV_MAX);
        } else {
          strcpy(reply, "Err - usage: battery | battery off | battery <suspend-mV> [resume-mV]");
        }
        return;
      }
      if (res <= susp) {
        strcpy(reply, "Err - resume must be greater than suspend");
        return;
      }
      if (nextToken(&p) != NULL) {
        strcpy(reply, "Err - usage: battery | battery off | battery <suspend-mV> [resume-mV]");
        return;
      }
    } else {
      res = susp + BATT_RESUME_MARGIN_MV;   // default hysteresis margin
    }
    gate.setThresholds(susp, res);
    sprintf(reply, "OK - gate on; suspend <%umV; resume >=%umV", susp, res);
  }
}
