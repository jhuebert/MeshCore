#pragma once

#include <stdint.h>

// PowerProfile.h — the fork's power settings that are not radio-driver code:
// (see POWER.md, and AGENTS.md for the upstream touchpoints of the RXPS layer)
//   * the CPU clock cap (`cpu.freq`), applied at boot and on change;
//   * the repeater's power defaults (RX power saving on, balanced level);
//   * the hook that pushes the stored RXPS config into the radio.
//
// Kept out of CommonCLI/MyMesh so those upstream files only carry the call
// sites, and kept out of the radio driver files so the duty-cycle state
// machine lives in one place (RXPowerSavingRuntime).

class NodePrefs;

// Apply the persisted CPU frequency. Called from loadPrefs() at boot (after the
// prefs are known) and from `set cpu.freq`.
void powerApplyCpuFrequency(const NodePrefs& prefs);

// The repeater's power defaults, written into a freshly constructed prefs
// object before the stored prefs are loaded over it.
void powerApplyDefaults(NodePrefs& prefs);

// Push the stored RXPS config into the radio for the given (or stored) radio
// parameters. Called at boot and after a temporary radio parameter change.
void powerApplyRadioProfile(NodePrefs& prefs);
void powerApplyRadioProfile(NodePrefs& prefs, uint8_t sf, float bw);

// `get cpu.freq` / `set cpu.freq <mhz>`; true when the command was one of ours.
bool powerCliGet(const char* config, const NodePrefs& prefs, char* reply);
bool powerCliSet(const char* config, NodePrefs& prefs, char* reply, bool* changed);
