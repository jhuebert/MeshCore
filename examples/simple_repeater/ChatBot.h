// ChatBot.h — fixed, reactive replies to test/ping chatter on one keyed channel.
// Fork-owned; see BOT.md for the behaviour and the mesh's bot-etiquette rules.
//
// The bot owns no radio, filesystem or RNG. MyMesh injects a send callback (the
// only place that builds a group packet) and a jitter callback (the only place
// with an RNG). Host tests drive the same three entry points with fakes:
// onGroupData(), appendChannelByHash() and botCLI().

#ifndef _CHAT_BOT_H
#define _CHAT_BOT_H

#include <Arduino.h>
#include <Mesh.h>
#include <helpers/IdentityStore.h>   // FILESYSTEM typedef
#include <helpers/RegionMap.h>
#include "ChatBotConfig.h"
#include "PersistUtil.h"

class ChatBot {
public:
  // Compose nothing on air: MyMesh builds the group datagram from the text.
  // Returns true iff a packet was created and queued.
  using SendFn = bool (*)(void* ctx, const uint8_t* secret, uint8_t chan_hash,
                          uint8_t radio_profile, uint32_t delay_ms,
                          const char* text, size_t text_len);
  using JitterFn = uint32_t (*)(void* ctx);

private:
  bool enabled;
  char name[BOT_NAME_MAX + 1];
  uint8_t psk[32];
  uint8_t psk_len;
  uint8_t chan_hash;
  uint32_t last_reply_ms;   // wrap-safe millis() stamp of the last successful TX
  bool has_replied;         // a stamp of 0 is a valid time, so it needs its own flag
  void* ctx;
  SendFn send;
  JitterFn jitter;
  LazySave save_flag;

  void resetToDefaults();

public:
  ChatBot();

  void begin(FILESYSTEM* fs, void* ctx, SendFn send, JitterFn jitter);
  void loop(FILESYSTEM* fs);        // lazy dirty-save only
  void load(FILESYSTEM* fs);
  void save(FILESYSTEM* fs);

  // Receive hook, called below the battery gate and after the filter verdict.
  void onGroupData(const mesh::Packet* pkt, uint8_t type, const mesh::GroupChannel& channel,
                   const uint8_t* data, size_t len, const RegionEntry* region,
                   const char* node_name);

  // Keyed-channel supply for core's group decryption (fleet's contract).
  int appendChannelByHash(const uint8_t* hash, mesh::GroupChannel dest[],
                          int max_matches, int filled);

  bool isEnabled() const { return enabled; }
  void setEnabled(bool on);

  // CLI mutators. setName(NULL/"") clears; setChannel takes '#name' or hex.
  void setName(const char* n);
  bool setChannel(const char* value);
  void clearChannel();

  // Read-only accessors for the persistence writer and the status line.
  const char* getName() const { return name; }
  uint8_t getPskLen() const { return psk_len; }
  const uint8_t* getPsk() const { return psk; }
  uint8_t getChanHash() const { return chan_hash; }
};

void botCLI(ChatBot& bot, const char* command, char* reply);

#endif // _CHAT_BOT_H
