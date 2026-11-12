// Radio2Config.h — constants for the companion's second radio preset.
// Fork-owned; see CUSTOM_COMPANION.md (Radio2 second preset) for the
// user-facing model.

#ifndef _RADIO2_CONFIG_H
#define _RADIO2_CONFIG_H

#include <stdint.h>

#define RADIO2_CFG_FILE          "/radio2_cfg"
#define RADIO2_CFG_VERSION       2
#define RADIO2_CFG_MAGIC         "R2CF"

// The persisted payload is 20 B; the file appends a 2 B CRC-16.
// v2 carries the local TX preset in the byte that was implicit struct padding,
// so the record layout is byte-identical to v1 (a v1 file loads with
// tx_local == 0, i.e. RADIO_PROFILE_PRIMARY).
#define RADIO2_CFG_PAYLOAD_BYTES 20
#define RADIO2_CFG_RECORD_BYTES  22

struct Radio2CfgRecord {
  char     magic[4];          // "R2CF"
  uint8_t  version;           // 2
  uint8_t  enabled;           // 0/1
  uint8_t  policy_primary;    // Radio2Action
  uint8_t  policy_secondary;  // Radio2Action
  uint8_t  advert;            // Radio2Advert
  uint8_t  sf;
  uint8_t  cr;
  uint8_t  tx_local;        // local TX preset (RADIO_PROFILE_*); 0 on v1 files
  float    freq_mhz;
  float    bw_khz;
};
static_assert(sizeof(Radio2CfgRecord) == RADIO2_CFG_PAYLOAD_BYTES,
              "Radio2CfgRecord payload is frozen at 20 B");

// Scheduler timing. RADIO2_SWITCH_US is a conservative default; measure on
// hardware before tightening it.
#define RADIO2_ACTIVE_HOLD_MS        30000UL
#define RADIO2_SWITCH_US             3000UL
#define RADIO2_LOOP_MARGIN_US        2000UL
#define RADIO2_MIN_VISIT_US          5000UL
#define RADIO2_ACTIVE_SECONDARY_DIV  4U
#define RADIO2_SLEEP_MAX_MS          1000UL
#define RADIO2_APPLY_DELAY_MS        2000UL
#define RADIO2_MAX_SWITCH_FAILURES   10U
#define RADIO2_SWITCH_RETRY_MS       1000UL
#define RADIO2_OVERLAP_GUARD_KHZ     6.0f

#endif  // _RADIO2_CONFIG_H
