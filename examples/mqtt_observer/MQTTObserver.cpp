#ifdef WITH_MQTT_OBSERVER

#include "MQTTObserver.h"
#include "MQTTObserverFormat.h"
#include "MQTTObserverRecord.h"
#include <WiFi.h>
#include <mbedtls/base64.h>
#include <sys/time.h>
#include <time.h>
#include <new>

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "unknown"
#endif
namespace {

const char kConfigPath[] = "/mqtt_prefs";
const uint32_t kBackoffMs[] = {10000, 30000, 60000, 120000, 300000};

void bytesToHex(const uint8_t* bytes, size_t len, char* out, bool lowercase = false) {
  const char* digits = lowercase ? "0123456789abcdef" : "0123456789ABCDEF";
  for (size_t i = 0; i < len; i++) {
    out[2 * i] = digits[bytes[i] >> 4];
    out[2 * i + 1] = digits[bytes[i] & 0x0f];
  }
  out[2 * len] = 0;
}

bool startsWith(const char* text, const char* prefix) {
  return text && strncmp(text, prefix, strlen(prefix)) == 0;
}

const char* commandValue(const char* command, const char* prefix) {
  size_t len = strlen(prefix);
  if (strncmp(command, prefix, len) != 0 || (command[len] && command[len] != ' ')) return nullptr;
  const char* value = command + len;
  while (*value == ' ') value++;
  return value;
}

bool parseToggle(const char* value, uint8_t& enabled) {
  if (strcmp(value, "on") == 0) { enabled = 1; return true; }
  if (strcmp(value, "off") == 0) { enabled = 0; return true; }
  return false;
}

bool base64Url(const uint8_t* input, size_t input_len, char* output, size_t capacity, size_t& written) {
  if (!input || !output || capacity < 2) return false;
  size_t len = 0;
  if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(output), capacity - 1,
                            &len, input, input_len) != 0 || len >= capacity) return false;
  for (size_t i = 0; i < len; i++) {
    if (output[i] == '+') output[i] = '-';
    else if (output[i] == '/') output[i] = '_';
  }
  while (len && output[len - 1] == '=') len--;
  output[len] = 0;
  written = len;
  return true;
}

bool secureUri(const char* uri) {
  return strncmp(uri, "mqtts://", 8) == 0 || strncmp(uri, "wss://", 6) == 0;
}

bool makeTopic(const MQTTObserverConfig& config, const char* device_id,
               const char* leaf, char* out, size_t capacity) {
  int len = snprintf(out, capacity, "meshcore/%s/%s/%s", config.iata, device_id, leaf);
  return len > 0 && static_cast<size_t>(len) < capacity;
}

}  // namespace

MQTTObserver::MQTTObserver()
    : _fs(nullptr), _identity(nullptr), _origin_source(nullptr), _origin{},
      _radio_freq(0), _radio_bw(0), _radio_sf(0), _radio_cr(0), _config{},
      _config_mux(portMUX_INITIALIZER_UNLOCKED), _queue_mux(portMUX_INITIALIZER_UNLOCKED),
      _queue(), _raw_stager(), _client(nullptr), _task(nullptr), _runtime_config{},
      _device_id{}, _jwt_username{}, _jwt_token{}, _json{}, _connected(false),
      _status_pending(false), _wifi_started(false), _client_started(false),
      _next_connect_ms(0), _last_status_ms(0), _jwt_expiry(0), _backoff_index(0),
      _state(STATE_OFF), _save_state(), _received(0), _published(0),
      _disconnected_drops(0), _queue_drops(0), _publish_drops(0) {
  MQTTObserverConfigCodec::setDefaults(_config);
  MQTTObserverConfigCodec::setDefaults(_runtime_config);
}

void MQTTObserver::begin(FILESYSTEM* fs, mesh::LocalIdentity* identity, const char* origin,
                         float freq, float bw, uint8_t sf, uint8_t cr) {
  _fs = fs;
  _identity = identity;
  _origin_source = origin;
  _radio_freq = freq;
  _radio_bw = bw;
  _radio_sf = sf;
  _radio_cr = cr;
  strncpy(_origin, origin ? origin : "", sizeof(_origin) - 1);
  bytesToHex(identity->pub_key, PUB_KEY_SIZE, _device_id);

  char path[64];
  PersistLoadSource source = chooseConfigToLoad(fs, kConfigPath, validateRecord, nullptr,
                                                 path, sizeof(path));
  if (source != PERSIST_LOAD_NONE) {
    File file = fsOpenRead(fs, path);
    if (file) {
      MQTTObserverConfig loaded;
      if (readRecord(file, loaded)) {
        portENTER_CRITICAL(&_config_mux);
        _config = loaded;
        portEXIT_CRITICAL(&_config_mux);
        if (source == PERSIST_LOAD_RECOVERED) _save_state.markDirty();
      }
      file.close();
    }
  }

  if (xTaskCreatePinnedToCore(taskEntry, "mqttObserver", 8192, this, 1, &_task, 0) != pdPASS) {
    _task = nullptr;
    _state.store(STATE_ERROR, std::memory_order_relaxed);
  }
}

void MQTTObserver::taskEntry(void* context) {
  static_cast<MQTTObserver*>(context)->worker();
  vTaskDelete(nullptr);
}

void MQTTObserver::copyConfig(MQTTObserverConfig& config) {
  portENTER_CRITICAL(&_config_mux);
  config = _config;
  portEXIT_CRITICAL(&_config_mux);
}

bool MQTTObserver::ready(const MQTTObserverConfig& config) const {
  return config.enabled && config.wifi_ssid[0] && config.server[0] && config.iata[0] &&
         MQTTObserverConfigCodec::valid(config);
}

bool MQTTObserver::readRecord(File& file, MQTTObserverConfig& config) {
  if (file.size() != MQTTObserverRecord::kRecordSize) return false;
  uint8_t record[MQTTObserverRecord::kRecordSize];
  if (file.read(record, sizeof(record)) != sizeof(record)) return false;
  return MQTTObserverRecord::decode(record, sizeof(record), config);
}

bool MQTTObserver::writeRecord(File& file, void* context) {
  const uint8_t* record = static_cast<const uint8_t*>(context);
  return record && file.write(record, MQTTObserverRecord::kRecordSize) == MQTTObserverRecord::kRecordSize;
}

bool MQTTObserver::validateRecord(File& file, void*) {
  MQTTObserverConfig config;
  return readRecord(file, config);
}

bool MQTTObserver::saveConfig() {
  MQTTObserverConfig config;
  copyConfig(config);
  uint8_t record[MQTTObserverRecord::kRecordSize];
  if (MQTTObserverRecord::encode(config, record, sizeof(record)) != sizeof(record)) {
    _save_state.retryLater();
    return false;
  }
  bool saved = saveStaged(_fs, kConfigPath, writeRecord, record,
                          validateRecord, nullptr, validateRecord, nullptr);
  if (saved) _save_state.clear();
  else _save_state.retryLater();
  return saved;
}

bool MQTTObserver::needsAwake() const {
  MQTTObserverConfig config;
  portENTER_CRITICAL(&_config_mux);
  config = _config;
  portEXIT_CRITICAL(&_config_mux);
  State state = _state.load(std::memory_order_relaxed);
  return ready(config) || state == STATE_WIFI || state == STATE_TIME ||
         state == STATE_CONNECTING || state == STATE_CONNECTED || state == STATE_RETRY;
}

void MQTTObserver::setRadio(float freq, float bw, uint8_t sf, uint8_t cr) {
  portENTER_CRITICAL(&_config_mux);
  _radio_freq = freq;
  _radio_bw = bw;
  _radio_sf = sf;
  _radio_cr = cr;
  portEXIT_CRITICAL(&_config_mux);
}

void MQTTObserver::loop() {
  portENTER_CRITICAL(&_config_mux);
  if (_origin_source && strncmp(_origin, _origin_source, sizeof(_origin)) != 0) {
    strncpy(_origin, _origin_source, sizeof(_origin) - 1);
    _origin[sizeof(_origin) - 1] = 0;
  }
  portEXIT_CRITICAL(&_config_mux);
  if (_save_state.due()) saveConfig();
}

void MQTTObserver::stageRaw(float snr, float rssi, const uint8_t raw[], int len) {
  if (len <= 0) _raw_stager.stage(nullptr, 0, snr, rssi);
  else _raw_stager.stage(raw, static_cast<size_t>(len), snr, rssi);
}

void MQTTObserver::capture(mesh::Packet* packet, float score) {
  if (!packet) return;
  _received.fetch_add(1, std::memory_order_relaxed);

  MQTTObserverRawStager::Snapshot raw;
  bool has_raw = _raw_stager.consume(raw);
  struct timeval now;
  gettimeofday(&now, nullptr);
  if (now.tv_sec < kMinimumValidTime) {
    _publish_drops.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  Event event{};
  event.packet = *packet;
  event.raw_len = has_raw ? raw.length : 0;
  if (event.raw_len) memcpy(event.raw, raw.bytes, event.raw_len);
  event.timestamp = now.tv_sec;
  event.timestamp_usec = now.tv_usec;
  event.snr = has_raw ? raw.snr : packet->getSNR();
  event.rssi = has_raw ? raw.rssi : 0;
  event.score = score;

  portENTER_CRITICAL(&_queue_mux);
  MQTTObserverLiveQueue<Event, kQueueCapacity>::OfferResult result = _queue.offer(_connected, event);
  portEXIT_CRITICAL(&_queue_mux);
  if (result == MQTTObserverLiveQueue<Event, kQueueCapacity>::DROPPED_DISCONNECTED) {
    _disconnected_drops.fetch_add(1, std::memory_order_relaxed);
  } else if (result == MQTTObserverLiveQueue<Event, kQueueCapacity>::DROPPED_OLDEST) {
    _queue_drops.fetch_add(1, std::memory_order_relaxed);
  }
}

void MQTTObserver::clearQueueOnDisconnect() {
  portENTER_CRITICAL(&_queue_mux);
  size_t discarded = _queue.size();
  _queue.clear();
  _connected = false;
  _status_pending = false;
  portEXIT_CRITICAL(&_queue_mux);
  if (discarded) _disconnected_drops.fetch_add(discarded, std::memory_order_relaxed);
}

size_t MQTTObserver::queueSize() {
  portENTER_CRITICAL(&_queue_mux);
  size_t count = _queue.size();
  portEXIT_CRITICAL(&_queue_mux);
  return count;
}

bool MQTTObserver::createJwt(char* token, size_t capacity, uint32_t& expires_at) {
  if (!_identity || !_runtime_config.audience[0] || !token || capacity == 0) return false;
  time_t now = time(nullptr);
  if (now < static_cast<time_t>(kMinimumValidTime)) return false;

  char public_key[2 * PUB_KEY_SIZE + 1];
  bytesToHex(_identity->pub_key, PUB_KEY_SIZE, public_key);
  snprintf(_jwt_username, sizeof(_jwt_username), "v1_%s", public_key);

  const char header_json[] = "{\"alg\":\"Ed25519\",\"typ\":\"JWT\"}";
  char payload_json[256];
  unsigned long issued_at = static_cast<unsigned long>(now);
  unsigned long expiry = issued_at + kJwtLifetimeSeconds;
  int payload_len = snprintf(payload_json, sizeof(payload_json),
      "{\"publicKey\":\"%s\",\"aud\":\"%s\",\"iat\":%lu,\"exp\":%lu}",
      public_key, _runtime_config.audience, issued_at, expiry);
  if (payload_len <= 0 || static_cast<size_t>(payload_len) >= sizeof(payload_json)) return false;

  char encoded_header[64], encoded_payload[384], signing_input[512];
  size_t header_len = 0, encoded_payload_len = 0;
  if (!base64Url(reinterpret_cast<const uint8_t*>(header_json), strlen(header_json),
                 encoded_header, sizeof(encoded_header), header_len) ||
      !base64Url(reinterpret_cast<const uint8_t*>(payload_json), static_cast<size_t>(payload_len),
                 encoded_payload, sizeof(encoded_payload), encoded_payload_len)) return false;
  int signing_len = snprintf(signing_input, sizeof(signing_input), "%s.%s", encoded_header, encoded_payload);
  if (signing_len <= 0 || static_cast<size_t>(signing_len) >= sizeof(signing_input)) return false;

  uint8_t signature[64];
  _identity->sign(signature, reinterpret_cast<const uint8_t*>(signing_input), signing_len);
  char signature_hex[129];
  bytesToHex(signature, sizeof(signature), signature_hex);
  int token_len = snprintf(token, capacity, "%s.%s.%s", encoded_header, encoded_payload, signature_hex);
  if (token_len <= 0 || static_cast<size_t>(token_len) >= capacity) {
    token[0] = 0;
    return false;
  }
  expires_at = expiry;
  return true;
}

bool MQTTObserver::configureClient(const MQTTObserverConfig& config) {
  if (!_client) {
    _client = new (std::nothrow) PsychicMqttClient();
    if (!_client) return false;
    _client->setAutoReconnect(false);
    _client->onConnect([this](bool) {
      portENTER_CRITICAL(&_queue_mux);
      _connected = true;
      _status_pending = true;
      portEXIT_CRITICAL(&_queue_mux);
      _state.store(STATE_CONNECTED, std::memory_order_relaxed);
    });
    _client->onDisconnect([this](bool) {
      clearQueueOnDisconnect();
      _state.store(STATE_RETRY, std::memory_order_relaxed);
    });
    _client->onError([this](esp_mqtt_error_codes_t) {
      _state.store(STATE_ERROR, std::memory_order_relaxed);
    });
  }

  _client->setServer(config.server);
  _client->setClientId(_device_id);
  _client->setKeepAlive(60);
  _client->setBufferSize(1024);
  _client->setCleanSession(true);
  _client->attachArduinoCACertBundle(secureUri(config.server));
  if (config.audience[0]) {
    if (!createJwt(_jwt_token, sizeof(_jwt_token), _jwt_expiry)) return false;
    _client->setCredentials(_jwt_username, _jwt_token);
  } else {
    _jwt_expiry = 0;
    _client->setCredentials(config.username[0] ? config.username : nullptr,
                            config.password[0] ? config.password : nullptr);
  }
  return true;
}

void MQTTObserver::worker() {
  bool have_runtime_config = false;
  bool was_connected = false;
  uint32_t attempt_started = 0;
  uint32_t connected_at = 0;
  uint32_t last_ntp_request = 0;
  char active_ssid[sizeof(_config.wifi_ssid)] = {};
  char active_password[sizeof(_config.wifi_password)] = {};

  while (true) {
    MQTTObserverConfig config;
    copyConfig(config);
    uint32_t now = millis();

    if (!ready(config)) {
      if (_client_started && _client) {
        _client->disconnect();
        _client_started = false;
      }
      clearQueueOnDisconnect();
      if (_wifi_started) {
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
        _wifi_started = false;
      }
      have_runtime_config = false;
      _state.store(config.enabled ? STATE_CONFIG : STATE_OFF, std::memory_order_relaxed);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    if (_wifi_started && (strcmp(active_ssid, config.wifi_ssid) != 0 ||
                          strcmp(active_password, config.wifi_password) != 0)) {
      if (_client_started && _client) {
        _client->disconnect();
        _client_started = false;
      }
      clearQueueOnDisconnect();
      WiFi.disconnect(true, false);
      WiFi.mode(WIFI_OFF);
      _wifi_started = false;
      have_runtime_config = false;
    }

    if (!_wifi_started) {
      WiFi.mode(WIFI_STA);
      WiFi.setAutoReconnect(true);
      WiFi.begin(config.wifi_ssid, config.wifi_password);
      strcpy(active_ssid, config.wifi_ssid);
      strcpy(active_password, config.wifi_password);
      _wifi_started = true;
      last_ntp_request = 0;
      _state.store(STATE_WIFI, std::memory_order_relaxed);
    }

    if (WiFi.status() != WL_CONNECTED) {
      if (_client_started && _client) {
        _client->disconnect();
        _client_started = false;
      }
      clearQueueOnDisconnect();
      was_connected = false;
      _state.store(STATE_WIFI, std::memory_order_relaxed);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    time_t epoch = time(nullptr);
    if (epoch < static_cast<time_t>(kMinimumValidTime)) {
      if (!last_ntp_request || static_cast<uint32_t>(now - last_ntp_request) >= 30000) {
        configTime(0, 0, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
        last_ntp_request = now;
      }
      _state.store(STATE_TIME, std::memory_order_relaxed);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (!have_runtime_config || memcmp(&config, &_runtime_config, sizeof(config)) != 0) {
      if (_client_started && _client) {
        _client->disconnect();
        _client_started = false;
      }
      clearQueueOnDisconnect();
      _runtime_config = config;
      have_runtime_config = true;
      if (!configureClient(_runtime_config)) {
        _state.store(STATE_ERROR, std::memory_order_relaxed);
        vTaskDelay(pdMS_TO_TICKS(1000));
        continue;
      }
      _next_connect_ms = now;
      _backoff_index = 0;
    }

    if (!_client) {
      _state.store(STATE_ERROR, std::memory_order_relaxed);
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    bool connected = _client->connected();
    if (connected) {
      if (!was_connected) connected_at = now;
      was_connected = true;
      _state.store(STATE_CONNECTED, std::memory_order_relaxed);
      if (static_cast<uint32_t>(now - connected_at) >= 120000) _backoff_index = 0;

      epoch = time(nullptr);
      if (_jwt_expiry && epoch + 300 >= _jwt_expiry) {
        _client->disconnect();
        _client_started = false;
        clearQueueOnDisconnect();
        if (configureClient(_runtime_config)) _next_connect_ms = now;
        else _state.store(STATE_ERROR, std::memory_order_relaxed);
        was_connected = false;
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }

      bool send_status = false;
      portENTER_CRITICAL(&_queue_mux);
      send_status = _status_pending;
      _status_pending = false;
      portEXIT_CRITICAL(&_queue_mux);
      if (_runtime_config.status_enabled &&
          (send_status || !_last_status_ms ||
           static_cast<uint32_t>(now - _last_status_ms) >= kStatusIntervalMs)) {
        if (publishStatus()) _last_status_ms = now;
        else _publish_drops.fetch_add(1, std::memory_order_relaxed);
      }

      Event event;
      bool have_event = false;
      portENTER_CRITICAL(&_queue_mux);
      have_event = _queue.pop(event);
      portEXIT_CRITICAL(&_queue_mux);
      if (have_event) publishEvent(event);
      vTaskDelay(pdMS_TO_TICKS(have_event ? 5 : 50));
      continue;
    }

    if (was_connected) {
      was_connected = false;
      clearQueueOnDisconnect();
    }
    if (_client_started && static_cast<uint32_t>(now - attempt_started) >= 30000) {
      _client->disconnect();
      _client_started = false;
      uint8_t index = _backoff_index < 5 ? _backoff_index : 4;
      _next_connect_ms = now + kBackoffMs[index];
      if (_backoff_index < 4) _backoff_index++;
      _state.store(STATE_RETRY, std::memory_order_relaxed);
    }
    if (!_client_started && static_cast<int32_t>(now - _next_connect_ms) >= 0) {
      _client->connect();
      _client_started = true;
      attempt_started = now;
      _state.store(STATE_CONNECTING, std::memory_order_relaxed);
    }
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

bool MQTTObserver::publishStatus() {
  if (!_client || !_client->connected() || !_runtime_config.status_enabled) return false;
  char topic[112];
  if (!makeTopic(_runtime_config, _device_id, "status", topic, sizeof(topic))) return false;
  struct timeval now;
  gettimeofday(&now, nullptr);
  char timestamp[40];
  if (!MQTTObserverFormat::formatTimestamp(now.tv_sec, now.tv_usec, timestamp,
                                           sizeof(timestamp))) return false;
  char radio[64];
  char origin[sizeof(_origin)];
  float freq, bw;
  uint8_t sf, cr;
  portENTER_CRITICAL(&_config_mux);
  memcpy(origin, _origin, sizeof(origin));
  freq = _radio_freq;
  bw = _radio_bw;
  sf = _radio_sf;
  cr = _radio_cr;
  portEXIT_CRITICAL(&_config_mux);
  snprintf(radio, sizeof(radio), "%.3f,%.1f,%u,%u", static_cast<double>(freq),
           static_cast<double>(bw), static_cast<unsigned>(sf), static_cast<unsigned>(cr));
  size_t written = 0;
  if (!MQTTObserverFormat::buildStatus(origin, _device_id,
          "MeshCore Repeater", FIRMWARE_VERSION, radio, timestamp,
          _json, sizeof(_json), written)) return false;
  return _client->publish(topic, 1, false, _json, written, true) >= 0;
}

bool MQTTObserver::publishEvent(const Event& event) {
  if (!_client || !_client->connected()) {
    _disconnected_drops.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  bool published = false;
  char origin[sizeof(_origin)];
  portENTER_CRITICAL(&_config_mux);
  memcpy(origin, _origin, sizeof(origin));
  portEXIT_CRITICAL(&_config_mux);
  char topic[112];
  size_t written = 0;

  // Synchronous QoS 0 publishes bypass the client outbox, so packet data cannot replay later.
  if (_runtime_config.packets_enabled) {
    uint8_t serialized[MAX_TRANS_UNIT];
    const uint8_t* wire = event.raw;
    size_t wire_len = event.raw_len;
    if (!wire_len) {
      int expected = event.packet.getRawLength();
      if (expected <= 0 || expected > MAX_TRANS_UNIT || event.packet.payload_len > MAX_PACKET_PAYLOAD) {
        _publish_drops.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      wire_len = event.packet.writeTo(serialized);
      if (!wire_len || wire_len > sizeof(serialized)) {
        _publish_drops.fetch_add(1, std::memory_order_relaxed);
        return false;
      }
      wire = serialized;
    }
    uint8_t hash[MAX_HASH_SIZE];
    event.packet.calculatePacketHash(hash);
    MQTTObserverFormat::PacketView view = {
      origin, _device_id, event.timestamp, event.timestamp_usec,
      wire, wire_len, event.packet.getPayloadType(),
      event.packet.isRouteDirect() ? "D" : "F", event.packet.payload_len,
      event.snr, event.rssi, event.score, hash, sizeof(hash), event.packet.path,
      event.packet.isRouteDirect() ? event.packet.getPathHashCount() : 0,
      event.packet.getPathHashSize()
    };
    if (makeTopic(_runtime_config, _device_id, "packets", topic, sizeof(topic)) &&
        MQTTObserverFormat::buildPacket(view, _json, sizeof(_json), written)) {
      int result = _client->publish(topic, 0, false, _json, written, false);
      if (result >= 0) {
        _published.fetch_add(1, std::memory_order_relaxed);
        published = true;
      } else {
        _publish_drops.fetch_add(1, std::memory_order_relaxed);
      }
    } else {
      _publish_drops.fetch_add(1, std::memory_order_relaxed);
    }
  }

  if (_runtime_config.raw_enabled && event.raw_len) {
    char timestamp[40];
    if (MQTTObserverFormat::formatTimestamp(event.timestamp, event.timestamp_usec,
                                             timestamp, sizeof(timestamp)) &&
        makeTopic(_runtime_config, _device_id, "raw", topic, sizeof(topic)) &&
        MQTTObserverFormat::buildRaw(origin, _device_id, timestamp,
                                     event.raw, event.raw_len, _json, sizeof(_json), written)) {
      int result = _client->publish(topic, 0, false, _json, written, false);
      if (result >= 0) {
        _published.fetch_add(1, std::memory_order_relaxed);
        published = true;
      } else {
        _publish_drops.fetch_add(1, std::memory_order_relaxed);
      }
    } else {
      _publish_drops.fetch_add(1, std::memory_order_relaxed);
    }
  }
  return published;
}

void MQTTObserver::markDirty() {
  _save_state.markDirty();
}

bool MQTTObserver::setString(char* destination, size_t capacity, const char* value) {
  if (!destination || !value || strlen(value) >= capacity) return false;
  memset(destination, 0, capacity);
  memcpy(destination, value, strlen(value));
  return true;
}

bool MQTTObserver::handleCommand(char* command, char* reply) {
  if (!command || !reply) return false;
  if (handleGetCommand(command, reply)) return true;
  if (startsWith(command, "set mqtt") || startsWith(command, "set wifi"))
    return handleSetCommand(command, reply);
  return false;
}

bool MQTTObserver::handleSetCommand(char* command, char* reply) {
  bool changed = false;
  uint8_t value = 0;
  if (strcmp(command, "set mqtt on") == 0 || strcmp(command, "set mqtt off") == 0) {
    value = command[9] == 'o' && command[10] == 'n';
    portENTER_CRITICAL(&_config_mux);
    changed = _config.enabled != value;
    _config.enabled = value;
    portEXIT_CRITICAL(&_config_mux);
    if (changed) markDirty();
    snprintf(reply, 160, value ? "OK - MQTT enabled" : "OK - MQTT disabled");
    return true;
  }

  const char* text = commandValue(command, "set mqtt.server");
  if (text) {
    if (*text && !MQTTObserverConfigCodec::validServer(text)) {
      snprintf(reply, 160, "Err - expected mqtt(s):// or ws(s):// URL");
      return true;
    }
    portENTER_CRITICAL(&_config_mux);
    changed = strcmp(_config.server, text) != 0 && setString(_config.server, sizeof(_config.server), text);
    portEXIT_CRITICAL(&_config_mux);
    if (strlen(text) >= sizeof(_config.server)) snprintf(reply, 160, "Err - server URL too long");
    else {
      if (changed) markDirty();
      snprintf(reply, 160, "OK - MQTT server saved");
    }
    return true;
  }

  text = commandValue(command, "set mqtt.audience");
  if (text) {
    if (!MQTTObserverConfigCodec::validAudience(text) || strlen(text) >= sizeof(_config.audience)) {
      snprintf(reply, 160, "Err - invalid MQTT audience");
      return true;
    }
    portENTER_CRITICAL(&_config_mux);
    changed = strcmp(_config.audience, text) != 0 &&
              setString(_config.audience, sizeof(_config.audience), text);
    portEXIT_CRITICAL(&_config_mux);
    if (changed) markDirty();
    snprintf(reply, 160, "OK - MQTT audience saved");
    return true;
  }

  text = commandValue(command, "set mqtt.iata");
  if (text) {
    if (!MQTTObserverConfigCodec::validIata(text)) {
      snprintf(reply, 160, "Err - IATA must be 3 uppercase letters");
      return true;
    }
    portENTER_CRITICAL(&_config_mux);
    changed = strcmp(_config.iata, text) != 0 && setString(_config.iata, sizeof(_config.iata), text);
    portEXIT_CRITICAL(&_config_mux);
    if (changed) markDirty();
    snprintf(reply, 160, "OK - MQTT IATA saved");
    return true;
  }

  struct ToggleField { const char* command; size_t offset; };
  const ToggleField toggles[] = {
    {"set mqtt.status", offsetof(MQTTObserverConfig, status_enabled)},
    {"set mqtt.packets", offsetof(MQTTObserverConfig, packets_enabled)},
    {"set mqtt.raw", offsetof(MQTTObserverConfig, raw_enabled)}
  };
  for (size_t i = 0; i < sizeof(toggles) / sizeof(toggles[0]); i++) {
    text = commandValue(command, toggles[i].command);
    if (!text) continue;
    if (!parseToggle(text, value)) {
      snprintf(reply, 160, "Err - expected on or off");
      return true;
    }
    portENTER_CRITICAL(&_config_mux);
    uint8_t* field = reinterpret_cast<uint8_t*>(&_config) + toggles[i].offset;
    changed = *field != value;
    *field = value;
    portEXIT_CRITICAL(&_config_mux);
    if (changed) markDirty();
    snprintf(reply, 160, "OK - MQTT setting saved");
    return true;
  }

  struct StringField { const char* command; size_t offset; size_t capacity; bool secret; };
  const StringField strings[] = {
    {"set wifi.ssid", offsetof(MQTTObserverConfig, wifi_ssid), sizeof(_config.wifi_ssid), false},
    {"set wifi.pwd", offsetof(MQTTObserverConfig, wifi_password), sizeof(_config.wifi_password), true},
    {"set mqtt.username", offsetof(MQTTObserverConfig, username), sizeof(_config.username), true},
    {"set mqtt.password", offsetof(MQTTObserverConfig, password), sizeof(_config.password), true}
  };
  for (size_t i = 0; i < sizeof(strings) / sizeof(strings[0]); i++) {
    text = commandValue(command, strings[i].command);
    if (!text) continue;
    if (strlen(text) >= strings[i].capacity) {
      snprintf(reply, 160, "Err - setting too long");
      return true;
    }
    portENTER_CRITICAL(&_config_mux);
    char* field = reinterpret_cast<char*>(&_config) + strings[i].offset;
    changed = strcmp(field, text) != 0;
    if (changed) setString(field, strings[i].capacity, text);
    portEXIT_CRITICAL(&_config_mux);
    if (changed) markDirty();
    snprintf(reply, 160, strings[i].secret ? "OK - credential saved" : "OK - WiFi SSID saved");
    return true;
  }

  snprintf(reply, 160, "Err - unsupported MQTT/WiFi setting");
  return true;
}

bool MQTTObserver::handleGetCommand(const char* command, char* reply) {
  if (strcmp(command, "get mqtt.status") == 0) {
    formatStatus(reply, 160);
    return true;
  }
  if (strcmp(command, "get wifi.status") == 0) {
    snprintf(reply, 160, "wifi %s; ssid %s",
             WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
             WiFi.status() == WL_CONNECTED ? "configured" : "not connected");
    return true;
  }

  MQTTObserverConfig config;
  if (strcmp(command, "get mqtt.server") == 0 || strcmp(command, "get mqtt.iata") == 0 ||
      strcmp(command, "get mqtt.audience") == 0 || strcmp(command, "get mqtt.enabled") == 0 ||
      strcmp(command, "get mqtt.status.enabled") == 0 || strcmp(command, "get mqtt.packets") == 0 ||
      strcmp(command, "get mqtt.raw") == 0 || strcmp(command, "get mqtt.password") == 0 ||
      strcmp(command, "get mqtt.username") == 0 || strcmp(command, "get wifi.ssid") == 0 ||
      strcmp(command, "get wifi.pwd") == 0) {
    copyConfig(config);
    if (strcmp(command, "get mqtt.server") == 0) {
      char host[64];
      MQTTObserverConfigCodec::hostLabel(config.server, host, sizeof(host));
      snprintf(reply, 160, "%s", host[0] ? host : "not configured");
    } else if (strcmp(command, "get mqtt.iata") == 0) {
      snprintf(reply, 160, "%s", config.iata[0] ? config.iata : "not configured");
    } else if (strcmp(command, "get mqtt.audience") == 0) {
      snprintf(reply, 160, "%s", config.audience[0] ? config.audience : "not configured");
    } else if (strcmp(command, "get mqtt.enabled") == 0) {
      snprintf(reply, 160, "%s", config.enabled ? "on" : "off");
    } else if (strcmp(command, "get mqtt.status.enabled") == 0) {
      snprintf(reply, 160, "%s", config.status_enabled ? "on" : "off");
    } else if (strcmp(command, "get mqtt.packets") == 0) {
      snprintf(reply, 160, "%s", config.packets_enabled ? "on" : "off");
    } else if (strcmp(command, "get mqtt.raw") == 0) {
      snprintf(reply, 160, "%s", config.raw_enabled ? "on" : "off");
    } else if (strcmp(command, "get mqtt.password") == 0) {
      snprintf(reply, 160, "%s", config.password[0] ? "configured" : "not set");
    } else if (strcmp(command, "get mqtt.username") == 0) {
      snprintf(reply, 160, "%s", config.username[0] ? "configured" : "not set");
    } else if (strcmp(command, "get wifi.ssid") == 0) {
      snprintf(reply, 160, "%s", config.wifi_ssid[0] ? "configured" : "not set");
    } else {
      snprintf(reply, 160, "%s", config.wifi_password[0] ? "configured" : "not set");
    }
    return true;
  }
  return false;
}

const char* MQTTObserver::stateName() const {
  switch (_state.load(std::memory_order_relaxed)) {
    case STATE_OFF: return "off";
    case STATE_CONFIG: return "config";
    case STATE_WIFI: return "wifi";
    case STATE_TIME: return "time";
    case STATE_CONNECTING: return "connecting";
    case STATE_CONNECTED: return "connected";
    case STATE_RETRY: return "retry";
    default: return "error";
  }
}

void MQTTObserver::formatStatus(char* reply, size_t capacity) {
  if (!reply || capacity == 0) return;
  MQTTObserverConfig config;
  copyConfig(config);
  char host[24];
  MQTTObserverConfigCodec::hostLabel(config.server, host, sizeof(host));
  if (!host[0]) strcpy(host, "none");
  const char* wifi = WiFi.status() == WL_CONNECTED ? "up" : "down";
  time_t epoch = time(nullptr);
  const char* time_state = epoch >= static_cast<time_t>(kMinimumValidTime) ? "ok" : "wait";
  const char* jwt = config.audience[0] ? time_state : "n/a";
  uint32_t drops = _disconnected_drops.load(std::memory_order_relaxed) +
                   _queue_drops.load(std::memory_order_relaxed) +
                   _publish_drops.load(std::memory_order_relaxed);
  snprintf(reply, capacity,
      "mqtt %s wifi:%s %s time:%s jwt:%s %s rx:%lu pub:%lu drop:%lu q:%u heap:%u/%u",
      config.enabled ? "on" : "off", wifi, host, time_state, jwt, stateName(),
      static_cast<unsigned long>(_received.load(std::memory_order_relaxed)),
      static_cast<unsigned long>(_published.load(std::memory_order_relaxed)),
      static_cast<unsigned long>(drops), static_cast<unsigned>(queueSize()),
      static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

#endif  // WITH_MQTT_OBSERVER
