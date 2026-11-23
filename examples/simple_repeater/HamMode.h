// HamMode.h: dual Part 97 / Part 15 transmit power for the repeater.
// Fork-owned; see HAM.md for the user-facing model.
//
// Ham mode never changes a packet. It only picks a TX power just before the
// dispatcher starts a physical transmit. Anything not positively identified as
// publicly readable by the packet classifier stays at normal power.
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

// CommonCLI adverts spend one flag byte and, when sharing position, two
// 32-bit coordinates before the UTF-8 name.
struct HamAdvertName {
  const char* text;
  size_t budget;
  HamAdvertName(const char* name, bool with_position = false)
      : text(name), budget(MAX_ADVERT_DATA_SIZE - 1 - (with_position ? 8 : 0)) {}
};

class HamMode {
public:
  HamMode();

  void load(FILESYSTEM* fs);
  void save(FILESYSTEM* fs);
  // Flushes lazy saves and watches the active-state transition. Does not transmit.
  void loop(uint32_t now, FILESYSTEM* fs, HamAdvertName node_name);

  // Pure function of packet type and payload bytes. Public for host tests.
  HamTxClass classify(const mesh::Packet* pkt) const;

  // The only place that decides public versus private. `default_dbm` is the
  // live `set tx` value, returned for every encrypted packet and whenever the
  // activation gate is not satisfied.
  int8_t selectPower(const mesh::Packet* pkt, HamAdvertName node_name, int8_t default_dbm);

  bool isActive(HamAdvertName node_name) const;
  const char* inactiveReason(HamAdvertName node_name) const;

  // Result of the normal power write at boot or from `set tx`.
  void noteRadioPower(int8_t dbm, bool ok);
  // The applied-power cache: only write the radio when the chosen class really
  // changes, so RXPS duty cycle is not torn down on every packet.
  template<class Radio>
  bool prepareTransmit(Radio& radio, const mesh::Packet* pkt, HamAdvertName name, int8_t normal) {
    bool active = isActive(name);
    if (!active && !restore_power) return true;
    int8_t dbm = active ? activePower(pkt, normal) : normal;
    if (power_known && dbm == applied_dbm) return true;
    radio.setTxPower(dbm);
    power_known = radio.lastTxPowerOk();
    // Even a failed PA write may have partially configured the chip. Restore
    // private power before allowing another send, including after `ham off`.
    restore_power = !power_known || dbm != normal;
    if (power_known) applied_dbm = dbm;
    return power_known;
  }

  // Station ID timer. `loop()` arms it on a false-to-true active transition;
  // creating a local advert pushes it out another interval.
  bool idDue(uint32_t now) const;
  void noteAdvertOnAir(uint32_t now);
  uint32_t secondsToId(uint32_t now) const;

  // Read-outs used by the CLI and the host suite.
  bool isEnabled() const { return cfg.enabled != 0; }
  const char* getCallsign() const { return cfg.callsign; }
  int8_t getHamDbm() const { return cfg.ham_dbm; }
  int8_t getPrivateDbm() const { return private_dbm; }
  int8_t getAppliedDbm() const { return applied_dbm; }
  bool powerKnown() const { return power_known; }
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
  bool power_known, restore_power;
  uint32_t n_public;
  uint32_t n_private;

  uint32_t next_id_at;
  bool was_active;

  LazySave save_flag;

  void resetToDefaults();
  int8_t activePower(const mesh::Packet* pkt, int8_t normal);
  void rebuildKeys();
  bool tryKnownKey(const KnownKey& key, uint8_t hash, const uint8_t* payload, size_t len) const;
  bool matchesKnownKey(const uint8_t* payload, size_t len) const;
};

// `command` is everything after the "ham" verb.
void hamCLI(HamMode& ham, HamAdvertName node_name, const char* command, char* reply);

#endif  // _HAM_MODE_H
