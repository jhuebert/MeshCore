// ChatBotConfig.h — tunables and persisted record for the simple_repeater chat
// bot. Fork-owned; see BOT.md for the user-facing behaviour.

#ifndef _CHAT_BOT_CONFIG_H
#define _CHAT_BOT_CONFIG_H

#include <stddef.h>
#include <stdint.h>

#define BOT_CFG_FILE          "/bot_cfg"
#define BOT_CFG_MAGIC         "BOTC"
#define BOT_CFG_VERSION       1

#define BOT_NAME_MAX          16   // reply name, NUL not included

// One reply maximum per BOT_MIN_INTERVAL_MS; MyMesh owns the RNG that spaces
// the actual transmission inside BOT_REPLY_DELAY_*.
#ifndef BOT_MIN_INTERVAL_MS
#define BOT_MIN_INTERVAL_MS   5000
#endif
#ifndef BOT_REPLY_DELAY_MIN_MS
#define BOT_REPLY_DELAY_MIN_MS 1000
#endif
#ifndef BOT_REPLY_DELAY_MAX_MS
#define BOT_REPLY_DELAY_MAX_MS 3000
#endif

// A group-text reply is "<ts(4)><txt_type(1)><text>"; BOT_TEXT_MAX matches
// MAX_TEXT_LEN so the 5-byte header plus NUL still fit one packet.
#define BOT_TEXT_MAX          160
#define BOT_REPLY_BUF         (BOT_TEXT_MAX + 6)

// Persisted record: magic + version + enabled + name + PSK length + PSK, then
// a trailing CRC-16 over the payload. The channel name is deliberately NOT
// stored: it is only a command-time input to the key derivation.
struct BotCfgRecord {
  char     magic[4];                //  0  "BOTC"
  uint8_t  version;                 //  4
  uint8_t  enabled;                 //  5
  char     name[BOT_NAME_MAX + 1];  //  6  17 B
  uint8_t  psk_len;                 // 23  0 (no channel), 16 or 32
  uint8_t  psk[32];                 // 24  zero-padded
};                                   // = 56 B
#define BOT_CFG_PAYLOAD_BYTES 56
#define BOT_CFG_RECORD_BYTES  58   // payload + 2 B CRC-16
static_assert(sizeof(BotCfgRecord) == BOT_CFG_PAYLOAD_BYTES,
              "the bot config record is frozen at 56 bytes");

#endif // _CHAT_BOT_CONFIG_H
