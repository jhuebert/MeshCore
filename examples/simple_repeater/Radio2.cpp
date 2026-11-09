// Radio2.cpp — second radio preset: config, policy, scheduler and CLI.
// Fork-owned; see RADIO2.md.

#include "Radio2.h"

#include <math.h>
#include <string.h>

#include <Utils.h>

#include "CliUtil.h"

// ---------------------------------------------------------------- helpers

static const char* actionName(uint8_t a) {
  switch (a) {
    case RADIO2_ACTION_SAME: return "same";
    case RADIO2_ACTION_OTHER: return "other";
    case RADIO2_ACTION_BOTH: return "both";
    case RADIO2_ACTION_DROP: return "drop";
  }
  return "same";
}

static bool actionFromName(const char* s, uint8_t* out) {
  for (uint8_t i = 0; i <= RADIO2_ACTION_DROP; i++) {
    if (strcmp(s, actionName(i)) == 0) { *out = i; return true; }
  }
  return false;
}

static const char* conflictName(Radio2Conflict c) {
  switch (c) {
    case RADIO2_CONFLICT_RXPS: return "radio.rxps";
    case RADIO2_CONFLICT_EXTRA_SF: return "extra.sf";
    case RADIO2_CONFLICT_PRESET: return "preset";
    case RADIO2_CONFLICT_TIMING: return "timing";
    case RADIO2_CONFLICT_SWITCH: return "switch";
    default: return "none";
  }
}

static bool validTuple(float freq, float bw, uint8_t sf, uint8_t cr) {
  if (!isfinite(freq) || !isfinite(bw)) return false;
  return freq >= 150.0f && freq <= 2500.0f &&
         bw >= 7.0f && bw <= 500.0f &&
         sf >= 5 && sf <= 12 && cr >= 5 && cr <= 8;
}

static bool overlaps(const Radio2Profile& a, const Radio2Profile& b) {
  if (a.bw <= 0 || b.bw <= 0) return false;
  // freq is MHz, bandwidth and the guard are kHz.
  return fabsf(a.freq - b.freq) * 1000.0f <
         (a.bw + b.bw) / 2.0f + RADIO2_OVERLAP_GUARD_KHZ;
}

static uint32_t toUs(float v) {
  if (!(v > 0.0f)) return 0;
  if (v > 4000000000.0f) return 0xFFFFFFFFUL;
  return (uint32_t)(v + 0.5f);
}

// capture/bound from RADIO2.md §9.3, with the preamble length mirroring
// RadioLibWrapper::preambleLengthForSF().
static void profileTiming(const Radio2Profile& p, float capture_symbols,
                          uint32_t* capture_us, uint32_t* bound_us) {
  uint32_t preamble_symbols = p.sf <= 8 ? 32 : 16;
  float symbol_us = ((float)(1UL << p.sf) * 1000.0f) / p.bw;
  float capture = capture_symbols * symbol_us;
  float bound = ((float)preamble_symbols - capture_symbols) * symbol_us
                - 2.0f * (float)RADIO2_SWITCH_US - (float)RADIO2_LOOP_MARGIN_US;
  *capture_us = toUs(capture);
  *bound_us = toUs(bound);
}

static uint32_t maxUs(uint32_t a, uint32_t b) { return a > b ? a : b; }
static uint32_t usToMs(uint32_t us) { return (us + 999) / 1000; }

// ---------------------------------------------------------------- lifecycle

Radio2::Radio2(Radio2Device& dev) : device(dev) {
  resetToDefaults();
}

void Radio2::resetToDefaults() {
  memset(&cfg, 0, sizeof(cfg));
  memcpy(cfg.magic, RADIO2_CFG_MAGIC, 4);
  cfg.version = RADIO2_CFG_VERSION;
  cfg.enabled = 0;
  cfg.policy_primary = RADIO2_ACTION_SAME;
  cfg.policy_secondary = RADIO2_ACTION_SAME;
  cfg.advert = RADIO2_ADVERT_PRIMARY;
  memset(&primary, 0, sizeof(primary));
  memset(&secondary, 0, sizeof(secondary));

  active = false;
  paused = false;
  pending_apply = false;
  pending_apply_at_ms = 0;
  conflict_state = RADIO2_CONFLICT_NONE;
  phase = PHASE_PRIMARY;
  current = RADIO_PROFILE_PRIMARY;
  phase_end_ms = 0;
  last_primary_packet_ms = 0;
  switch_failures = 0;

  rx_primary = rx_secondary = tx_primary = tx_secondary = 0;
  both = both_drop = policy_drop = aborts = 0;
  switches = switch_errors = 0;
  save_flag.reset();
}

void Radio2::begin(FILESYSTEM* fs) {
  load(fs);
}

void Radio2::setPrimary(float freq, float bw, uint8_t sf, uint8_t cr) {
  primary.freq = freq;
  primary.bw = bw;
  primary.sf = sf;
  primary.cr = cr;
}

bool Radio2::start(bool rxps_enabled, bool extra_sf_enabled) {
  active = false;
  paused = false;
  pending_apply = false;
  if (!cfg.enabled) {
    conflict_state = RADIO2_CONFLICT_NONE;
    return false;
  }
  conflict_state = evaluate(rxps_enabled, extra_sf_enabled);
  if (conflict_state != RADIO2_CONFLICT_NONE) return false;

  active = true;
  current = RADIO_PROFILE_PRIMARY;
  phase = PHASE_PRIMARY;
  switch_failures = 0;
  last_primary_packet_ms = millis();  // start in active mode for the first 30 s
  scheduleFromNow();
  return true;
}

void Radio2::pauseForPrimaryChange() {
  paused = true;
  if (active && current != RADIO_PROFILE_PRIMARY) {
    switchTo(RADIO_PROFILE_PRIMARY, false);   // loop() retries if busy
  }
}

void Radio2::resumeAfterPrimaryChange(bool rxps_enabled, bool extra_sf_enabled) {
  paused = false;
  if (!cfg.enabled) {
    active = false;
    return;
  }
  conflict_state = evaluate(rxps_enabled, extra_sf_enabled);
  if (conflict_state != RADIO2_CONFLICT_NONE) {
    active = false;
    return;
  }
  if (!active) {
    active = true;
    current = RADIO_PROFILE_PRIMARY;
    phase = PHASE_PRIMARY;
    last_primary_packet_ms = millis();
  }
  switch_failures = 0;
  scheduleFromNow();
}

// ---------------------------------------------------------------- policy

static bool radio2Crossable(const mesh::Packet* pkt) {
  switch (pkt->getPayloadType()) {
    case PAYLOAD_TYPE_ADVERT:
      return true;
    case PAYLOAD_TYPE_GRP_TXT:
    case PAYLOAD_TYPE_GRP_DATA:
      return pkt->isRouteFlood();
    default:
      return false;
  }
}

Radio2Action Radio2::actionFor(uint8_t origin, const mesh::Packet* pkt) const {
  Radio2Action configured = origin == RADIO_PROFILE_SECONDARY
      ? (Radio2Action)cfg.policy_secondary : (Radio2Action)cfg.policy_primary;
  if (!radio2Crossable(pkt) && configured != RADIO2_ACTION_DROP) {
    return RADIO2_ACTION_SAME;
  }
  return configured;
}

uint8_t Radio2::targetFor(Radio2Action action, uint8_t origin) const {
  switch (action) {
    case RADIO2_ACTION_OTHER: return origin ^ 1;
    case RADIO2_ACTION_SAME:
    case RADIO2_ACTION_BOTH:
    case RADIO2_ACTION_DROP:
    default: return origin;
  }
}

uint8_t Radio2::replyProfile(uint8_t origin) const {
  return origin == RADIO_PROFILE_SECONDARY ? RADIO_PROFILE_SECONDARY
                                           : RADIO_PROFILE_PRIMARY;
}

// ---------------------------------------------------------------- timing

bool Radio2::timing(Radio2Timing* out) const {
  if (out == NULL) return false;
  memset(out, 0, sizeof(*out));
  if (!validTuple(primary.freq, primary.bw, primary.sf, primary.cr)) return false;
  if (!validTuple(secondary.freq, secondary.bw, secondary.sf, secondary.cr)) return false;

  float capture_symbols = device.captureCostSymbols();
  profileTiming(primary, capture_symbols, &out->primary_capture_us, &out->primary_bound_us);
  profileTiming(secondary, capture_symbols, &out->secondary_capture_us, &out->secondary_bound_us);

  out->usable = out->secondary_capture_us <= out->primary_bound_us &&
                out->primary_capture_us <= out->secondary_bound_us;
  out->active_mode = usesActiveMode();
  out->primary_visit_us = primaryVisitUs(*out);
  out->secondary_visit_us = secondaryVisitUs(*out);
  out->secondary_lossy = out->active_mode &&
                         out->primary_visit_us > out->secondary_bound_us;
  return out->usable;
}

bool Radio2::usesActiveMode() const {
  return (uint32_t)(millis() - last_primary_packet_ms) < RADIO2_ACTIVE_HOLD_MS;
}

uint32_t Radio2::primaryVisitUs(const Radio2Timing& t) const {
  if (!t.active_mode) return maxUs(t.primary_capture_us, RADIO2_MIN_VISIT_US);
  uint32_t sv = maxUs(t.secondary_capture_us, RADIO2_MIN_VISIT_US);
  return maxUs(t.primary_capture_us, RADIO2_ACTIVE_SECONDARY_DIV * sv);
}

uint32_t Radio2::secondaryVisitUs(const Radio2Timing& t) const {
  if (!t.active_mode) return t.primary_bound_us;   // the maximum safe share
  return maxUs(t.secondary_capture_us, RADIO2_MIN_VISIT_US);
}

// ---------------------------------------------------------------- scheduler

void Radio2::scheduleFromNow() {
  Radio2Timing t;
  if (!timing(&t)) return;
  phase = current == RADIO_PROFILE_SECONDARY ? PHASE_SECONDARY : PHASE_PRIMARY;
  phase_end_ms = millis() + usToMs(current == RADIO_PROFILE_SECONDARY
                                   ? secondaryVisitUs(t) : primaryVisitUs(t));
}

bool Radio2::switchTo(uint8_t profile, bool abort_receive, bool force) {
  if (profile == current && !force) return true;
  if (profile == RADIO_PROFILE_SECONDARY && !cfg.enabled) return false;
  // A complete packet is never abandoned: let core read it first.
  if (device.hasCompletedReceive()) return false;
  // A receive in progress is only abandoned when the caller explicitly asks.
  if (!abort_receive && device.isReceiveInProgress()) return false;

  const Radio2Profile& p = profile == RADIO_PROFILE_PRIMARY ? primary : secondary;
  if (!device.retune(p.freq, p.bw, p.sf, p.cr, abort_receive)) {
    switch_failures++;
    switch_errors++;
    if (switch_failures >= RADIO2_MAX_SWITCH_FAILURES) {
      active = false;   // stay primary; a config change re-enables attempts
      conflict_state = RADIO2_CONFLICT_SWITCH;
    }
    return false;
  }
  switch_failures = 0;
  conflict_state = RADIO2_CONFLICT_NONE;
  current = profile;
  switches++;
  return true;
}

bool Radio2::selectTxProfile(uint8_t profile) {
  // Off (or disabled after failures): only the primary radio is valid. A
  // secondary-target packet falls back to it; while a retune back is pending,
  // wait so nothing is ever transmitted on the wrong preset.
  if (!active) return current == RADIO_PROFILE_PRIMARY;
  if (current == profile) return true;
  if (device.hasCompletedReceive() || device.isReceiveInProgress()) return false;
  if (!switchTo(profile, false)) return current == profile;   // busy or failed: retry
  scheduleFromNow();
  return true;
}

void Radio2::loop(FILESYSTEM* fs, bool quiet) {
  if (save_flag.due()) save(fs);

  uint32_t now = millis();

  if (pending_apply && quiet && (int32_t)(now - pending_apply_at_ms) >= 0) {
    if (applyPending()) pending_apply = false;
  }

  if (paused) {
    if (current != RADIO_PROFILE_PRIMARY) switchTo(RADIO_PROFILE_PRIMARY, false);
    return;
  }
  if (!active) {
    // A disabled scanner still has to get the radio back to primary, unless a
    // deferred apply is deliberately waiting for the command reply to go out.
    if (!pending_apply && current != RADIO_PROFILE_PRIMARY) {
      switchTo(RADIO_PROFILE_PRIMARY, false);
    }
    return;
  }
  if (!device.isInRecvMode()) return;        // TX (or idle): never abort it
  if (device.hasCompletedReceive()) return;  // let core read the packet
  if ((int32_t)(now - phase_end_ms) < 0) return;

  Radio2Timing t;
  if (!timing(&t)) {
    active = false;
    conflict_state = RADIO2_CONFLICT_TIMING;
    return;
  }

  if (phase == PHASE_PRIMARY) {
    if (device.isReceiveInProgress()) {   // primary wins
      phase_end_ms = now + 1;
      return;
    }
    if (switchTo(RADIO_PROFILE_SECONDARY, false)) {
      phase = PHASE_SECONDARY;
      phase_end_ms = now + usToMs(secondaryVisitUs(t));
    } else if (active) {
      phase_end_ms = now + RADIO2_SWITCH_RETRY_MS;
    }
    return;
  }

  // PHASE_SECONDARY
  if (device.isReceiveInProgress()) {
    if (t.active_mode) {
      // Primary is busy: abandon the secondary receive rather than miss it.
      if (switchTo(RADIO_PROFILE_PRIMARY, true)) {
        noteAbort();
        phase = PHASE_PRIMARY;
        phase_end_ms = now + usToMs(primaryVisitUs(t));
      } else if (active) {
        phase_end_ms = now + RADIO2_SWITCH_RETRY_MS;
      }
    } else {
      // Primary is idle: let the long secondary packet finish.
      phase_end_ms = now + RADIO2_HOLD_POLL_MS;
    }
    return;
  }
  if (switchTo(RADIO_PROFILE_PRIMARY, false)) {
    phase = PHASE_PRIMARY;
    phase_end_ms = now + usToMs(primaryVisitUs(t));
  } else if (active) {
    phase_end_ms = now + RADIO2_SWITCH_RETRY_MS;
  }
}

bool Radio2::applyPending() {
  if (!cfg.enabled) {
    active = false;
    conflict_state = RADIO2_CONFLICT_NONE;
    return true;   // the !active branch retunes to primary when quiet
  }
  Radio2Timing t;
  if (!timing(&t)) {
    conflict_state = RADIO2_CONFLICT_TIMING;
    active = false;
    return true;
  }
  conflict_state = RADIO2_CONFLICT_NONE;
  bool was_active = active;
  active = true;
  switch_failures = 0;
  if (current == RADIO_PROFILE_SECONDARY &&
      !switchTo(RADIO_PROFILE_SECONDARY, false, true)) {
    active = was_active;
    return false;   // busy: retry on a later loop pass
  }
  last_primary_packet_ms = millis();
  scheduleFromNow();
  return true;
}

uint32_t Radio2::getSleepMs() {
  if (!active || paused || pending_apply) return 0;
  if (!device.isInRecvMode() || device.hasCompletedReceive() ||
      device.isReceiveInProgress()) {
    return 1;   // wake soon; never sleep through a packet
  }
  int32_t delta = (int32_t)(phase_end_ms - millis());
  if (delta <= 0) return 1;
  return (uint32_t)delta < RADIO2_SLEEP_MAX_MS ? (uint32_t)delta : RADIO2_SLEEP_MAX_MS;
}

// ---------------------------------------------------------------- counters

void Radio2::notePrimaryPacket() {
  last_primary_packet_ms = millis();
}

void Radio2::noteRx(uint8_t profile) {
  if (profile == RADIO_PROFILE_SECONDARY) rx_secondary++;
  else rx_primary++;
}

void Radio2::noteTx(uint8_t profile) {
  if (profile == RADIO_PROFILE_SECONDARY) tx_secondary++;
  else tx_primary++;
}

// ---------------------------------------------------------------- config

Radio2Conflict Radio2::evaluate(bool rxps_enabled, bool extra_sf_enabled) const {
  if (rxps_enabled) return RADIO2_CONFLICT_RXPS;
  if (extra_sf_enabled) return RADIO2_CONFLICT_EXTRA_SF;
  if (!validTuple(secondary.freq, secondary.bw, secondary.sf, secondary.cr)) {
    return RADIO2_CONFLICT_PRESET;
  }
  if (overlaps(primary, secondary)) return RADIO2_CONFLICT_PRESET;
  Radio2Timing t;
  return timing(&t) ? RADIO2_CONFLICT_NONE : RADIO2_CONFLICT_TIMING;
}

Radio2SetResult Radio2::setPreset(float freq, float bw, uint8_t sf, uint8_t cr) {
  Radio2Profile candidate = { freq, bw, sf, cr };
  if (!validTuple(freq, bw, sf, cr)) return RADIO2_SET_BAD_PRESET;
  if (overlaps(primary, candidate)) return RADIO2_SET_OVERLAP;

  // Validate the pair off to the side: a refused preset leaves the live one
  // byte-for-byte unchanged.
  Radio2Profile saved = secondary;
  secondary = candidate;
  Radio2Timing t;
  bool usable = timing(&t);
  secondary = saved;
  if (!usable) return RADIO2_SET_UNSCANNABLE;

  secondary = candidate;
  cfg.enabled = 1;
  cfg.freq_mhz = freq;
  cfg.bw_khz = bw;
  cfg.sf = sf;
  cfg.cr = cr;
  switch_failures = 0;
  pending_apply = true;
  pending_apply_at_ms = millis() + RADIO2_APPLY_DELAY_MS;
  markDirty();
  return RADIO2_SET_OK;
}

void Radio2::setEnabled(bool on) {
  cfg.enabled = on ? 1 : 0;
  switch_failures = 0;
  pending_apply = true;
  pending_apply_at_ms = millis() + RADIO2_APPLY_DELAY_MS;
  markDirty();
}

void Radio2::setPolicy(uint8_t primary, uint8_t secondary) {
  uint8_t pa = primary <= RADIO2_ACTION_DROP ? primary : RADIO2_ACTION_SAME;
  uint8_t sa = secondary <= RADIO2_ACTION_DROP ? secondary : RADIO2_ACTION_SAME;
  if (cfg.policy_primary == pa && cfg.policy_secondary == sa) return;
  cfg.policy_primary = pa;
  cfg.policy_secondary = sa;
  markDirty();
}

void Radio2::setAdvert(uint8_t advert) {
  uint8_t a = advert == RADIO2_ADVERT_BOTH ? RADIO2_ADVERT_BOTH : RADIO2_ADVERT_PRIMARY;
  if (cfg.advert == a) return;
  cfg.advert = a;
  markDirty();
}

// ---------------------------------------------------------------- persistence

static bool radio2ValidateRecord(const Radio2CfgRecord& rec) {
  if (memcmp(rec.magic, RADIO2_CFG_MAGIC, 4) != 0) return false;
  if (rec.version != RADIO2_CFG_VERSION) return false;
  if (rec.enabled > 1) return false;
  if (rec.policy_primary > RADIO2_ACTION_DROP ||
      rec.policy_secondary > RADIO2_ACTION_DROP) return false;
  if (rec.advert > RADIO2_ADVERT_BOTH) return false;

  bool zero = rec.freq_mhz == 0.0f && rec.bw_khz == 0.0f && rec.sf == 0 && rec.cr == 0;
  if (rec.enabled) {
    return validTuple(rec.freq_mhz, rec.bw_khz, rec.sf, rec.cr);
  }
  return zero || validTuple(rec.freq_mhz, rec.bw_khz, rec.sf, rec.cr);
}

static bool radio2WriteFile(File& f, void* ctx) {
  Radio2& r = *(Radio2*)ctx;
  Radio2CfgRecord rec;
  memset(&rec, 0, sizeof(rec));
  memcpy(rec.magic, RADIO2_CFG_MAGIC, 4);
  rec.version = RADIO2_CFG_VERSION;
  rec.enabled = r.isEnabled() ? 1 : 0;
  rec.policy_primary = r.getPolicyPrimary();
  rec.policy_secondary = r.getPolicySecondary();
  rec.advert = r.getAdvert();
  const Radio2Profile& p = r.getSecondaryProfile();
  rec.freq_mhz = p.freq;
  rec.bw_khz = p.bw;
  rec.sf = p.sf;
  rec.cr = p.cr;

  uint8_t buf[RADIO2_CFG_RECORD_BYTES];
  memcpy(buf, &rec, RADIO2_CFG_PAYLOAD_BYTES);
  uint16_t crc = crc16_ccitt(buf, RADIO2_CFG_PAYLOAD_BYTES);
  memcpy(&buf[RADIO2_CFG_PAYLOAD_BYTES], &crc, 2);
  return f.write(buf, RADIO2_CFG_RECORD_BYTES) == RADIO2_CFG_RECORD_BYTES;
}

static bool radio2ValidateFile(File& f, void* ctx) {
  (void)ctx;
  uint8_t buf[RADIO2_CFG_RECORD_BYTES];
  if (f.read(buf, RADIO2_CFG_RECORD_BYTES) != RADIO2_CFG_RECORD_BYTES) return false;
  uint16_t stored, computed;
  memcpy(&stored, &buf[RADIO2_CFG_PAYLOAD_BYTES], 2);
  computed = crc16_ccitt(buf, RADIO2_CFG_PAYLOAD_BYTES);
  if (stored != computed) return false;
  Radio2CfgRecord rec;
  memcpy(&rec, buf, sizeof(rec));
  return radio2ValidateRecord(rec);
}

void Radio2::save(FILESYSTEM* fs) {
  bool ok = saveStaged(fs, RADIO2_CFG_FILE, radio2WriteFile, this,
                       radio2ValidateFile, NULL, radio2ValidateFile, NULL);
  if (ok) save_flag.clear();
  else save_flag.retryLater();
}

void Radio2::load(FILESYSTEM* fs) {
  resetToDefaults();
  save_flag.reset();

  char path[64];
  PersistLoadSource src = chooseConfigToLoad(fs, RADIO2_CFG_FILE, radio2ValidateFile, NULL,
                                             path, sizeof(path));
  if (src == PERSIST_LOAD_NONE) return;
  File file = fsOpenRead(fs, path);
  if (!file) return;

  uint8_t buf[RADIO2_CFG_RECORD_BYTES];
  if (file.read(buf, RADIO2_CFG_RECORD_BYTES) != RADIO2_CFG_RECORD_BYTES) {
    file.close();
    return;
  }
  file.close();

  uint16_t stored, computed;
  memcpy(&stored, &buf[RADIO2_CFG_PAYLOAD_BYTES], 2);
  computed = crc16_ccitt(buf, RADIO2_CFG_PAYLOAD_BYTES);
  if (stored != computed) return;

  Radio2CfgRecord rec;
  memcpy(&rec, buf, sizeof(rec));
  if (!radio2ValidateRecord(rec)) return;

  cfg = rec;
  secondary.freq = rec.freq_mhz;
  secondary.bw = rec.bw_khz;
  secondary.sf = rec.sf;
  secondary.cr = rec.cr;

  if (src == PERSIST_LOAD_RECOVERED) markDirty();
}

// ---------------------------------------------------------------- CLI

#define RADIO2_USAGE \
  "Err - usage: status|timing|<freq>,<bw>,<sf>,<cr>|off|policy [a] [b]|advert primary|both"

static void radio2Status(Radio2& r, char* reply) {
  if (!r.isEnabled()) {
    snprintf(reply, CLI_REPLY_MAX, "off");
    return;
  }
  if (!r.isScanning()) {
    if (r.hasPendingApply()) {
      snprintf(reply, CLI_REPLY_MAX, "on; apply=pending; at=%s",
               r.currentProfile() == RADIO_PROFILE_SECONDARY ? "radio2" : "primary");
    } else if (r.isPaused()) {
      snprintf(reply, CLI_REPLY_MAX, "off; paused");
    } else {
      snprintf(reply, CLI_REPLY_MAX, "off; conflict=%s", conflictName(r.conflict()));
    }
    return;
  }

  Radio2Timing t;
  bool usable = r.timing(&t);
  unsigned pct = 50;
  if (usable) {
    uint32_t total = t.primary_visit_us + t.secondary_visit_us;
    if (total > 0) pct = (unsigned)((uint64_t)t.primary_visit_us * 100 / total);
  }

  char line[256];
  CliBuf buf(line, sizeof(line));
  buf.add("on");
  if (r.hasPendingApply()) buf.add("; apply=pending");
  buf.add("; at=%s", r.currentProfile() == RADIO_PROFILE_SECONDARY ? "radio2" : "primary");
  buf.add("; mode=%s", t.active_mode ? "active" : "idle");
  if (t.secondary_lossy) buf.add("; lossy");
  buf.add("; split=%u/%u", pct, 100 - pct);
  buf.add("; nf=%d", r.getNoiseFloor());
  buf.add("; sw=%lu; swf=%lu", (unsigned long)r.getSwitchCount(),
          (unsigned long)r.getSwitchErrors());
  buf.add("; rx=%lu/%lu", (unsigned long)r.getRxPrimary(),
          (unsigned long)r.getRxSecondary());
  buf.add("; tx=%lu/%lu", (unsigned long)r.getTxPrimary(),
          (unsigned long)r.getTxSecondary());
  buf.add("; both=%lu; bdrop=%lu; drop=%lu; abrt=%lu", (unsigned long)r.getBoth(),
          (unsigned long)r.getBothDrop(), (unsigned long)r.getPolicyDrop(),
          (unsigned long)r.getAborts());
  cliCutReply(reply, CLI_REPLY_MAX, line);
}

static void radio2TimingReply(Radio2& r, char* reply) {
  Radio2Timing t;
  if (!r.timing(&t)) {
    snprintf(reply, CLI_REPLY_MAX,
             "unusable: secondary capture %lums > primary-safe %lums",
             (unsigned long)(t.secondary_capture_us / 1000),
             (unsigned long)(t.primary_bound_us / 1000));
    return;
  }
  uint32_t total = t.primary_visit_us + t.secondary_visit_us;
  unsigned pct = total > 0
      ? (unsigned)((uint64_t)t.primary_visit_us * 100 / total) : 50;
  snprintf(reply, CLI_REPLY_MAX,
           "primary=%lums; secondary=%lums; split=%u/%u; switch=%lums; mode=%s",
           (unsigned long)(t.primary_visit_us / 1000),
           (unsigned long)(t.secondary_visit_us / 1000), pct, 100 - pct,
           (unsigned long)(RADIO2_SWITCH_US / 1000),
           t.active_mode ? "active" : "idle");
}

static bool parseFloatToken(const char* tok, float* out) {
  if (tok == NULL || tok[0] == 0) return false;
  errno = 0;
  char* end;
  float v = strtof(tok, &end);
  if (end == tok || *end != 0 || errno == ERANGE || !isfinite(v)) return false;
  *out = v;
  return true;
}

static void radio2SetPreset(Radio2& r, const char* tuple, bool rxps_enabled,
                            bool extra_sf_enabled, char* reply) {
  char scratch[48];
  if (!cliCopyCommand(scratch, sizeof(scratch), tuple)) {
    snprintf(reply, CLI_REPLY_MAX, "Err - radio2 preset must be <freq>,<bw>,<sf>,<cr>");
    return;
  }
  const char* parts[4];
  int n = mesh::Utils::parseTextParts(scratch, parts, 4);
  if (n != 4) {
    snprintf(reply, CLI_REPLY_MAX, "Err - radio2 preset must be <freq>,<bw>,<sf>,<cr>");
    return;
  }

  float freq, bw;
  long sf, cr;
  if (!parseFloatToken(parts[0], &freq) || !parseFloatToken(parts[1], &bw) ||
      !parseIntRange(parts[2], 5, 12, &sf) || !parseIntRange(parts[3], 5, 8, &cr)) {
    snprintf(reply, CLI_REPLY_MAX, "Err - radio2 preset must be <freq>,<bw>,<sf>,<cr>");
    return;
  }
  if (rxps_enabled) {
    snprintf(reply, CLI_REPLY_MAX, "Err - radio.rxps is on; set radio.rxps off first");
    return;
  }
  if (extra_sf_enabled) {
    snprintf(reply, CLI_REPLY_MAX, "Err - radio2 conflicts with extra.sf; clear extra SF first");
    return;
  }

  switch (r.setPreset(freq, bw, (uint8_t)sf, (uint8_t)cr)) {
    case RADIO2_SET_OK: {
      const Radio2Profile& p = r.getSecondaryProfile();
      snprintf(reply, CLI_REPLY_MAX, "OK - radio2 %g,%g,%u,%u",
               p.freq, p.bw, (unsigned)p.sf, (unsigned)p.cr);
      break;
    }
    case RADIO2_SET_OVERLAP:
      snprintf(reply, CLI_REPLY_MAX, "Err - radio2 overlaps radio");
      break;
    case RADIO2_SET_UNSCANNABLE:
      snprintf(reply, CLI_REPLY_MAX,
               "Err - radio2 preset cannot be scanned without degrading radio");
      break;
    default:
      snprintf(reply, CLI_REPLY_MAX,
               "Err - radio2 preset must be <freq>,<bw>,<sf>,<cr>");
      break;
  }
}

void radio2CLI(Radio2& r, bool rxps_enabled, bool extra_sf_enabled,
               const char* command, char* reply) {
  char scratch[128];
  if (!cliPrepare(command, scratch, sizeof(scratch), reply)) return;

  char* p = scratch;
  char* sub = nextToken(&p);
  if (sub == NULL || strcmp(sub, "status") == 0) {
    if (!cliNoExtra(p, reply, RADIO2_USAGE)) return;
    radio2Status(r, reply);
    return;
  }
  if (strcmp(sub, "timing") == 0) {
    if (!cliNoExtra(p, reply, RADIO2_USAGE)) return;
    radio2TimingReply(r, reply);
    return;
  }
  if (strcmp(sub, "off") == 0) {
    if (!cliNoExtra(p, reply, RADIO2_USAGE)) return;
    r.setEnabled(false);
    snprintf(reply, CLI_REPLY_MAX, "OK - radio2 off");
    return;
  }
  if (strcmp(sub, "policy") == 0) {
    char* a = nextToken(&p);
    if (a == NULL) {
      snprintf(reply, CLI_REPLY_MAX, "> primary=%s,secondary=%s",
               actionName(r.getPolicyPrimary()), actionName(r.getPolicySecondary()));
      return;
    }
    char* b = nextToken(&p);
    uint8_t pa, sa;
    if (!actionFromName(a, &pa) || (b != NULL && !actionFromName(b, &sa))) {
      snprintf(reply, CLI_REPLY_MAX, "Err - radio2 policy must be same|other|both|drop");
      return;
    }
    if (b == NULL) sa = pa;
    if (!cliNoExtra(p, reply, RADIO2_USAGE)) return;
    r.setPolicy(pa, sa);
    snprintf(reply, CLI_REPLY_MAX, "OK - radio2 policy primary=%s,secondary=%s",
             actionName(pa), actionName(sa));
    return;
  }
  if (strcmp(sub, "advert") == 0) {
    char* a = nextToken(&p);
    if (a == NULL) {
      snprintf(reply, CLI_REPLY_MAX, "> %s",
               r.getAdvert() == RADIO2_ADVERT_BOTH ? "both" : "primary");
      return;
    }
    if (strcmp(a, "primary") != 0 && strcmp(a, "both") != 0) {
      snprintf(reply, CLI_REPLY_MAX, "Err - radio2 advert must be primary|both");
      return;
    }
    if (!cliNoExtra(p, reply, RADIO2_USAGE)) return;
    r.setAdvert(strcmp(a, "both") == 0 ? RADIO2_ADVERT_BOTH : RADIO2_ADVERT_PRIMARY);
    snprintf(reply, CLI_REPLY_MAX, "OK - radio2 advert %s", a);
    return;
  }

  // Anything else is a preset tuple. nextToken() only split on spaces, so the
  // token still holds the whole comma list.
  radio2SetPreset(r, sub, rxps_enabled, extra_sf_enabled, reply);
}

