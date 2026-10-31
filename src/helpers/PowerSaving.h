#pragma once

#include "MeshCore.h"

#if defined(ESP32_PLATFORM)
#include "esp_pm.h"
#include "esp_bt.h"
#include <esp_err.h>
#include "driver/uart.h"
#endif

class PowerSaving {
public:
    #if defined(ESP32_PLATFORM)
    // Companion profile. `powersaving` is the original switch: BLE modem sleep
    // plus DVFS. `light_sleep` adds automatic light sleep, the only way an ESP32
    // companion gets below the ~20 mA an awake CPU costs - the repeater reaches
    // that with board.sleep(), which a companion cannot use because BLE has to
    // keep its connection events.
    // Returns false when the PM block refused the profile, so the caller retries
    // instead of latching something that never took effect.
    static bool applyCompanionPowerSaving(bool powersaving, bool light_sleep);

  private:
    static esp_err_t configurePowerManagement(int max_freq_mhz, int min_freq_mhz, bool light_sleep_enable);
    #endif
};
