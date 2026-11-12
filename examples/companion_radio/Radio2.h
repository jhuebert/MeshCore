// Radio2.h — the companion's second LoRa preset: config, policy, scheduler
// and CLI. Fork-owned; see CUSTOM_COMPANION.md (Radio2 second preset) for the
// user-facing model.
//
// The feature is host-testable: Radio2Device is the only radio surface it
// touches, so the policy and scheduler run against a fake in the native tests.

#ifndef _RADIO2_H
#define _RADIO2_H

#include <Arduino.h>
#include <stdint.h>

#include <Packet.h>
#include <helpers/radiolib/RadioProfileControl.h>

#include "PersistUtil.h"
#include "Radio2Config.h"

enum Radio2Action : uint8_t {
  RADIO2_ACTION_SAME = 0,
  RADIO2_ACTION_OTHER = 1,
  RADIO2_ACTION_BOTH = 2,
  RADIO2_ACTION_DROP = 3,
};

enum Radio2Advert : uint8_t {
  RADIO2_ADVERT_PRIMARY = 0,
  RADIO2_ADVERT_BOTH = 1,
};

enum Radio2Conflict : uint8_t {
  RADIO2_CONFLICT_NONE = 0,
  RADIO2_CONFLICT_RXPS,
  RADIO2_CONFLICT_EXTRA_SF,
  RADIO2_CONFLICT_PRESET,     // overlap / bad tuple
  RADIO2_CONFLICT_TIMING,     // unsafe to scan this pair
  RADIO2_CONFLICT_SWITCH,     // too many retune failures
};

enum Radio2SetResult : uint8_t {
  RADIO2_SET_OK = 0,
  RADIO2_SET_BAD_PRESET,
  RADIO2_SET_OVERLAP,
  RADIO2_SET_UNSCANNABLE,
};

struct Radio2Profile {
  float freq;
  float bw;
  uint8_t sf;
  uint8_t cr;
};

struct Radio2Timing {
  bool usable;
  bool active_mode;
  bool secondary_lossy;
  uint32_t primary_capture_us;
  uint32_t secondary_capture_us;
  uint32_t primary_bound_us;
  uint32_t secondary_bound_us;
  uint32_t primary_visit_us;
  uint32_t secondary_visit_us;
  // Longest packet the secondary preset can carry. Once a secondary receive
  // has started, the scheduler lets it finish for at most this long, so one
  // packet (not an arbitrary stuck receive) can hold the primary.
  uint32_t secondary_max_airtime_us;
};

// The radio surface Radio2 needs. MyMesh supplies a thin adapter over
// radio_driver; the native tests supply a fake.
class Radio2Device {
public:
  virtual ~Radio2Device() = default;
  virtual bool retune(float freq, float bw, uint8_t sf, uint8_t cr,
                      bool abort_receive = false) = 0;
  virtual bool hasCompletedReceive() const = 0;
  virtual bool isReceiveInProgress() = 0;
  virtual bool isInRecvMode() const = 0;
  virtual int getNoiseFloor() const = 0;
  virtual float captureCostSymbols() const = 0;
};

class Radio2 : public RadioProfileControl {
public:
  explicit Radio2(Radio2Device& device);

  // RadioProfileControl: the dispatcher asks before every transmission.
  bool selectTxProfile(uint8_t profile) override;

  void begin(FILESYSTEM* fs);           // load persisted config
  void loop(FILESYSTEM* fs, bool quiet);  // scheduler + lazy save
  // Evaluate the current config against the environment and start (or not).
  // Returns true when scanning is active.
  bool start(bool rxps_enabled, bool extra_sf_enabled);
  void setPrimary(float freq, float bw, uint8_t sf, uint8_t cr);
  // tempradio: stop scanning while a temporary primary is active, revalidate
  // once it reverts.
  void pauseForPrimaryChange();
  void resumeAfterPrimaryChange(bool rxps_enabled, bool extra_sf_enabled);

  bool isEnabled() const { return cfg.enabled != 0; }
  // True only while the scanner is actually time-sharing. A paused scanner
  // (temporary primary change) reports false, matching the `off; paused` status
  // and keeping policy/counters off the radio until it resumes.
  bool isScanning() const { return active && !paused; }
  bool isPaused() const { return paused; }
  bool hasPendingApply() const { return pending_apply; }
  // True only while the scheduler/save machinery has something to do. When
  // radio2 is off and settled this is false, so MyMesh skips radio2.loop().
  bool needsService() const {
    return active || paused || pending_apply || current != RADIO_PROFILE_PRIMARY ||
           save_flag.isDirty();
  }
  uint8_t currentProfile() const { return current; }
  Radio2Conflict conflict() const { return conflict_state; }

  // policy
  Radio2Action actionFor(uint8_t origin, const mesh::Packet* pkt) const;
  uint8_t targetFor(Radio2Action action, uint8_t origin) const;
  uint8_t replyProfile(uint8_t origin) const;
  bool advertBoth() const { return cfg.advert == RADIO2_ADVERT_BOTH && active && !paused; }
  uint8_t getPolicyPrimary() const { return cfg.policy_primary; }
  uint8_t getPolicySecondary() const { return cfg.policy_secondary; }
  uint8_t getAdvert() const { return cfg.advert; }
  // Local TX preset for traffic the user originates (replies to a received
  // packet always follow its origin). Persisted in /radio2_cfg.
  uint8_t localTxProfile() const { return cfg.tx_local; }
  void setLocalTxProfile(uint8_t p);

  // CLI mutations. The preset/enable changes persist immediately but defer
  // the RF apply so a remote admin's reply goes out on the old config.
  Radio2SetResult setPreset(float freq, float bw, uint8_t sf, uint8_t cr);
  void setEnabled(bool on);
  void setPolicy(uint8_t primary, uint8_t secondary);
  void setAdvert(uint8_t advert);

  const Radio2Profile& getSecondaryProfile() const { return secondary; }

  // timing/status
  bool timing(Radio2Timing* out) const;
  uint32_t getSleepMs();

  // counters
  void notePrimaryPacket();
  void noteRx(uint8_t profile);
  void noteTx(uint8_t profile);
  void noteBoth() { both++; }
  void noteBothDrop() { both_drop++; }
  void notePolicyDrop() { policy_drop++; }
  void noteAbort() { aborts++; }

  // status read-outs
  int getNoiseFloor() const { return device.getNoiseFloor(); }
  uint32_t getSwitchCount() const { return switches; }
  uint32_t getSwitchErrors() const { return switch_errors; }
  uint32_t getRxPrimary() const { return rx_primary; }
  uint32_t getRxSecondary() const { return rx_secondary; }
  uint32_t getTxPrimary() const { return tx_primary; }
  uint32_t getTxSecondary() const { return tx_secondary; }
  uint32_t getBoth() const { return both; }
  uint32_t getBothDrop() const { return both_drop; }
  uint32_t getPolicyDrop() const { return policy_drop; }
  uint32_t getAborts() const { return aborts; }

  // persistence
  void save(FILESYSTEM* fs);
  void load(FILESYSTEM* fs);
  void markDirty() { save_flag.markDirty(); }

private:
  enum Phase : uint8_t { PHASE_PRIMARY, PHASE_SECONDARY };

  Radio2Device& device;
  Radio2CfgRecord cfg;
  Radio2Profile primary;
  Radio2Profile secondary;

  bool active = false;
  bool paused = false;
  bool pending_apply = false;
  uint32_t pending_apply_at_ms = 0;
  Radio2Conflict conflict_state = RADIO2_CONFLICT_NONE;

  Phase phase = PHASE_PRIMARY;
  uint8_t current = RADIO_PROFILE_PRIMARY;
  uint32_t phase_end_ms = 0;
  uint32_t secondary_rx_deadline_ms = 0;   // bound on one secondary receive
  uint32_t last_primary_packet_ms = 0;
  uint8_t switch_failures = 0;

  uint32_t rx_primary = 0, rx_secondary = 0;
  uint32_t tx_primary = 0, tx_secondary = 0;
  uint32_t both = 0, both_drop = 0, policy_drop = 0, aborts = 0;
  uint32_t switches = 0, switch_errors = 0;

  LazySave save_flag;

  void resetToDefaults();
  bool applyPending();
  bool switchTo(uint8_t profile, bool abort_receive, bool force = false);
  bool usesActiveMode() const;
  uint32_t primaryVisitUs(const Radio2Timing& t) const;
  uint32_t secondaryVisitUs(const Radio2Timing& t) const;
  void scheduleFromNow();
  Radio2Conflict evaluate(bool rxps_enabled, bool extra_sf_enabled) const;
};

// `command` is everything after the "radio2" verb.
void radio2CLI(Radio2& radio2, bool rxps_enabled, bool extra_sf_enabled,
               const char* command, char* reply);

#endif  // _RADIO2_H
