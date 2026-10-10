#pragma once

#ifdef WITH_MQTT_OBSERVER

#include <Arduino.h>
#include <Identity.h>
#include <Packet.h>
#include <helpers/IdentityStore.h>
#include <PsychicMqttClient.h>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "../simple_repeater/PersistUtil.h"
#include "MQTTObserverConfig.h"
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
  };

  enum State : uint8_t {
    STATE_OFF, STATE_CONFIG, STATE_WIFI, STATE_TIME, STATE_CONNECTING,
    STATE_CONNECTED, STATE_RETRY, STATE_ERROR
  };

  struct BrokerRuntime {
    PsychicMqttClient* client;
    char server_uri[sizeof(MQTTObserverBrokerConfig::server) + 16];
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

    BrokerRuntime();
  };

  static const uint32_t kMinimumValidTime = 1735689600UL;
  static const uint32_t kJwtLifetimeSeconds = 86400;

  FILESYSTEM* _fs;
  mesh::LocalIdentity* _identity;
  const char* _origin_source;
  char _origin[33];
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
  MQTTObserverEventQueue<Event, MQTT_OBSERVER_QUEUE_CAPACITY> _queue;
  MQTTObserverRawStager _raw_stager;
  BrokerRuntime _brokers[MQTT_OBSERVER_MAX_BROKERS];
  bool _wifi_started;
  TaskHandle_t _task;
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
  bool configureClient(size_t slot, const MQTTObserverBrokerConfig& config);
  bool createJwt(size_t slot, const MQTTObserverBrokerConfig& config, uint32_t& expires_at);
  bool publishEvent(const Event& event);
  bool publishStatus(size_t slot);
  void disconnectBroker(size_t slot);
  void trackDisconnectedQueue(uint32_t now);
  void flushQueue();
  size_t queueSize();
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
             float freq, float bw, uint8_t sf, uint8_t cr);
  void loop();
  void setRadio(float freq, float bw, uint8_t sf, uint8_t cr);
  bool needsAwake() const;
  void stageRaw(float snr, float rssi, const uint8_t raw[], int len);
  void capture(mesh::Packet* packet, float score);
  bool handleCommand(char* command, char* reply);
  void formatStatus(char* reply, size_t capacity);
};

#endif  // WITH_MQTT_OBSERVER
