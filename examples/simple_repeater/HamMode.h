// HamMode.h: dual Part 97 / Part 15 transmit power for the repeater.
// Fork-owned; see HAM.md for the user-facing model.
//
// Ham mode never changes a packet. It only picks a TX power just before the
// dispatcher starts a physical transmit. Anything not positively identified as
// publicly readable is encrypted, so a mistake can only send *less* power on
// public traffic, never ham power on private traffic.
//
// The live node name is passed in at every entry point instead of stored, so a
// `set name` takes effect on the activation gate without a refresh hook.

#ifndef _HAM_MODE_H
#define _HAM_MODE_H

#include <Arduino.h>
#include <Packet.h>
#include <helpers/IdentityStore.h>   // FILESYSTEM typedef
#include "PersistUtil.h"
#include "HamModeConfig.h"

enum HamTxClass : uint8_t {
  HAM_TX_ENCRYPTED = 0,
  HAM_TX_PUBLIC = 1,
};

class HamMode {
public:
  HamMode();

  void load(FILESYSTEM* fs);
  void save(FILESYSTEM* fs);
  // Flushes lazy saves and watches the active-state transition. Does not transmit.
  void loop(uint32_t now, FILESYSTEM* fs, const char* node_name);

  // Pure function of packet type and payload bytes. Public for host tests.
  HamTxClass classify(const mesh::Packet* pkt) const;

  // The only place that decides public versus private. `default_dbm` is the
  // live `set tx` value, returned for every encrypted packet and whenever the
  // activation gate is not satisfied.
  int8_t selectPower(const mesh::Packet* pkt, const char* node_name, int8_t default_dbm);

  bool isActive(const char* node_name) const;
  const char* inactiveReason(const char* node_name) const;

  // `set tx` changed (or boot applied it): the radio is now at private power.
  void noteRadioPower(int8_t dbm);
  // The applied-power cache: only write the radio when the chosen class really
  // changes, so RXPS duty cycle is not torn down on every packet.
  bool shouldApplyPower(int8_t dbm) const { return dbm != applied_dbm; }
  void noteAppliedPower(int8_t dbm) { applied_dbm = dbm; }

  // Station ID timer. `loop()` arms it on a false-to-true active transition;
  // every advert reaching the air pushes it out another interval.
  bool idDue(uint32_t now) const;
  void noteAdvertOnAir(uint32_t now);
  uint32_t secondsToId(uint32_t now) const;

  // Read-outs used by the CLI and the host suite.
  bool isEnabled() const { return cfg.enabled != 0; }
  const char* getCallsign() const { return cfg.callsign; }
  int8_t getHamDbm() const { return cfg.ham_dbm; }
  int8_t getPrivateDbm() const { return private_dbm; }
  int8_t getAppliedDbm() const { return applied_dbm; }
  int getTagCount() const;
  const char* getTag(int i) const { return cfg.tags[i]; }
  int findTag(const char* name) const;
  uint32_t getPublicCount() const { return n_public; }
  uint32_t getPrivateCount() const { return n_private; }

  // Mutations. The CLI validates input before calling these.
  void setEnabled(bool on);
  void setCallsign(const char* call);
  void setHamDbm(int8_t dbm);
  bool addTag(const char* name);     // false on duplicate or a full list
  bool removeTag(const char* name);  // false when the name is not listed
  void markDirty() { save_flag.markDirty(); }

  // Used only by the staged-save callbacks in HamMode.cpp.
  const HamCfgRecord& getRecord() const { return cfg; }
  void applyRecord(const HamCfgRecord& rec) { cfg = rec; rebuildKeys(); }

private:
  struct KnownKey {
    uint8_t secret[16];
    uint8_t hash;
    bool used;
  };

  HamCfgRecord cfg;
  KnownKey public_key;
  KnownKey tag_keys[HAM_MAX_TAGS];
  // One shared zero-padded secret and one shared decrypt scratch keep the
  // per-packet frame small and predictable; classify() is const, so mutable.
  mutable uint8_t hmac_secret[PUB_KEY_SIZE];
  mutable uint8_t decrypt_scratch[MAX_PACKET_PAYLOAD];

  int8_t private_dbm;
  int8_t applied_dbm;
  uint32_t n_public;
  uint32_t n_private;

  uint32_t next_id_at;
  bool was_active;

  LazySave save_flag;

  void resetToDefaults();
  void rebuildKeys();
  bool tryKnownKey(const KnownKey& key, uint8_t hash, const uint8_t* payload, size_t len) const;
  bool matchesKnownKey(const uint8_t* payload, size_t len) const;
};

// `command` is everything after the "ham" verb.
void hamCLI(HamMode& ham, const char* node_name, const char* command, char* reply);

#endif  // _HAM_MODE_H
