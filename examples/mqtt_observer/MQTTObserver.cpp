#ifdef WITH_MQTT_OBSERVER

#include "MQTTObserver.h"
#include "MQTTObserverFormat.h"
#include "MQTTObserverRecord.h"
#include <WiFi.h>
#include <esp_wifi.h>
#include <mbedtls/base64.h>
#include <mbedtls/platform.h>
#if defined(BOARD_HAS_PSRAM)
#include <esp_heap_caps.h>
#endif
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
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

bool parseUnsigned(const char* value, unsigned min, unsigned max, unsigned& parsed) {
  if (!value || !*value) return false;
  unsigned result = 0;
  for (const char* p = value; *p; p++) {
    if (*p < '0' || *p > '9') return false;
    unsigned digit = static_cast<unsigned>(*p - '0');
    if (result > (max - digit) / 10) return false;
    result = result * 10 + digit;
  }
  if (result < min) return false;
  parsed = result;
  return true;
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

bool jsonEscape(const char* input, char* output, size_t capacity) {
  if (!input || !output || capacity == 0) return false;
  size_t used = 0;
  static const char hex[] = "0123456789abcdef";
  for (const unsigned char* p = reinterpret_cast<const unsigned char*>(input); *p; p++) {
    char escaped[6];
    const char* value = nullptr;
    size_t len = 0;
    if (*p == '"' || *p == '\\') {
      escaped[0] = '\\';
      escaped[1] = static_cast<char>(*p);
      value = escaped;
      len = 2;
    } else if (*p < 0x20) {
      escaped[0] = '\\'; escaped[1] = 'u'; escaped[2] = '0'; escaped[3] = '0';
      escaped[4] = hex[*p >> 4]; escaped[5] = hex[*p & 0x0f];
      value = escaped;
      len = 6;
    } else {
      escaped[0] = static_cast<char>(*p);
      value = escaped;
      len = 1;
    }
    if (used + len >= capacity) return false;
    memcpy(output + used, value, len);
    used += len;
  }
  output[used] = 0;
  return true;
}

void* observerMbedtlsCalloc(size_t count, size_t size) {
#if defined(BOARD_HAS_PSRAM)
  void* memory = heap_caps_calloc(count, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (memory) return memory;
#endif
  return calloc(count, size);
}

void observerMbedtlsFree(void* memory) {
  free(memory);
}

bool secureUri(const char* uri) {
  return strncmp(uri, "mqtts://", 8) == 0 || strncmp(uri, "wss://", 6) == 0;
}

bool sameConnectionConfig(const MQTTObserverConfig& a, const MQTTObserverConfig& b) {
  // Publish-only settings update live without tearing down a healthy TLS session.
  return strcmp(a.wifi_ssid, b.wifi_ssid) == 0 &&
         strcmp(a.wifi_password, b.wifi_password) == 0 &&
         strcmp(a.server, b.server) == 0 && a.port == b.port &&
         strcmp(a.username, b.username) == 0 && strcmp(a.password, b.password) == 0 &&
         strcmp(a.audience, b.audience) == 0 && strcmp(a.owner, b.owner) == 0 &&
         strcmp(a.email, b.email) == 0;
}

}  // namespace

MQTTObserver::MQTTObserver()
    : _fs(nullptr), _identity(nullptr), _origin_source(nullptr), _origin{},
      _radio_freq(0), _radio_bw(0), _radio_sf(0), _radio_cr(0), _config{}, _save_record{},
      _config_mux(portMUX_INITIALIZER_UNLOCKED), _queue_mux(portMUX_INITIALIZER_UNLOCKED),
      _queue(), _raw_stager(), _client(nullptr), _task(nullptr), _runtime_config{},
      _device_id{}, _server_uri{}, _jwt_username{}, _jwt_token{}, _json{}, _connected(false),
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
#if defined(BOARD_HAS_PSRAM)
  if (psramFound()) mbedtls_platform_set_calloc_free(observerMbedtlsCalloc, observerMbedtlsFree);
#endif
  _radio_freq = freq;
  _radio_bw = bw;
  _radio_sf = sf;
  _radio_cr = cr;
  strncpy(_origin, origin ? origin : "", sizeof(_origin) - 1);
  _origin[sizeof(_origin) - 1] = 0;
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
        if (source == PERSIST_LOAD_RECOVERED || file.size() == MQTTObserverRecord::kLegacyV1RecordSize)
          _save_state.markDirty();
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
  bool needs_iata = !config.topic[0] || strstr(config.topic, "{iata}") != nullptr;
  return config.enabled && config.wifi_ssid[0] && config.server[0] &&
         (!needs_iata || (config.iata[0] && strcmp(config.iata, "XXX") != 0)) &&
         MQTTObserverConfigCodec::valid(config);
}

bool MQTTObserver::readRecord(File& file, MQTTObserverConfig& config) {
  size_t size = file.size();
  if (size != MQTTObserverRecord::kRecordSize &&
      size != MQTTObserverRecord::kLegacyV1RecordSize) return false;
  uint8_t record[MQTTObserverRecord::kRecordSize];
  if (file.read(record, size) != size) return false;
  return MQTTObserverRecord::decode(record, size, config);
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
  if (MQTTObserverRecord::encode(config, _save_record, sizeof(_save_record)) !=
      sizeof(_save_record)) {
    _save_state.retryLater();
    return false;
  }
  bool saved = saveStaged(_fs, kConfigPath, writeRecord, _save_record,
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
  MQTTObserverConfig config;
  copyConfig(config);
  const char* origin = config.origin[0] ? config.origin : (_origin_source ? _origin_source : "");
  portENTER_CRITICAL(&_config_mux);
  if (strncmp(_origin, origin, sizeof(_origin)) != 0) {
    strncpy(_origin, origin, sizeof(_origin) - 1);
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
  MQTTObserverConfig config;
  copyConfig(config);
  if (!config.rx_enabled || (!config.packets_enabled && !config.raw_enabled)) return;

  Event event{};
  bool has_raw = _raw_stager.consume(event.raw, sizeof(event.raw), event.raw_len,
                                     event.snr, event.rssi);
  struct timeval now;
  gettimeofday(&now, nullptr);
  if (now.tv_sec < kMinimumValidTime) {
    _publish_drops.fetch_add(1, std::memory_order_relaxed);
    return;
  }

  event.packet = *packet;
  if (!has_raw) {
    event.raw_len = 0;
    event.snr = packet->getSNR();
    event.rssi = 0;
  }
  event.timestamp = now.tv_sec;
  event.timestamp_usec = now.tv_usec;
  event.received_ms = millis();
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
  unsigned long issued_at = static_cast<unsigned long>(now);
  unsigned long expiry = issued_at + kJwtLifetimeSeconds;
  char owner[sizeof(_runtime_config.owner)];
  strncpy(owner, _runtime_config.owner, sizeof(owner) - 1);
  owner[sizeof(owner) - 1] = 0;
  for (char* p = owner; *p; p++) if (*p >= 'a' && *p <= 'f') *p -= 'a' - 'A';
  char client_version[96], escaped_client[193], escaped_email[129];
  snprintf(client_version, sizeof(client_version), "meshcore-jhuebert/%s", FIRMWARE_VERSION);
  if (!jsonEscape(client_version, escaped_client, sizeof(escaped_client)) ||
      !jsonEscape(_runtime_config.email, escaped_email, sizeof(escaped_email))) return false;
  char owner_field[80] = {}, email_field[160] = {};
  if (owner[0] && snprintf(owner_field, sizeof(owner_field), ",\"owner\":\"%s\"", owner) >=
                      static_cast<int>(sizeof(owner_field))) return false;
  if (escaped_email[0] && snprintf(email_field, sizeof(email_field), ",\"email\":\"%s\"",
                                   escaped_email) >= static_cast<int>(sizeof(email_field))) return false;
  int payload_len = snprintf(_json, sizeof(_json),
      "{\"publicKey\":\"%s\",\"aud\":\"%s\",\"iat\":%lu,\"exp\":%lu,\"client\":\"%s\"%s%s}",
      public_key, _runtime_config.audience, issued_at, expiry, escaped_client,
      owner_field, email_field);
  if (payload_len <= 0 || static_cast<size_t>(payload_len) >= sizeof(_json)) return false;

  char encoded_header[64];
  size_t header_len = 0, encoded_payload_len = 0;
  if (!base64Url(reinterpret_cast<const uint8_t*>(header_json), strlen(header_json),
                 encoded_header, sizeof(encoded_header), header_len) ||
      !base64Url(reinterpret_cast<const uint8_t*>(_json), static_cast<size_t>(payload_len),
                 reinterpret_cast<char*>(token), capacity, encoded_payload_len)) return false;
  int signing_len = snprintf(_json, sizeof(_json), "%s.%s", encoded_header, token);
  if (signing_len <= 0 || static_cast<size_t>(signing_len) >= sizeof(_json)) return false;

  uint8_t signature[64];
  _identity->sign(signature, reinterpret_cast<const uint8_t*>(_json), signing_len);
  char signature_hex[129];
  bytesToHex(signature, sizeof(signature), signature_hex);
  size_t token_len = header_len + 1 + encoded_payload_len + 1 + sizeof(signature_hex) - 1;
  if (token_len >= capacity) {
    token[0] = 0;
    return false;
  }
  memmove(token + header_len + 1, token, encoded_payload_len);
  memcpy(token, encoded_header, header_len);
  token[header_len] = '.';
  memcpy(token + header_len + 1 + encoded_payload_len, ".", 1);
  memcpy(token + header_len + 2 + encoded_payload_len, signature_hex, sizeof(signature_hex));
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

  if (!MQTTObserverConfigCodec::buildServerUri(config, _server_uri, sizeof(_server_uri))) return false;
  _client->setServer(_server_uri);
  _client->setClientId(_device_id);
  _client->setKeepAlive(60);
  esp_mqtt_client_config_t* mqtt_config = _client->getMqttConfig();
  if (mqtt_config) mqtt_config->network_timeout_ms = 2500;
  _client->setBufferSize(1280);
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
  if (mqtt_config) mqtt_config->buffer.out_size = 1280;
#endif
  _client->setCleanSession(true);
  _client->attachArduinoCACertBundle(secureUri(_server_uri));
  if (mqtt_config) {
#if ESP_IDF_VERSION_MAJOR == 5
    mqtt_config->credentials.username = nullptr;
    mqtt_config->credentials.authentication.password = nullptr;
#else
    mqtt_config->username = nullptr;
    mqtt_config->password = nullptr;
#endif
  }
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
  char active_ntp_server[sizeof(_config.ntp_server)] = {};
  uint8_t active_wifi_power_save = 0xff;

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
      active_wifi_power_save = 0xff;
      active_ntp_server[0] = 0;
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

    if (active_wifi_power_save != config.wifi_power_save) {
      wifi_ps_type_t power_save = config.wifi_power_save == 1 ? WIFI_PS_NONE :
                                  config.wifi_power_save == 2 ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM;
      if (esp_wifi_set_ps(power_save) == ESP_OK) active_wifi_power_save = config.wifi_power_save;
    }

    const char* ntp_server = config.ntp_server[0] ? config.ntp_server : "pool.ntp.org";
    if (strcmp(active_ntp_server, ntp_server) != 0) {
      strncpy(active_ntp_server, ntp_server, sizeof(active_ntp_server) - 1);
      active_ntp_server[sizeof(active_ntp_server) - 1] = 0;
      configTime(0, 0, active_ntp_server, "time.google.com", "time.cloudflare.com");
      last_ntp_request = now;
    }

    time_t epoch = time(nullptr);
    if (epoch < static_cast<time_t>(kMinimumValidTime)) {
      if (!last_ntp_request || static_cast<uint32_t>(now - last_ntp_request) >= 30000) {
        configTime(0, 0, config.ntp_server[0] ? config.ntp_server : "pool.ntp.org",
                   "time.google.com", "time.cloudflare.com");
        last_ntp_request = now;
      }
      _state.store(STATE_TIME, std::memory_order_relaxed);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (!have_runtime_config || !sameConnectionConfig(config, _runtime_config)) {
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
    } else {
      _runtime_config = config;
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
           static_cast<uint32_t>(now - _last_status_ms) >=
               static_cast<uint32_t>(_runtime_config.status_interval_minutes) * 60000UL)) {
        if (publishStatus()) _last_status_ms = now;
        else _publish_drops.fetch_add(1, std::memory_order_relaxed);
      }

      Event event;
      bool have_event = false;
      portENTER_CRITICAL(&_queue_mux);
      have_event = _queue.pop(event);
      portEXIT_CRITICAL(&_queue_mux);
      if (have_event) {
        if (MQTTObserverQueuePolicy::eventExpired(millis(), event.received_ms))
          _publish_drops.fetch_add(1, std::memory_order_relaxed);
        else
          publishEvent(event);
      }
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
  char topic[128];
  if (!MQTTObserverConfigCodec::buildTopic(_runtime_config, _device_id, "status", topic, sizeof(topic))) return false;
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
  char topic[128];
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
    if (MQTTObserverConfigCodec::buildTopic(_runtime_config, _device_id, "packets", topic, sizeof(topic)) &&
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
        MQTTObserverConfigCodec::buildTopic(_runtime_config, _device_id, "raw", topic, sizeof(topic)) &&
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
      snprintf(reply, 160, "Err - expected hostname or mqtt(s)/ws(s) URL");
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

  text = commandValue(command, "set mqtt.port");
  if (text) {
    unsigned port;
    if (!parseUnsigned(text, 1, 65535, port)) {
      snprintf(reply, 160, "Err - port must be 1-65535");
      return true;
    }
    portENTER_CRITICAL(&_config_mux);
    changed = _config.port != port;
    _config.port = static_cast<uint16_t>(port);
    portEXIT_CRITICAL(&_config_mux);
    if (changed) markDirty();
    snprintf(reply, 160, "OK - MQTT port saved");
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
    char iata[sizeof(_config.iata)];
    if (strlen(text) >= sizeof(iata)) {
      snprintf(reply, 160, "Err - IATA must be 3 letters/digits; XXX is reserved");
      return true;
    }
    for (size_t i = 0; i <= strlen(text); i++)
      iata[i] = (text[i] >= 'a' && text[i] <= 'z') ? text[i] - 'a' + 'A' : text[i];
    if (!MQTTObserverConfigCodec::validIata(iata)) {
      snprintf(reply, 160, "Err - IATA must be 3 letters/digits; XXX is reserved");
      return true;
    }
    portENTER_CRITICAL(&_config_mux);
    changed = strcmp(_config.iata, iata) != 0 && setString(_config.iata, sizeof(_config.iata), iata);
    portEXIT_CRITICAL(&_config_mux);
    if (changed) markDirty();
    snprintf(reply, 160, "OK - MQTT IATA saved");
    return true;
  }

  text = commandValue(command, "set mqtt.interval");
  if (text) {
    unsigned minutes;
    if (!parseUnsigned(text, 1, 60, minutes)) {
      snprintf(reply, 160, "Err - interval must be 1-60 minutes");
      return true;
    }
    portENTER_CRITICAL(&_config_mux);
    changed = _config.status_interval_minutes != minutes;
    _config.status_interval_minutes = static_cast<uint8_t>(minutes);
    portEXIT_CRITICAL(&_config_mux);
    if (changed) markDirty();
    snprintf(reply, 160, "OK - MQTT interval saved");
    return true;
  }

  text = commandValue(command, "set wifi.powersave");
  if (text) {
    uint8_t power_save;
    if (strcmp(text, "min") == 0) power_save = 0;
    else if (strcmp(text, "none") == 0) power_save = 1;
    else if (strcmp(text, "max") == 0) power_save = 2;
    else {
      snprintf(reply, 160, "Err - expected none, min, or max");
      return true;
    }
    portENTER_CRITICAL(&_config_mux);
    changed = _config.wifi_power_save != power_save;
    _config.wifi_power_save = power_save;
    portEXIT_CRITICAL(&_config_mux);
    if (changed) markDirty();
    snprintf(reply, 160, "OK - WiFi power save saved");
    return true;
  }

  struct StringSetting { const char* command; size_t offset; size_t capacity; bool secret; };
  const StringSetting settings[] = {
    {"set mqtt.origin", offsetof(MQTTObserverConfig, origin), sizeof(_config.origin), false},
    {"set mqtt.topic", offsetof(MQTTObserverConfig, topic), sizeof(_config.topic), false},
    {"set mqtt.token", offsetof(MQTTObserverConfig, token), sizeof(_config.token), true},
    {"set mqtt.ntp", offsetof(MQTTObserverConfig, ntp_server), sizeof(_config.ntp_server), false},
    {"set mqtt.owner", offsetof(MQTTObserverConfig, owner), sizeof(_config.owner), false},
    {"set mqtt.email", offsetof(MQTTObserverConfig, email), sizeof(_config.email), false}
  };
  for (size_t i = 0; i < sizeof(settings) / sizeof(settings[0]); i++) {
    text = commandValue(command, settings[i].command);
    if (!text) continue;
    const char* value = (i == 3 && strcmp(text, "none") == 0) ? "" : text;
    if (((i == 1 || i == 2) && !MQTTObserverConfigCodec::validTopic(value)) ||
        (i == 3 && !MQTTObserverConfigCodec::validNtpServer(value)) ||
        (i == 4 && !MQTTObserverConfigCodec::validOwner(value)) ||
        (i == 5 && !MQTTObserverConfigCodec::validEmail(value)) ||
        strlen(value) >= settings[i].capacity) {
      snprintf(reply, 160, "Err - invalid or oversized setting");
      return true;
    }
    portENTER_CRITICAL(&_config_mux);
    char* field = reinterpret_cast<char*>(&_config) + settings[i].offset;
    changed = strcmp(field, value) != 0;
    if (changed) setString(field, settings[i].capacity, value);
    portEXIT_CRITICAL(&_config_mux);
    if (changed) markDirty();
    snprintf(reply, 160, settings[i].secret ? "OK - credential saved" : "OK - MQTT setting saved");
    return true;
  }

  struct ToggleField { const char* command; size_t offset; };
  const ToggleField toggles[] = {
    {"set mqtt.status", offsetof(MQTTObserverConfig, status_enabled)},
    {"set mqtt.packets", offsetof(MQTTObserverConfig, packets_enabled)},
    {"set mqtt.raw", offsetof(MQTTObserverConfig, raw_enabled)},
    {"set mqtt.rx", offsetof(MQTTObserverConfig, rx_enabled)}
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
    IPAddress ip = WiFi.localIP();
    snprintf(reply, 160, "wifi %s ip:%u.%u.%u.%u rssi:%d",
             WiFi.status() == WL_CONNECTED ? "connected" : "disconnected",
             static_cast<unsigned>(ip[0]), static_cast<unsigned>(ip[1]),
             static_cast<unsigned>(ip[2]), static_cast<unsigned>(ip[3]),
             WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
    return true;
  }

  MQTTObserverConfig config;
  copyConfig(config);
  if (strcmp(command, "get mqtt.server") == 0) {
    char host[64];
    MQTTObserverConfigCodec::hostLabel(config.server, host, sizeof(host));
    snprintf(reply, 160, "%s", host[0] ? host : "not configured");
  } else if (strcmp(command, "get mqtt.port") == 0) {
    snprintf(reply, 160, "%u", static_cast<unsigned>(MQTTObserverConfigCodec::configuredPort(config)));
  } else if (strcmp(command, "get mqtt.iata") == 0) {
    snprintf(reply, 160, "%s", config.iata[0] ? config.iata : "not configured");
  } else if (strcmp(command, "get mqtt.origin") == 0) {
    const char* origin = config.origin[0] ? config.origin : (_origin_source ? _origin_source : "");
    snprintf(reply, 160, "%s", origin[0] ? origin : "not configured");
  } else if (strcmp(command, "get mqtt.audience") == 0) {
    snprintf(reply, 160, "%s", config.audience[0] ? config.audience : "not configured");
  } else if (strcmp(command, "get mqtt.topic") == 0) {
    snprintf(reply, 160, "%s", config.topic[0] ? config.topic : "meshcore/{iata}/{device}/{type}");
  } else if (strcmp(command, "get mqtt.ntp") == 0) {
    snprintf(reply, 160, "%s", config.ntp_server[0] ? config.ntp_server : "pool.ntp.org");
  } else if (strcmp(command, "get mqtt.owner") == 0) {
    snprintf(reply, 160, "%s", config.owner[0] ? config.owner : "not configured");
  } else if (strcmp(command, "get mqtt.email") == 0) {
    snprintf(reply, 160, "%s", config.email[0] ? config.email : "not configured");
  } else if (strcmp(command, "get mqtt.interval") == 0) {
    snprintf(reply, 160, "%u", static_cast<unsigned>(config.status_interval_minutes));
  } else if (strcmp(command, "get mqtt.enabled") == 0) {
    snprintf(reply, 160, "%s", config.enabled ? "on" : "off");
  } else if (strcmp(command, "get mqtt.rx") == 0) {
    snprintf(reply, 160, "%s", config.rx_enabled ? "on" : "off");
  } else if (strcmp(command, "get mqtt.status.enabled") == 0) {
    snprintf(reply, 160, "%s", config.status_enabled ? "on" : "off");
  } else if (strcmp(command, "get mqtt.packets") == 0) {
    snprintf(reply, 160, "%s", config.packets_enabled ? "on" : "off");
  } else if (strcmp(command, "get mqtt.raw") == 0) {
    snprintf(reply, 160, "%s", config.raw_enabled ? "on" : "off");
  } else if (strcmp(command, "get wifi.powersave") == 0) {
    const char* value = config.wifi_power_save == 1 ? "none" :
                        config.wifi_power_save == 2 ? "max" : "min";
    snprintf(reply, 160, "%s", value);
  } else if (strcmp(command, "get mqtt.password") == 0) {
    snprintf(reply, 160, "%s", config.password[0] ? "configured" : "not set");
  } else if (strcmp(command, "get mqtt.username") == 0) {
    snprintf(reply, 160, "%s", config.username[0] ? "configured" : "not set");
  } else if (strcmp(command, "get mqtt.token") == 0) {
    snprintf(reply, 160, "%s", config.token[0] ? "configured" : "not set");
  } else if (strcmp(command, "get wifi.ssid") == 0) {
    snprintf(reply, 160, "%s", config.wifi_ssid[0] ? config.wifi_ssid : "not set");
  } else if (strcmp(command, "get wifi.pwd") == 0) {
    snprintf(reply, 160, "%s", config.wifi_password[0] ? "configured" : "not set");
  } else {
    return false;
  }
  return true;
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
