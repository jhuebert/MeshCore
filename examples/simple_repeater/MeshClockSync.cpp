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

// The state a fresh node starts in: disabled, empty batch, no decision yet. One
// definition, so construction and reload cannot drift apart.
void MeshClockSync::reset() {
  memset(samples, 0, sizeof(samples));
  count = 0;
  enabled = false;
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

// ---------------------------------------------------------------- collection

bool MeshClockSync::hasSource(const uint8_t* fp) const {
  // linear scan: count <= MESHCLOCK_SLOTS, so it is cheaper than any index and
  // needs no extra state
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

  if (count == MESHCLOCK_SLOTS) evaluate(local_now);
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
  const uint32_t median = sorted[count / 2];   // odd batch: the single middle value

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
    if (d <= MESHCLOCK_TOLERANCE_SECS) inliers++;
  }
  if (inliers < MESHCLOCK_MIN_AGREE) {
    last_result = NO_CONSENSUS;
    clear();
    return;
  }

  // deadband vs the node's own clock
  const uint32_t d = median > local_now ? median - local_now : local_now - median;
  last_delta = (int32_t)(median - local_now);   // exact within ±68 years
  if (d < MESHCLOCK_MIN_DIFF_SECS) {
    last_result = IN_SYNC;
    clear();
    return;
  }

  if (set_fn) set_fn(set_ctx, median);
  last_result = SET_CLOCK;
  clear();
}

// ---------------------------------------------------------------- persistence

// Format-level validity of a record: version, an enabled flag of 0/1, and the
// CRC. Shared by the file validator, the staged-save writeback check and load(),
// so no route can admit a record another refuses.
static bool clockRecordValid(const MeshClockCfg& rec) {
  if (rec.version != MESHCLOCK_CFG_VERSION) return false;
  if (rec.enabled > 1) return false;
  return rec.crc == crc16_ccitt((const uint8_t*)&rec, offsetof(MeshClockCfg, crc));
}

static bool clockWriteFile(File& f, void* ctx) {
  const MeshClockSync& c = *(const MeshClockSync*)ctx;
  MeshClockCfg rec;
  rec.version = MESHCLOCK_CFG_VERSION;
  rec.enabled = c.isEnabled() ? 1 : 0;
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

void MeshClockSync::save(FILESYSTEM* fs) {
  // Staged save: scratch file, read back, keep the previous good file as a
  // backup, then promote — the same transaction every persisted config here uses.
  bool ok = saveStaged(fs, MESHCLOCK_CFG_FILE, clockWriteFile, this,
                       clockValidateFile, NULL, clockValidateFile, NULL);
  if (ok) save_flag.clear();
  else save_flag.retryLater();
}

void MeshClockSync::load(FILESYSTEM* fs) {
  reset();   // the file decides everything below; nothing survives from before

  char path[64];
  PersistLoadSource src = chooseConfigToLoad(fs, MESHCLOCK_CFG_FILE, clockValidateFile, NULL,
                                             path, sizeof(path));
  if (src == PERSIST_LOAD_NONE) return;   // nothing usable: keep the defaults
  File file = fsOpenRead(fs, path);
  if (!file) return;

  MeshClockCfg rec;
  const bool ok = file.read((uint8_t*)&rec, sizeof(rec)) == sizeof(rec);
  file.close();
  if (!ok) return;
  if (!clockRecordValid(rec)) return;   // chooseConfigToLoad validated it; re-verify anyway

  enabled = (rec.enabled == 1);

  // Loaded from the backup, so put a good canonical file back at the next
  // opportunity; the backup stays valid meanwhile.
  if (src == PERSIST_LOAD_RECOVERED) markDirty();
}

// ---------------------------------------------------------------- CLI

#define MESHCLOCK_USAGE "Err - clock mesh takes on|off"

static void clockStatus(MeshClockSync& c, char* reply) {
  if (!c.isEnabled()) {
    strcpy(reply, "off");
    return;
  }
  char* out = reply;
  int remain = CLI_REPLY_MAX;
  radd(&out, &remain, "on samples=%d/%d last=", c.getSampleCount(), MESHCLOCK_SLOTS);
  switch (c.getLastResult()) {
    case MeshClockSync::SET_CLOCK:
      radd(&out, &remain, "set(%+lds)", (long)c.getLastDeltaSecs());
      break;
    case MeshClockSync::IN_SYNC: {
      long d = (long)c.getLastDeltaSecs();
      if (d < 0) d = -d;
      radd(&out, &remain, "in-sync(%lds)", d);
      break;
    }
    case MeshClockSync::NO_CONSENSUS:
      radd(&out, &remain, "no-consensus");
      break;
    default:
      radd(&out, &remain, "idle");
      break;
  }
}

void meshClockCLI(MeshClockSync& clock, const char* command, char* reply) {
  // An oversized command is refused whole rather than acted on as a prefix.
  char buf[MAX_PACKET_PAYLOAD + 1];
  if (!cliCopyCommand(buf, sizeof(buf), command)) { strcpy(reply, "Err - command too long"); return; }
  char* p = buf;
  char* cmd = nextToken(&p);

  if (cmd == NULL) {
    if (!cliNoExtra(p, reply, MESHCLOCK_USAGE)) return;
    clockStatus(clock, reply);
  } else if (strcmp(cmd, "on") == 0) {
    if (!cliNoExtra(p, reply, MESHCLOCK_USAGE)) return;
    clock.setEnabled(true);
    strcpy(reply, "OK - mesh clock sync on");
  } else if (strcmp(cmd, "off") == 0) {
    if (!cliNoExtra(p, reply, MESHCLOCK_USAGE)) return;
    clock.setEnabled(false);
    strcpy(reply, "OK - mesh clock sync off");
  } else {
    strcpy(reply, MESHCLOCK_USAGE);
  }
}
