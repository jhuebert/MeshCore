#include "helpers/PowerSaving.h"

#if defined(ESP32_PLATFORM)
// PowerSaving for companions
bool PowerSaving::applyCompanionPowerSaving(const CompanionConfig& cfg) {
#if defined(BLE_PIN_CODE) && !CONFIG_IDF_TARGET_ESP32C6
  // The BT controller has to be asleep-capable before the PM block may put the
  // CPU to sleep while a connection is open, so a light sleep request implies
  // modem sleep even when `powersaving` itself is off.
  if (cfg.powersaving || cfg.light_sleep) {
    esp_bt_sleep_enable();
  } else {
    esp_bt_sleep_disable();
  }
#endif

  int max_freq_mhz = 80;
#ifdef ESP32_CPU_FREQ
  max_freq_mhz = ESP32_CPU_FREQ;
#endif
  int min_freq_mhz = cfg.powersaving ? 10 : max_freq_mhz;

  esp_err_t errPM = configurePowerManagement(max_freq_mhz, min_freq_mhz, cfg.light_sleep);
  if (errPM == ESP_OK) return true;

  if (cfg.light_sleep) {
    // Automatic light sleep is unavailable in this build/config (e.g. a BT
    // controller that is not sleep-capable). Keep the CPU awake rather than
    // ending up with a partly applied profile.
    POWERSAVING_DEBUG_PRINTLN("Light sleep unavailable, staying awake: %d", errPM);
    configurePowerManagement(max_freq_mhz, min_freq_mhz, false);
  }
  return false;
}

// The pin is level-triggered only while it is a wake source; see the note in
// PowerSaving.h. Restore the pin's own (disabled) interrupt type afterwards -
// gpio_wakeup_disable() does not, and the repeater's sleep() restores it for the
// same reason.
void PowerSaving::armRadioWakeup(int gpio) {
  if (gpio < 0) return;

  gpio_num_t pin = (gpio_num_t) gpio;
  gpio_wakeup_enable(pin, GPIO_INTR_HIGH_LEVEL);   // DIO1 goes high on a packet
  esp_sleep_enable_gpio_wakeup();
}

void PowerSaving::disarmRadioWakeup(int gpio) {
  if (gpio < 0) return;

  gpio_num_t pin = (gpio_num_t) gpio;
  gpio_wakeup_disable(pin);
  gpio_set_intr_type(pin, GPIO_INTR_DISABLE);
}

esp_err_t PowerSaving::configurePowerManagement(int max_freq_mhz, int min_freq_mhz, bool light_sleep_enable) {
  // Configure Power Management
#if CONFIG_IDF_TARGET_ESP32C3
  esp_pm_config_esp32c3_t pm_config;
#elif CONFIG_IDF_TARGET_ESP32S3
  esp_pm_config_esp32s3_t pm_config;
#elif CONFIG_IDF_TARGET_ESP32
  esp_pm_config_esp32_t pm_config;
#elif CONFIG_IDF_TARGET_ESP32C6
  esp_pm_config_t pm_config;
#endif

  // Set PM Config
  pm_config = { .max_freq_mhz = max_freq_mhz, .min_freq_mhz = min_freq_mhz, .light_sleep_enable = light_sleep_enable };
  esp_err_t errPM = esp_pm_configure(&pm_config);
  if (errPM == ESP_OK) {
    POWERSAVING_DEBUG_PRINTLN("Power Management configured successfully");
  } else {
    POWERSAVING_DEBUG_PRINTLN("Power Management failed to configure: %d\r\n", errPM);
  }

  return errPM;
}
#endif
