// HamModeConfig.h: tunables and persisted record for the repeater's ham mode.
// Fork-owned; see HAM.md for the user-facing model.

#ifndef _HAM_MODE_CONFIG_H
#define _HAM_MODE_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define HAM_CFG_FILE            "/ham_cfg"
#define HAM_CFG_VERSION         1
#define HAM_CFG_MAGIC           "HAMC"

// A self-contained list of publicly known hashtag channels. The well-known
// Public channel is hardcoded in KeyedChannel.h and takes no slot here.
#define HAM_MAX_TAGS            8
#define HAM_TAG_MAX_LEN         16   // '#' + up to 14 chars + NUL, as parseChannelSpec
#define HAM_CALLSIGN_MAX_LEN    16   // room for an /indicator suffix

// 47 CFR 97.119(a): the call sign must be sent at least every 10 minutes.
// The interval is fixed, so there is no config field and no CLI to change it.
#define HAM_ID_INTERVAL_MS      (10UL * 60UL * 1000UL)
// Same delay the zero-hop advert CLI uses. Arming subtracts it so the on-air
// spacing, not the scheduling spacing, is 10 minutes.
#define HAM_ID_SEND_DELAY_MS    1500UL

// Default for `ham tx`, the power used by publicly readable traffic. The
// board's compiled LORA_TX_POWER is the right fresh-install value when one is
// present; the fallback keeps host builds independent of a board variant.
#ifdef LORA_TX_POWER
  #define HAM_TX_POWER LORA_TX_POWER
#else
  #define HAM_TX_POWER 20
#endif

// Persisted payload: 4 + 1 + 1 + 1 + 1 + 16 + 8*16 = 152 B. The file appends a
// 2-byte CRC-16, so the record on disk is 154 B. Names are stored; the compact
// key cache is derived on load and on every add/remove.
#define HAM_CFG_PAYLOAD_BYTES   152
#define HAM_CFG_RECORD_BYTES    154

struct HamCfgRecord {
  char     magic[4];                              // offset 0, "HAMC"
  uint8_t  version;                               // offset 4
  uint8_t  enabled;                               // offset 5
  uint8_t  reserved;                              // offset 6
  int8_t   ham_dbm;                               // offset 7
  char     callsign[HAM_CALLSIGN_MAX_LEN];         // offset 8
  char     tags[HAM_MAX_TAGS][HAM_TAG_MAX_LEN];    // offset 24
};
static_assert(sizeof(HamCfgRecord) == 152,
              "HamCfgRecord payload is frozen at 152 B");
static_assert(offsetof(HamCfgRecord, callsign) == 8,
              "HamCfgRecord callsign offset is frozen at 8");
static_assert(offsetof(HamCfgRecord, tags) == 24,
              "HamCfgRecord tags offset is frozen at 24");

#endif  // _HAM_MODE_CONFIG_H
