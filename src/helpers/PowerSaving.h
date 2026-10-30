#pragma once

#include "MeshCore.h"

#if defined(ESP32_PLATFORM)
#include "esp_pm.h"
#include "esp_bt.h"
#include <esp_err.h>
#include "esp_sleep.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#endif

class PowerSaving {
public:
    #if defined(ESP32_PLATFORM)
    // Companion power profile. `powersaving` is the original switch: BLE modem
    // sleep plus DVFS. `light_sleep` adds automatic light sleep (the idle task
    // stops the CPU between events), which is the only way an ESP32 companion
    // drops below the ~20 mA an awake CPU costs - the repeater gets there by
    // calling board.sleep() itself, a companion cannot (BLE has to keep its
    // connection events, so only the PM/tickless path is safe).
    struct CompanionConfig {
        bool powersaving;
        bool light_sleep;
    };
    // Returns false when the PM block refused the profile, so the caller can
    // retry instead of latching a state that never took effect.
    static bool applyCompanionPowerSaving(const CompanionConfig& cfg);
    static esp_err_t configurePowerManagement(int max_freq_mhz, int min_freq_mhz, bool light_sleep_enable);

    // Radio IRQ (DIO1) as a light sleep wake source, so a packet ends the sleep
    // at once instead of waiting for the next loop tick. Arm it around the idle
    // wait and disarm it right after: waking on a level needs the pin to be
    // level-triggered, and leaving it that way floods the CPU with interrupts
    // the moment the radio asserts the line (fire and forget is what a TX does).
    static void armRadioWakeup(int gpio);
    static void disarmRadioWakeup(int gpio);
    #endif
};
