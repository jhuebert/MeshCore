#include "PowerProfile.h"

#include <Arduino.h>

#include "CommonCLI.h"
#include "MeshCore.h"

void powerApplyCpuFrequency(const NodePrefs& prefs) {
#if defined(ESP32)
  // Captured on the first call (boot, before any change) so "default" can
  // restore the frequency the board started with.
  static uint16_t boot_mhz = getCpuFrequencyMhz();
  uint16_t mhz = prefs.cpu_freq_mhz ? prefs.cpu_freq_mhz : boot_mhz;
  if (mhz == getCpuFrequencyMhz()) return;   // nothing to change: no clock call,
                                             // and the UART is left alone
  if (!setCpuFrequencyMhz(mhz)) {
    MESH_DEBUG_PRINTLN("CPU frequency %u MHz was not applied", (unsigned)mhz);
  }
#if !defined(ARDUINO_USB_CDC_ON_BOOT) || !ARDUINO_USB_CDC_ON_BOOT
  // A UART console derives its baud divider from the APB clock; re-init it so
  // the console survives the clock change. USB CDC consoles are unaffected.
  Serial.begin(115200);
#endif
#endif
}

void powerApplyDefaults(NodePrefs& prefs) {
  // Pre-power-feature behaviour: continuous RX, no CPU cap. The level/preamble
  // defaults are carried so `set radio.rxps on` has something sane to start
  // from, but nothing is armed until the operator asks for it. "No CPU cap"
  // means the board's own clock (ESP32_CPU_FREQ where a variant sets it), not
  // an assumed 240 MHz.
  prefs.rxps.enabled = 0;
  prefs.rxps.level = RX_POWERSAVING_BALANCED_LEVEL;
  prefs.rxps.preamble = RX_POWERSAVING_PROFILE_PREAMBLE;
  prefs.cpu_freq_mhz = 0;
}

void powerApplyRadioProfile(NodePrefs& prefs) {
  powerApplyRadioProfile(prefs, prefs.sf, prefs.bw);
}

void powerApplyRadioProfile(NodePrefs& prefs, uint8_t sf, float bw) {
  prefs.getRadioPrefs()->applyRxPowerSaving(sf, bw);
}

bool powerCliGet(const char* config, const NodePrefs& prefs, char* reply) {
  if (memcmp(config, "cpu.freq", 8) != 0 || (config[8] != 0 && config[8] != ' ')) return false;
  if (prefs.cpu_freq_mhz)
    snprintf(reply, 160, "> %u", (unsigned)prefs.cpu_freq_mhz);
  else
    strcpy(reply, "> default");
  return true;
}

bool powerCliSet(const char* config, NodePrefs& prefs, char* reply, bool* changed) {
  if (memcmp(config, "cpu.freq ", 9) != 0) return false;
  *changed = false;
#if defined(ESP32)
  const char* value = &config[9];
  char* end = nullptr;
  long mhz = strtol(value, &end, 10);
  if (*value == 0 || end == nullptr || *end != 0 ||
      !(mhz == 0 || mhz == 40 || mhz == 80 || mhz == 160 || mhz == 240)) {
    strcpy(reply, "Err - cpu.freq must be 0, 40, 80, 160 or 240");
    return true;
  }
  prefs.cpu_freq_mhz = (uint16_t)mhz;
  *changed = true;
  powerApplyCpuFrequency(prefs);
  if (mhz)
    snprintf(reply, 160, "OK - %u MHz", (unsigned)mhz);
  else
    strcpy(reply, "OK - default");
#else
  strcpy(reply, "Board not supported");
#endif
  return true;
}
