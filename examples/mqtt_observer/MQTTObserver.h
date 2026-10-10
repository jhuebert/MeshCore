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
    uint32_t received_ms;
    float snr;
    float rssi;
    float score;
  };

  enum State : uint8_t {
    STATE_OFF, STATE_CONFIG, STATE_WIFI, STATE_TIME, STATE_CONNECTING,
    STATE_CONNECTED, STATE_RETRY, STATE_ERROR
  };

  static const size_t kQueueCapacity = 4;
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
  uint8_t _save_record[MQTTObserverRecord::kRecordSize];
  mutable portMUX_TYPE _config_mux;
  portMUX_TYPE _queue_mux;
  MQTTObserverLiveQueue<Event, kQueueCapacity> _queue;
  MQTTObserverRawStager _raw_stager;
  PsychicMqttClient* _client;
  TaskHandle_t _task;
  MQTTObserverConfig _runtime_config;
  char _device_id[2 * PUB_KEY_SIZE + 1];
  // PsychicMqttClient keeps the URI pointer in its client config across async connects.
  char _server_uri[sizeof(_config.server) + 16];
  char _jwt_username[3 + 2 * PUB_KEY_SIZE + 1];
  char _jwt_token[1024];
  char _json[2048];
  bool _connected;
  bool _status_pending;
  bool _wifi_started;
  bool _client_started;
  uint32_t _next_connect_ms;
  uint32_t _last_status_ms;
  uint32_t _jwt_expiry;
  uint8_t _backoff_index;
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
  bool ready(const MQTTObserverConfig& config) const;
  bool configureClient(const MQTTObserverConfig& config);
  bool createJwt(char* token, size_t capacity, uint32_t& expires_at);
  bool publishEvent(const Event& event);
  bool publishStatus();
  void clearQueueOnDisconnect();
  size_t queueSize();
  void markDirty();
  bool saveConfig();
  static bool writeRecord(File& file, void* context);
  static bool validateRecord(File& file, void* context);
  static bool readRecord(File& file, MQTTObserverConfig& config);
  bool handleSetCommand(char* command, char* reply);
  bool handleGetCommand(const char* command, char* reply);
  bool setString(char* destination, size_t capacity, const char* value);
  const char* stateName() const;

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
