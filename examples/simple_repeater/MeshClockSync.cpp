// MeshClockSync.cpp — see MeshClockSync.h for the design overview and
// MESHCLOCK.md for the user manual.

#include "MeshClockSync.h"
#include "BuildEpoch.h"
#include "CliUtil.h"

#include <stddef.h>

// Fingerprint: 4 bytes sampled from the advert pubkey at fixed offsets clear of
// the vanity zones at both ends (the same idea AdvertRateLimiter uses). This is
// deliberately a private copy, not shared with AdvertRateLimiter: keeping clock
// sync self-contained is what makes it deletable if upstream adds its own clock
// feature. A 4-byte collision just drops the second key's vote for one batch
// (rare; accepted rather than widening the fingerprint and spending RAM).
static const uint8_t MESHCLOCK_KEY_OFFSETS[4] = { 8, 14, 20, 26 };

// Strict plausibility floor: an advert timestamp before this firmware's build
// timestamp cannot contribute a vote. buildEpoch() interprets the compiler's
// local __DATE__/__TIME__ as UTC; no grace is subtracted, so earlier timestamps
// are always refused.
uint32_t MeshClockSync::buildFloor() {
  return meshclock::buildEpoch();
}

MeshClockSync::MeshClockSync() {
  reset();
}

// The knob pair must satisfy the same rule the compile-time defaults do:
// quorum within [2, slots], batch size within the array the samples are sized
// for. One definition, shared by the setters, the CLI and the loader, so a
// value no route can produce is a value no route can apply.
static bool validSlots(uint8_t slots, uint8_t min_agree) {
  if (slots < MESHCLOCK_SLOTS_MIN || slots > MESHCLOCK_SLOTS) return false;
  if (min_agree < 2 || min_agree > slots) return false;
  return true;
}

// Zero tolerance would deadlock consensus outright (no advert ever lands
// within zero seconds of another), so it is the one value refused; the
// deadband has no such floor — 0 means the clock always steps.
static bool validTolerance(uint16_t tolerance) {
  return tolerance >= 1;
}

// The state a fresh node starts in: disabled, empty batch, no decision yet,
// knobs at the compile-time defaults. One definition, so construction and
// reload cannot drift apart.
void MeshClockSync::reset() {
  memset(samples, 0, sizeof(samples));
  count = 0;
  enabled = false;
  slots = MESHCLOCK_SLOTS;
  min_agree = MESHCLOCK_MIN_AGREE;
  tolerance = MESHCLOCK_TOLERANCE_SECS;
  min_diff = MESHCLOCK_MIN_DIFF_SECS;
  last_result = IDLE;
  last_delta = 0;
  save_flag.reset();   // a reset supersedes any edit still waiting to be written
}

void MeshClockSync::begin(FILESYSTEM* fs) {
  load(fs);
}

void MeshClockSync::loop(FILESYSTEM* fs) {
  if (save_flag.due()) save(fs);
}

void MeshClockSync::setClockFn(void (*fn)(void* ctx, uint32_t epoch), void* ctx) {
  set_fn = fn;
  set_ctx = ctx;
}

void MeshClockSync::setEnabled(bool on) {
  if (enabled == on) return;   // no-op: no dirty write
  enabled = on;
  clear();                     // a stale batch must not survive an off->on toggle
  markDirty();
}

bool MeshClockSync::setSlots(uint8_t new_slots, uint8_t new_min_agree) {
  if (!validSlots(new_slots, new_min_agree)) return false;
  if (slots == new_slots && min_agree == new_min_agree) return true;   // no-op: no dirty write
  slots = new_slots;
  min_agree = new_min_agree;
  clear();   // samples gathered under the old quorum must not decide under the new one
  markDirty();
  return true;
}

bool MeshClockSync::setTolerance(uint16_t new_tolerance, uint16_t new_min_diff) {
  if (!validTolerance(new_tolerance)) return false;
  if (tolerance == new_tolerance && min_diff == new_min_diff) return true;   // no-op
  tolerance = new_tolerance;
  min_diff = new_min_diff;
  clear();   // samples gathered under the old window must not decide under the new one
  markDirty();
  return true;
}

// ---------------------------------------------------------------- collection

bool MeshClockSync::hasSource(const uint8_t* fp) const {
  // linear scan: count <= slots (<= MESHCLOCK_SLOTS), so it is cheaper than any
  // index and needs no extra state
  for (int i = 0; i < count; i++) {
    if (memcmp(samples[i].fp, fp, 4) == 0) return true;
  }
  return false;
}

void MeshClockSync::onAdvert(uint32_t advert_ts, const uint8_t* pub_key, uint32_t local_now) {
  if (!enabled) return;

  // signed comparison of a modular difference: correct across the 2038 wrap
  if ((int32_t)(advert_ts - buildFloor()) < 0) return;

  uint8_t fp[4];
  for (int i = 0; i < 4; i++) fp[i] = pub_key[MESHCLOCK_KEY_OFFSETS[i]];
  if (hasSource(fp)) return;   // one vote per source per batch

  memcpy(samples[count].fp, fp, sizeof(fp));
  samples[count].offset = advert_ts - local_now;   // uint32 wrap
  count++;

  if (count == slots) evaluate(local_now);
}

// ---------------------------------------------------------------- decision

void MeshClockSync::evaluate(uint32_t local_now) {
  // Reconstruct each source's predicted current UTC: local_now + offset. Because
  // the local clock advances at ~real rate even while wrong, local_now now minus
  // local_now at receipt equals the real elapsed time, so every sample predicts
  // the same "now" — samples taken hours apart still cluster. That is why no
  // per-sample uptime and no collection window are needed.
  uint32_t cand[MESHCLOCK_SLOTS];
  for (int i = 0; i < count; i++) cand[i] = local_now + samples[i].offset;

  // Median, not mean: robust to a minority of wrong clocks and needing no
  // floating point. A mean would be dominated by the most frequent advertiser.
  uint32_t sorted[MESHCLOCK_SLOTS];
  memcpy(sorted, cand, count * sizeof(uint32_t));
  for (int i = 1; i < count; i++) {   // small insertion sort
    uint32_t v = sorted[i];
    int j = i - 1;
    while (j >= 0 && sorted[j] > v) { sorted[j + 1] = sorted[j]; j--; }
    sorted[j + 1] = v;
  }
  const uint32_t median = sorted[count / 2];   // odd batch: the middle value; even: the upper-middle

  // defensive floor check on the result
  if ((int32_t)(median - buildFloor()) < 0) {
    last_result = NO_CONSENSUS;
    clear();
    return;
  }

  // Inlier count, not standard deviation: one extreme value inflates a standard
  // deviation even when the rest agree; a count answers "is there a cluster?".
  int inliers = 0;
  for (int i = 0; i < count; i++) {
    const uint32_t d = cand[i] > median ? cand[i] - median : median - cand[i];
    if (d <= tolerance) inliers++;
  }
  if (inliers < min_agree) {
    last_result = NO_CONSENSUS;
    clear();
    return;
  }

  // deadband vs the node's own clock: a difference of min_diff or less is in
  // sync (the knob is the minimum difference that counts as out of sync, and
  // MESHCLOCK.md says "within 60 s" for the default)
  const uint32_t d = median > local_now ? median - local_now : local_now - median;
  last_delta = (int32_t)(median - local_now);   // exact within ±68 years
  if (d <= min_diff) {
    last_result = IN_SYNC;
    clear();
    return;
  }

  if (set_fn == NULL) {
    // no clock service wired: the decision cannot be carried out, and nothing
    // is claimed either way
    clear();
    return;
  }
  set_fn(set_ctx, median);
  last_result = SET_CLOCK;
  clear();
}

// ---------------------------------------------------------------- persistence

// Format-level validity of a v2 record: version, an enabled flag of 0/1, the
// shared knob rules and the CRC. Shared by the file validator, the staged-save
// writeback check and load(), so no route can admit a record another refuses.
static bool clockRecordValid(const MeshClockCfg& rec) {
  if (rec.version != MESHCLOCK_CFG_VERSION) return false;
  if (rec.enabled > 1) return false;
  if (!validSlots(rec.slots, rec.min_agree)) return false;
  if (!validTolerance(rec.tolerance)) return false;
  return rec.crc == crc16_ccitt((const uint8_t*)&rec, offsetof(MeshClockCfg, crc));
}

// A v1 record (enabled flag only, no knobs): the format the feature shipped
// with. Still read as a last-resort source by load() — the flag carries, the
// knobs take the compile-time defaults, and the next save writes v2 — so a
// config written by the shipped firmware is never lost.
struct MeshClockCfgV1 {
  uint8_t  version;   // 1
  uint8_t  enabled;   // 0/1
  uint16_t crc;       // crc16_ccitt over the version + enabled bytes
};
static_assert(sizeof(MeshClockCfgV1) == 4, "clock v1 record must stay 4 bytes");

static bool clockRecordValidV1(const MeshClockCfgV1& rec) {
  if (rec.version != 1) return false;
  if (rec.enabled > 1) return false;
  return rec.crc == crc16_ccitt((const uint8_t*)&rec, offsetof(MeshClockCfgV1, crc));
}

static bool clockWriteFile(File& f, void* ctx) {
  const MeshClockSync& c = *(const MeshClockSync*)ctx;
  MeshClockCfg rec;
  rec.version = MESHCLOCK_CFG_VERSION;
  rec.enabled = c.isEnabled() ? 1 : 0;
  rec.slots = (uint8_t)c.getSlots();
  rec.min_agree = (uint8_t)c.getMinAgree();
  rec.tolerance = (uint16_t)c.getToleranceSecs();
  rec.min_diff = (uint16_t)c.getMinDiffSecs();
  rec.crc = crc16_ccitt((const uint8_t*)&rec, offsetof(MeshClockCfg, crc));
  return f.write((const uint8_t*)&rec, sizeof(rec)) == sizeof(rec);
}

static bool clockValidateFile(File& f, void* ctx) {
  (void)ctx;
  MeshClockCfg rec;
  if (f.read((uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) return false;
  uint8_t extra;
  if (f.read(&extra, 1) != 0) return false;   // record must be exactly sizeof(rec)
  return clockRecordValid(rec);
}

static bool clockValidateFileV1(File& f, void* ctx) {
  (void)ctx;
  MeshClockCfgV1 rec;
  if (f.read((uint8_t*)&rec, sizeof(rec)) != sizeof(rec)) return false;
  uint8_t extra;
  if (f.read(&extra, 1) != 0) return false;   // record must be exactly sizeof(rec)
  return clockRecordValidV1(rec);
}

// usableFn for the staged save's canonical check only: accepts any format
// this firmware can load (v1 or v2), so a pre-v2 config is kept as the
// backup of the migration save rather than discarded for being old. The
// readback check for what this firmware writes stays strict (clockValidateFile).
static bool clockValidateFileAnyFormat(File& f, void* ctx) {
  (void)ctx;
  uint8_t ver;
  if (f.read(&ver, 1) != 1) return false;
  if (ver != 1 && ver != MESHCLOCK_CFG_VERSION) return false;
  if (ver == 1) {
    MeshClockCfgV1 rec;
    rec.version = ver;
    if (f.read(((uint8_t*)&rec) + 1, sizeof(rec) - 1) != sizeof(rec) - 1) return false;
    uint8_t extra;
    if (f.read(&extra, 1) != 0) return false;
    return clockRecordValidV1(rec);
  }
  MeshClockCfg rec;
  rec.version = ver;
  if (f.read(((uint8_t*)&rec) + 1, sizeof(rec) - 1) != sizeof(rec) - 1) return false;
  uint8_t extra;
  if (f.read(&extra, 1) != 0) return false;
  return clockRecordValid(rec);
}

void MeshClockSync::save(FILESYSTEM* fs) {
  // Staged save: scratch file, read back, keep the previous good file as a
  // backup, then promote — the same transaction every persisted config here uses.
  bool ok = saveStaged(fs, MESHCLOCK_CFG_FILE, clockWriteFile, this,
                       clockValidateFile, NULL, clockValidateFileAnyFormat, NULL);
  if (ok) save_flag.clear();
  else save_flag.retryLater();
}

void MeshClockSync::load(FILESYSTEM* fs) {
  reset();   // the file decides everything below; nothing survives from before

  char path[64];
  PersistLoadSource src = chooseConfigToLoad(fs, MESHCLOCK_CFG_FILE, clockValidateFile, NULL,
                                             path, sizeof(path));
  if (src != PERSIST_LOAD_NONE) {
    File file = fsOpenRead(fs, path);
    if (!file) return;

    MeshClockCfg rec;
    const bool ok = file.read((uint8_t*)&rec, sizeof(rec)) == sizeof(rec);
    file.close();
    if (!ok) return;
    if (!clockRecordValid(rec)) return;   // chooseConfigToLoad validated it; re-verify anyway

    enabled = (rec.enabled == 1);
    slots = rec.slots;
    min_agree = rec.min_agree;
    tolerance = rec.tolerance;
    min_diff = rec.min_diff;

    // Loaded from the backup, so put a good canonical file back at the next
    // opportunity; the backup stays valid meanwhile.
    if (src == PERSIST_LOAD_RECOVERED) markDirty();
    return;
  }

  // No usable v2 record anywhere: migrate a v1 one if it exists (the format
  // the feature shipped with). The enabled flag carries; the knobs take the
  // compile-time defaults (reset() above), and the v1 bytes stay as the
  // backup of the migrated save.
  src = chooseConfigToLoad(fs, MESHCLOCK_CFG_FILE, clockValidateFileV1, NULL,
                           path, sizeof(path));
  if (src == PERSIST_LOAD_NONE) return;   // nothing usable: keep the defaults
  File file = fsOpenRead(fs, path);
  if (!file) return;

  MeshClockCfgV1 rec;
  const bool ok = file.read((uint8_t*)&rec, sizeof(rec)) == sizeof(rec);
  file.close();
  if (!ok) return;
  if (!clockRecordValidV1(rec)) return;   // re-verify anyway

  enabled = (rec.enabled == 1);
  markDirty();   // nothing was read into the knobs: reset() holds the defaults
}

// ---------------------------------------------------------------- CLI

#define MESHCLOCK_USAGE "Err - clock mesh takes on|off|slots|tolerance"

static void clockStatus(MeshClockSync& c, const char* now, char* reply) {
  CliBuf buf(reply, CLI_REPLY_MAX);
  if (c.isEnabled()) {
    buf.add("on samples=%d/%d last=", c.getSampleCount(), c.getSlots());
    switch (c.getLastResult()) {
      case MeshClockSync::SET_CLOCK:
        buf.add("set(%+lds)", (long)c.getLastDeltaSecs());
        break;
      case MeshClockSync::IN_SYNC: {
        long d = (long)c.getLastDeltaSecs();
        if (d < 0) d = -d;
        buf.add("in-sync(%lds)", d);
        break;
      }
      case MeshClockSync::NO_CONSENSUS:
        buf.add("no-consensus");
        break;
      default:
        buf.add("idle");
        break;
    }
  } else {
    buf.addStr("off");
  }
  // One append point: the time and the configured knobs show in both states
  // and cannot drift between them.
  buf.add(" slots=%d/%d tol=%d/%d now=%s", c.getSlots(), c.getMinAgree(),
          c.getToleranceSecs(), c.getMinDiffSecs(), now);
}

void meshClockCLI(MeshClockSync& clock, const char* now, const char* command, char* reply) {
  // An oversized command is refused whole rather than acted on as a prefix, and
  // unbalanced quotes are an error rather than a silent misparse.
  char scratch[MAX_PACKET_PAYLOAD + 1];
  if (!cliPrepare(command, scratch, sizeof(scratch), reply)) return;
  char* p = scratch;
  char* cmd = nextToken(&p);

  if (cmd == NULL) {
    if (!cliNoExtra(p, reply, MESHCLOCK_USAGE)) return;
    clockStatus(clock, now, reply);
  } else if (strcmp(cmd, "on") == 0) {
    if (!cliNoExtra(p, reply, MESHCLOCK_USAGE)) return;
    clock.setEnabled(true);
    strcpy(reply, "OK - mesh clock sync on");
  } else if (strcmp(cmd, "off") == 0) {
    if (!cliNoExtra(p, reply, MESHCLOCK_USAGE)) return;
    clock.setEnabled(false);
    strcpy(reply, "OK - mesh clock sync off");
  } else if (strcmp(cmd, "slots") == 0) {
    // `clock mesh slots <slots> <min-agree>`: both values required, applied as
    // a pair or not at all
    char* tok = nextToken(&p);
    long s;
    if (!parseIntRange(tok, MESHCLOCK_SLOTS_MIN, MESHCLOCK_SLOTS, &s)) {
      if (allDigits(tok)) {
        snprintf(reply, CLI_REPLY_MAX, "Err - slots must be %d..%d",
                 MESHCLOCK_SLOTS_MIN, MESHCLOCK_SLOTS);
      } else {
        strcpy(reply, MESHCLOCK_USAGE);
      }
      return;
    }
    tok = nextToken(&p);
    long a;
    if (!parseIntRange(tok, 2, s, &a)) {
      if (allDigits(tok)) {
        snprintf(reply, CLI_REPLY_MAX, "Err - min-agree must be 2..%ld", s);
      } else {
        strcpy(reply, MESHCLOCK_USAGE);
      }
      return;
    }
    if (!cliNoExtra(p, reply, MESHCLOCK_USAGE)) return;
    if (!clock.setSlots((uint8_t)s, (uint8_t)a)) {   // prevalidated above; never claim applied
      snprintf(reply, CLI_REPLY_MAX, "Err - min-agree must be 2..%ld", s);
      return;
    }
    snprintf(reply, CLI_REPLY_MAX, "OK - mesh clock slots %ld/%ld", s, a);
  } else if (strcmp(cmd, "tolerance") == 0) {
    // `clock mesh tolerance <tolerance-s> <min-diff-s>`: both values required
    char* tok = nextToken(&p);
    long t;
    if (!parseIntRange(tok, 1, 65535, &t)) {
      if (allDigits(tok)) {
        strcpy(reply, "Err - tolerance must be 1..65535 seconds");
      } else {
        strcpy(reply, MESHCLOCK_USAGE);
      }
      return;
    }
    tok = nextToken(&p);
    long d;
    if (!parseIntRange(tok, 0, 65535, &d)) {
      if (allDigits(tok)) {
        strcpy(reply, "Err - min-diff must be 0..65535 seconds");
      } else {
        strcpy(reply, MESHCLOCK_USAGE);
      }
      return;
    }
    if (!cliNoExtra(p, reply, MESHCLOCK_USAGE)) return;
    if (!clock.setTolerance((uint16_t)t, (uint16_t)d)) {   // prevalidated above; never claim applied
      strcpy(reply, "Err - tolerance must be 1..65535 seconds");
      return;
    }
    snprintf(reply, CLI_REPLY_MAX, "OK - mesh clock tolerance %ld/%ld", t, d);
  } else {
    strcpy(reply, MESHCLOCK_USAGE);
  }
}
