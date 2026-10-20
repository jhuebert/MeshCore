#pragma once

#ifdef WITH_MQTT_OBSERVER

#include <Arduino.h>
#include <Identity.h>
#include <Packet.h>
#include <helpers/IdentityStore.h>
#include <PsychicMqttClient.h>
#include <atomic>
#include <freertos/FreeRTOS.h>

#include "../simple_repeater/PersistUtil.h"
#include "MQTTObserverConfig.h"
#include "MQTTObserverFormat.h"
#include "MQTTObserverPolicy.h"
#include "MQTTObserverRecord.h"

class MQTTObserver {
  struct Event {
    mesh::Packet packet;
    uint8_t raw[MQTTObserverRawStager::kCapacity];
    uint16_t raw_len;
    time_t timestamp;
    long timestamp_usec;
    float snr;
    float rssi;
    float score;
    bool is_tx;
  };

  using EventQueue = MQTTObserverEventQueue<Event, MQTT_OBSERVER_QUEUE_CAPACITY>;

  enum State : uint8_t {
    STATE_OFF, STATE_CONFIG, STATE_WIFI, STATE_TIME, STATE_CONNECTING,
    STATE_CONNECTED, STATE_RETRY, STATE_ERROR
  };

  struct BrokerRuntime {
    PsychicMqttClient* client;
    char server_uri[sizeof(MQTTObserverBrokerConfig::server) + 16];
    char client_id[2 * PUB_KEY_SIZE + 3];
    char username[65];
    char password[65];
    char jwt_username[3 + 2 * PUB_KEY_SIZE + 1];
    char jwt_token[1024];
    uint32_t jwt_expiry;
    uint32_t next_connect_ms;
    uint32_t attempt_started;
    std::atomic<uint32_t> connected_at;
    uint32_t last_status_ms;
    uint32_t status_retry_ms;
    uint8_t backoff_index;
    bool status_retry_pending;
    bool client_started;
    bool configured;
    std::atomic<bool> connected;
    std::atomic<bool> status_pending;
    std::atomic<State> state;
    std::atomic<int> probe_msg_id;
    std::atomic<uint32_t> probe_sent_ms;
    uint32_t last_probe_ms;

    BrokerRuntime();

    // One outstanding QoS 1 liveness probe per broker; a PUBACK for it clears
    // the outstanding marker, and starting a new one only happens when none is
    // in flight.
    void noteProbe(int msg_id);
    void notePuback(int msg_id);
  };

  static const uint32_t kMinimumValidTime = 1735689600UL;
  static const uint32_t kJwtLifetimeSeconds = 86400;

  // Supplies live device stats for the periodic status message; the repeater
  // registers this so the observer reports battery/uptime/noise floor and the
  // relay flag like other MeshCore observers.
  using StatusStatsFn = void (*)(void* context, MQTTObserverFormat::StatusStats& stats);

  FILESYSTEM* _fs;
  mesh::LocalIdentity* _identity;
  const char* _origin_source;
  char _origin[33];
  char _model[33];
  char _firmware[64];
  StatusStatsFn _status_stats_fn;
  void* _status_stats_ctx;
  float _radio_freq;
  float _radio_bw;
  uint8_t _radio_sf;
  uint8_t _radio_cr;
  MQTTObserverConfig _config;
  MQTTObserverConfig _runtime_config;
  MQTTObserverConfig _work_config;
  MQTTObserverConfig _io_config;
  uint8_t _save_record[MQTTObserverRecord::kRecordSize];
  uint8_t _load_record[MQTTObserverRecord::kRecordSize];
  mutable portMUX_TYPE _config_mux;
  portMUX_TYPE _queue_mux;
  EventQueue _queue;
  MQTTObserverRawStager _raw_stager;
  BrokerRuntime _brokers[MQTT_OBSERVER_MAX_BROKERS];
  bool _wifi_started;
  uint32_t _queue_disconnected_since;
  uint32_t _last_ntp_request;
  char _device_id[2 * PUB_KEY_SIZE + 1];
  char _json[2048];
  std::atomic<State> _state;
  LazySave _save_state;
  std::atomic<uint32_t> _received;
  std::atomic<uint32_t> _published;
  std::atomic<uint32_t> _disconnected_drops;
  std::atomic<uint32_t> _queue_drops;
  std::atomic<uint32_t> _publish_drops;

  static void taskEntry(void* context);
  void worker();
  void copyConfig(MQTTObserverConfig& config);
  bool ready(const MQTTObserverConfig& config, size_t slot) const;
  bool hasEnabledBrokerLocked() const;   // caller holds _config_mux
  bool hasConnectHeadroom(bool secure) const;
  bool configureClient(size_t slot, const MQTTObserverConfig& config);
  bool createJwt(size_t slot, const MQTTObserverBrokerConfig& config, uint32_t& expires_at);
  bool publishEvent(const Event& event, const MQTTObserverConfig& config);
  bool publishStatus(size_t slot, const MQTTObserverConfig& config);
  bool buildStatusMessage(char* out, size_t capacity, size_t& written);
  void disconnectBroker(size_t slot);
  void deferReconnect(size_t slot, uint32_t now);
  void trackDisconnectedQueue(uint32_t now);
  void flushQueue();
  size_t queueSize();
  bool stampEvent(Event& event);
  void enqueueEvent(const Event& event);
  void markDirty();
  bool saveConfig();
  static bool writeRecord(File& file, void* context);
  static bool validateRecord(File& file, void* context);
  bool readRecord(File& file, MQTTObserverConfig& config);
  bool handleSetCommand(char* command, char* reply);
  bool handleGetCommand(const char* command, char* reply);
  bool setString(char* destination, size_t capacity, const char* value);
  const char* stateName(State state) const;
  bool commandSlot(const char* command, const char* operation, size_t& slot,
                   const char*& property) const;

public:
  MQTTObserver();

  void begin(FILESYSTEM* fs, mesh::LocalIdentity* identity, const char* origin,
             const char* model, const char* firmware, float freq, float bw, uint8_t sf, uint8_t cr);
  void setStatusStatsFn(StatusStatsFn fn, void* context);
  void loop();
  void setRadio(float freq, float bw, uint8_t sf, uint8_t cr);
  bool needsAwake() const;
  void stageRaw(float snr, float rssi, const uint8_t raw[], int len);
  void capture(mesh::Packet* packet, float score);
  void captureTx(mesh::Packet* packet);
  bool handleCommand(char* command, char* reply);
  void formatStatus(char* reply, size_t capacity);
};

#endif  // WITH_MQTT_OBSERVER
