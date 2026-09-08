// BatteryGate.h — suspend repeating below a configurable battery voltage, for
// the simple_repeater firmware. Personal fork feature; all fork-owned code
// lives in this file pair plus minimal hook lines in MyMesh.h/MyMesh.cpp
// (same footprint as the packet filter). No changes to MeshCore core sources.
//
// The node samples its battery every 30 s from MyMesh::loop() and, using a
// lower/upper threshold pair (hysteresis) plus a two-reading debounce,
// suspends all packet forwarding while the battery is low. The node itself
// stays fully reachable: CLI replies and status responses are originated,
// never forwarded, and the node's own adverts continue.
//
// The suspended flag is RAM-only — recomputed from the live voltage after
// every boot — so a reboot mid-suspend can never leave the repeater off after
// the battery recovers. Only the thresholds (and enabled flag) persist, in
// /batt_cfg, using the same lazy-dirty-save pattern FilterRules uses.
//
// Manual "set off" (_prefs.disable_fwd) is orthogonal and unaffected: the
// gate only controls its own suspension.
//
// CLI: "battery ..." commands, handled by batteryCLI() (see BatteryGate.cpp).

#ifndef _BATTERY_GATE_H
#define _BATTERY_GATE_H

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/IdentityStore.h>   // FILESYSTEM typedef

// accepted threshold range, in millivolts
#define BATT_MV_MIN  1
#define BATT_MV_MAX  10000

class BatteryGate {
  bool enabled;
  bool suspended;             // RAM-only; never persisted
  uint16_t suspend_mV;        // suspend when voltage falls below this
  uint16_t resume_mV;         // resume when voltage rises to this (hysteresis)
  uint8_t low_count;          // consecutive low readings (debounce)
  uint32_t drops;             // packets gated while suspended (RAM-only)
  unsigned long next_sample_at;
  bool dirty;                 // needs save
  unsigned long dirty_since;

public:
  BatteryGate();

  void begin(FILESYSTEM* fs);                      // load persisted config
  void loop(FILESYSTEM* fs, mesh::MainBoard& board);  // periodic sample + lazy save

  bool isEnabled() const { return enabled; }
  bool isSuspended() const { return suspended; }
  uint16_t getSuspendMilliVolts() const { return suspend_mV; }
  uint16_t getResumeMilliVolts() const { return resume_mV; }
  uint32_t getDropCount() const { return drops; }

  // Packet-path hook: returns false when forwarding is suspended, counting the
  // gated packet. Reads the cached flag only — no ADC access on the packet path.
  bool checkForward() {
    if (suspended) { drops++; return false; }
    return true;
  }

  // gate on/off; disabling also lifts any current suspension
  void setEnabled(bool on);

  // set thresholds and enable the gate; false if invalid (resume must be
  // strictly greater than suspend). Applies on the next sample, which is
  // taken immediately on the following loop().
  bool setThresholds(uint16_t suspend, uint16_t resume);

  // take one reading and apply debounce + hysteresis
  void sample(mesh::MainBoard& board);

  void markDirty() { dirty = true; dirty_since = millis(); }

  // persistence: thresholds and enabled flag only (never the suspended state)
  void load(FILESYSTEM* fs);
  void save(FILESYSTEM* fs);
};

// CLI command handler: invoke with the command after "battery" (prefix
// removed). Replies must fit the 160-byte CLI reply buffer.
void batteryCLI(BatteryGate& gate, mesh::MainBoard& board, const char* command, char* reply);

#endif // _BATTERY_GATE_H
