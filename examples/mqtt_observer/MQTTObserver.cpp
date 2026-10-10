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

bool base64Url(const uint8_t* input, size_t input_len, char* output, size_t capacity,
               size_t& written) {
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

bool sameBrokerConnection(const MQTTObserverBrokerConfig& a,
                          const MQTTObserverBrokerConfig& b) {
  return a.enabled == b.enabled && a.port == b.port &&
         strcmp(a.server, b.server) == 0 && strcmp(a.username, b.username) == 0 &&
         strcmp(a.password, b.password) == 0 && strcmp(a.audience, b.audience) == 0 &&
         strcmp(a.owner, b.owner) == 0 && strcmp(a.email, b.email) == 0;
}

}  // namespace

MQTTObserver::BrokerRuntime::BrokerRuntime()
    : client(nullptr), server_uri{}, username{}, password{}, jwt_username{}, jwt_token{}, jwt_expiry(0),
      next_connect_ms(0), attempt_started(0), connected_at(0), last_status_ms(0),
      status_retry_ms(0), backoff_index(0), status_retry_pending(false),
      client_started(false), configured(false), connected(false), status_pending(false),
      state(STATE_OFF) {}

MQTTObserver::MQTTObserver()
    : _fs(nullptr), _identity(nullptr), _origin_source(nullptr), _origin{},
      _radio_freq(0), _radio_bw(0), _radio_sf(0), _radio_cr(0), _config{},
      _runtime_config{}, _work_config{}, _io_config{}, _save_record{}, _load_record{},
      _config_mux(portMUX_INITIALIZER_UNLOCKED), _queue_mux(portMUX_INITIALIZER_UNLOCKED),
      _queue(), _raw_stager(), _brokers{}, _wifi_started(false), _task(nullptr),
      _queue_disconnected_since(0), _last_ntp_request(0), _device_id{}, _json{},
      _state(STATE_OFF), _save_state(), _received(0), _published(0),
      _disconnected_drops(0), _queue_drops(0), _publish_drops(0) {
  MQTTObserverConfigCodec::setDefaults(_config);
  MQTTObserverConfigCodec::setDefaults(_runtime_config);
  MQTTObserverConfigCodec::setDefaults(_work_config);
  MQTTObserverConfigCodec::setDefaults(_io_config);
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
  PersistLoadSource source = chooseConfigToLoad(fs, kConfigPath, validateRecord, this,
                                                 path, sizeof(path));
  if (source != PERSIST_LOAD_NONE) {
    File file = fsOpenRead(fs, path);
    if (file) {
      if (readRecord(file, _io_config)) {
        portENTER_CRITICAL(&_config_mux);
        _config = _io_config;
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

bool MQTTObserver::ready(const MQTTObserverConfig& config, size_t slot) const {
  if (slot >= MQTT_OBSERVER_MAX_BROKERS) return false;
  const MQTTObserverBrokerConfig& broker = config.brokers[slot];
  bool needs_iata = !broker.topic[0] || strstr(broker.topic, "{iata}") != nullptr;
  return broker.enabled && config.wifi_ssid[0] && broker.server[0] &&
         (!needs_iata || (config.iata[0] && strcmp(config.iata, "XXX") != 0)) &&
         MQTTObserverConfigCodec::valid(config);
}

bool MQTTObserver::readRecord(File& file, MQTTObserverConfig& config) {
  if (file.size() != MQTTObserverRecord::kRecordSize) return false;
  if (file.read(_load_record, sizeof(_load_record)) != sizeof(_load_record)) return false;
  return MQTTObserverRecord::decode(_load_record, sizeof(_load_record), config);
}

bool MQTTObserver::writeRecord(File& file, void* context) {
  const uint8_t* record = static_cast<const uint8_t*>(context);
  return record && file.write(record, MQTTObserverRecord::kRecordSize) == MQTTObserverRecord::kRecordSize;
}

bool MQTTObserver::validateRecord(File& file, void* context) {
  MQTTObserver* observer = static_cast<MQTTObserver*>(context);
  return observer && observer->readRecord(file, observer->_io_config);
}

bool MQTTObserver::saveConfig() {
  portENTER_CRITICAL(&_config_mux);
  _io_config = _config;
  portEXIT_CRITICAL(&_config_mux);
  if (MQTTObserverRecord::encode(_io_config, _save_record, sizeof(_save_record)) !=
      sizeof(_save_record)) {
    _save_state.retryLater();
    return false;
  }
  bool saved = saveStaged(_fs, kConfigPath, writeRecord, _save_record,
                          validateRecord, this, validateRecord, this);
  if (saved) _save_state.clear();
  else _save_state.retryLater();
  return saved;
}

bool MQTTObserver::needsAwake() const {
  // Only a slot the worker would actually try to bring up may hold the radio
  // awake — a half-configured one (no SSID, or an {iata} topic without an
  // IATA) connects to nothing and would otherwise block powersaving forever.
  portENTER_CRITICAL(&_config_mux);
  bool configured = false;
  for (size_t i = 0; i < MQTT_OBSERVER_MAX_BROKERS; i++)
    configured = configured || ready(_config, i);
  portEXIT_CRITICAL(&_config_mux);
  State state = _state.load(std::memory_order_relaxed);
  return configured || state == STATE_WIFI || state == STATE_TIME ||
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
  char origin[sizeof(_origin)];
  portENTER_CRITICAL(&_config_mux);
  const char* configured_origin = _config.origin[0] ? _config.origin :
                                  (_origin_source ? _origin_source : "");
  strncpy(origin, configured_origin, sizeof(origin) - 1);
  origin[sizeof(origin) - 1] = 0;
  portEXIT_CRITICAL(&_config_mux);
  portENTER_CRITICAL(&_config_mux);
  if (strncmp(_origin, origin, sizeof(_origin)) != 0) memcpy(_origin, origin, sizeof(_origin));
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
  bool should_capture = false;
  portENTER_CRITICAL(&_config_mux);
  if (_config.rx_enabled && (_config.packets_enabled || _config.raw_enabled)) {
    for (size_t i = 0; i < MQTT_OBSERVER_MAX_BROKERS; i++) {
      const MQTTObserverBrokerConfig& broker = _config.brokers[i];
      if (broker.enabled && broker.server[0]) { should_capture = true; break; }
    }
  }
  portEXIT_CRITICAL(&_config_mux);
  if (!should_capture) return;

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
  event.score = score;

  portENTER_CRITICAL(&_queue_mux);
  MQTTObserverEventQueue<Event, MQTT_OBSERVER_QUEUE_CAPACITY>::OfferResult result =
      _queue.offer(event);
  portEXIT_CRITICAL(&_queue_mux);
  if (result != MQTTObserverEventQueue<Event, MQTT_OBSERVER_QUEUE_CAPACITY>::QUEUED)
    _queue_drops.fetch_add(1, std::memory_order_relaxed);
}

void MQTTObserver::flushQueue() {
  portENTER_CRITICAL(&_queue_mux);
  size_t discarded = _queue.size();
  _queue.clear();
  portEXIT_CRITICAL(&_queue_mux);
  if (discarded) _disconnected_drops.fetch_add(discarded, std::memory_order_relaxed);
}

size_t MQTTObserver::queueSize() {
  portENTER_CRITICAL(&_queue_mux);
  size_t count = _queue.size();
  portEXIT_CRITICAL(&_queue_mux);
  return count;
}

bool MQTTObserver::createJwt(size_t slot, const MQTTObserverBrokerConfig& config,
                             uint32_t& expires_at) {
  if (slot >= MQTT_OBSERVER_MAX_BROKERS || !_identity || !config.audience[0]) return false;
  BrokerRuntime& runtime = _brokers[slot];
  time_t now = time(nullptr);
  if (now < static_cast<time_t>(kMinimumValidTime)) return false;

  char public_key[2 * PUB_KEY_SIZE + 1];
  bytesToHex(_identity->pub_key, PUB_KEY_SIZE, public_key);
  snprintf(runtime.jwt_username, sizeof(runtime.jwt_username), "v1_%s", public_key);
  const char header_json[] = "{\"alg\":\"Ed25519\",\"typ\":\"JWT\"}";
  unsigned long issued_at = static_cast<unsigned long>(now);
  unsigned long expiry = issued_at + kJwtLifetimeSeconds;
  char owner[sizeof(config.owner)];
  strncpy(owner, config.owner, sizeof(owner) - 1);
  owner[sizeof(owner) - 1] = 0;
  for (char* p = owner; *p; p++) if (*p >= 'a' && *p <= 'f') *p -= 'a' - 'A';
  char client_version[96], escaped_client[193], escaped_email[129];
  snprintf(client_version, sizeof(client_version), "meshcore-jhuebert/%s", FIRMWARE_VERSION);
  if (!jsonEscape(client_version, escaped_client, sizeof(escaped_client)) ||
      !jsonEscape(config.email, escaped_email, sizeof(escaped_email))) return false;
  char owner_field[80] = {}, email_field[160] = {};
  if (owner[0] && snprintf(owner_field, sizeof(owner_field), ",\"owner\":\"%s\"", owner) >=
                      static_cast<int>(sizeof(owner_field))) return false;
  if (escaped_email[0] && snprintf(email_field, sizeof(email_field), ",\"email\":\"%s\"",
                                   escaped_email) >= static_cast<int>(sizeof(email_field))) return false;
  int payload_len = snprintf(_json, sizeof(_json),
      "{\"publicKey\":\"%s\",\"aud\":\"%s\",\"iat\":%lu,\"exp\":%lu,\"client\":\"%s\"%s%s}",
      public_key, config.audience, issued_at, expiry, escaped_client, owner_field, email_field);
  if (payload_len <= 0 || static_cast<size_t>(payload_len) >= sizeof(_json)) return false;

  char encoded_header[64];
  size_t header_len = 0, encoded_payload_len = 0;
  if (!base64Url(reinterpret_cast<const uint8_t*>(header_json), strlen(header_json),
                 encoded_header, sizeof(encoded_header), header_len) ||
      !base64Url(reinterpret_cast<const uint8_t*>(_json), static_cast<size_t>(payload_len),
                 runtime.jwt_token, sizeof(runtime.jwt_token), encoded_payload_len)) return false;
  int signing_len = snprintf(_json, sizeof(_json), "%s.%s", encoded_header, runtime.jwt_token);
  if (signing_len <= 0 || static_cast<size_t>(signing_len) >= sizeof(_json)) return false;

  uint8_t signature[64];
  _identity->sign(signature, reinterpret_cast<const uint8_t*>(_json), signing_len);
  char signature_hex[129];
  bytesToHex(signature, sizeof(signature), signature_hex);
  size_t token_len = header_len + 1 + encoded_payload_len + 1 + sizeof(signature_hex) - 1;
  if (token_len >= sizeof(runtime.jwt_token)) {
    runtime.jwt_token[0] = 0;
    return false;
  }
  memmove(runtime.jwt_token + header_len + 1, runtime.jwt_token, encoded_payload_len);
  memcpy(runtime.jwt_token, encoded_header, header_len);
  runtime.jwt_token[header_len] = '.';
  runtime.jwt_token[header_len + 1 + encoded_payload_len] = '.';
  memcpy(runtime.jwt_token + header_len + 2 + encoded_payload_len,
         signature_hex, sizeof(signature_hex));
  expires_at = expiry;
  return true;
}

bool MQTTObserver::configureClient(size_t slot, const MQTTObserverBrokerConfig& config) {
  if (slot >= MQTT_OBSERVER_MAX_BROKERS) return false;
  BrokerRuntime& runtime = _brokers[slot];
  if (!runtime.client) {
    runtime.client = new (std::nothrow) PsychicMqttClient();
    if (!runtime.client) return false;
    runtime.client->setAutoReconnect(false);
    runtime.client->onConnect([this, slot](bool) {
      _brokers[slot].connected_at.store(millis(), std::memory_order_relaxed);
      _brokers[slot].connected.store(true, std::memory_order_relaxed);
      _brokers[slot].status_pending.store(true, std::memory_order_relaxed);
      _brokers[slot].state.store(STATE_CONNECTED, std::memory_order_relaxed);
    });
    runtime.client->onDisconnect([this, slot](bool) {
      _brokers[slot].connected.store(false, std::memory_order_relaxed);
      _brokers[slot].state.store(STATE_RETRY, std::memory_order_relaxed);
    });
    runtime.client->onError([this, slot](esp_mqtt_error_codes_t) {
      _brokers[slot].state.store(STATE_ERROR, std::memory_order_relaxed);
    });
  }
  if (!MQTTObserverConfigCodec::buildServerUri(config, runtime.server_uri,
                                                sizeof(runtime.server_uri))) return false;
  runtime.client->setServer(runtime.server_uri);
  runtime.client->setClientId(_device_id);
  runtime.client->setKeepAlive(60);
  esp_mqtt_client_config_t* mqtt_config = runtime.client->getMqttConfig();
  if (mqtt_config) mqtt_config->network_timeout_ms = 2500;
  runtime.client->setBufferSize(1280);
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
  if (mqtt_config) mqtt_config->buffer.out_size = 1280;
#endif
  runtime.client->setCleanSession(true);
  runtime.client->attachArduinoCACertBundle(secureUri(runtime.server_uri));
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
    runtime.username[0] = 0;
    runtime.password[0] = 0;
    if (!createJwt(slot, config, runtime.jwt_expiry)) return false;
    runtime.client->setCredentials(runtime.jwt_username, runtime.jwt_token);
  } else {
    runtime.jwt_expiry = 0;
    strncpy(runtime.username, config.username, sizeof(runtime.username) - 1);
    runtime.username[sizeof(runtime.username) - 1] = 0;
    strncpy(runtime.password, config.password, sizeof(runtime.password) - 1);
    runtime.password[sizeof(runtime.password) - 1] = 0;
    runtime.client->setCredentials(runtime.username[0] ? runtime.username : nullptr,
                                   runtime.password[0] ? runtime.password : nullptr);
  }
  return true;
}

void MQTTObserver::disconnectBroker(size_t slot) {
  if (slot >= MQTT_OBSERVER_MAX_BROKERS) return;
  BrokerRuntime& runtime = _brokers[slot];
  if (runtime.client && (runtime.client_started || runtime.connected.load(std::memory_order_relaxed)))
    runtime.client->disconnect();
  runtime.client_started = false;
  runtime.connected.store(false, std::memory_order_relaxed);
  runtime.status_pending.store(false, std::memory_order_relaxed);
}

void MQTTObserver::worker() {
  bool runtime_config_initialized = false;
  char active_ssid[sizeof(_config.wifi_ssid)] = {};
  char active_password[sizeof(_config.wifi_password)] = {};
  char active_ntp_server[sizeof(_config.ntp_server)] = {};
  uint8_t active_wifi_power_save = 0xff;

  while (true) {
    copyConfig(_work_config);
    uint32_t now = millis();
    bool have_ready_broker = false;
    for (size_t i = 0; i < MQTT_OBSERVER_MAX_BROKERS; i++)
      have_ready_broker = have_ready_broker || ready(_work_config, i);

    if (!have_ready_broker) {
      for (size_t i = 0; i < MQTT_OBSERVER_MAX_BROKERS; i++) {
        disconnectBroker(i);
        _brokers[i].state.store(_work_config.brokers[i].enabled ? STATE_CONFIG : STATE_OFF,
                                std::memory_order_relaxed);
      }
      if (_wifi_started) {
        WiFi.disconnect(true, false);
        WiFi.mode(WIFI_OFF);
        _wifi_started = false;
      }
      runtime_config_initialized = false;
      _state.store(STATE_CONFIG, std::memory_order_relaxed);
      if (queueSize()) {
        if (!_queue_disconnected_since) _queue_disconnected_since = now ? now : 1;
        else if (MQTTObserverQueuePolicy::shouldFlushDisconnected(now, _queue_disconnected_since)) {
          flushQueue();
          _queue_disconnected_since = now ? now : 1;
        }
      } else {
        _queue_disconnected_since = 0;
      }
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }

    bool wifi_changed = _wifi_started &&
        (strcmp(active_ssid, _work_config.wifi_ssid) != 0 ||
         strcmp(active_password, _work_config.wifi_password) != 0);
    if (wifi_changed) {
      for (size_t i = 0; i < MQTT_OBSERVER_MAX_BROKERS; i++) disconnectBroker(i);
      WiFi.disconnect(true, false);
      WiFi.mode(WIFI_OFF);
      _wifi_started = false;
      active_ntp_server[0] = 0;
    }

    if (!_wifi_started) {
      WiFi.mode(WIFI_STA);
      WiFi.setAutoReconnect(true);
      WiFi.begin(_work_config.wifi_ssid, _work_config.wifi_password);
      strcpy(active_ssid, _work_config.wifi_ssid);
      strcpy(active_password, _work_config.wifi_password);
      _wifi_started = true;
      active_wifi_power_save = 0xff;
      active_ntp_server[0] = 0;
      _last_ntp_request = 0;
      _state.store(STATE_WIFI, std::memory_order_relaxed);
    }

    if (WiFi.status() != WL_CONNECTED) {
      for (size_t i = 0; i < MQTT_OBSERVER_MAX_BROKERS; i++) {
        disconnectBroker(i);
        if (_work_config.brokers[i].enabled)
          _brokers[i].state.store(STATE_WIFI, std::memory_order_relaxed);
      }
      _state.store(STATE_WIFI, std::memory_order_relaxed);
      if (queueSize()) {
        if (!_queue_disconnected_since) _queue_disconnected_since = now ? now : 1;
        else if (MQTTObserverQueuePolicy::shouldFlushDisconnected(now, _queue_disconnected_since)) {
          flushQueue();
          _queue_disconnected_since = now ? now : 1;
        }
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (active_wifi_power_save != _work_config.wifi_power_save) {
      wifi_ps_type_t power_save = _work_config.wifi_power_save == 1 ? WIFI_PS_NONE :
                                  _work_config.wifi_power_save == 2 ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM;
      if (esp_wifi_set_ps(power_save) == ESP_OK) active_wifi_power_save = _work_config.wifi_power_save;
    }

    const char* ntp_server = _work_config.ntp_server[0] ? _work_config.ntp_server : "pool.ntp.org";
    if (strcmp(active_ntp_server, ntp_server) != 0) {
      strncpy(active_ntp_server, ntp_server, sizeof(active_ntp_server) - 1);
      active_ntp_server[sizeof(active_ntp_server) - 1] = 0;
      configTime(0, 0, active_ntp_server, "time.google.com", "time.cloudflare.com");
      _last_ntp_request = now ? now : 1;
    }
    time_t epoch = time(nullptr);
    if (epoch < static_cast<time_t>(kMinimumValidTime)) {
      if (!_last_ntp_request || static_cast<uint32_t>(now - _last_ntp_request) >= 30000) {
        configTime(0, 0, ntp_server, "time.google.com", "time.cloudflare.com");
        _last_ntp_request = now ? now : 1;
      }
      for (size_t i = 0; i < MQTT_OBSERVER_MAX_BROKERS; i++)
        if (_work_config.brokers[i].enabled)
          _brokers[i].state.store(STATE_TIME, std::memory_order_relaxed);
      _state.store(STATE_TIME, std::memory_order_relaxed);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    bool any_connected = false;
    bool any_connecting = false;
    for (size_t i = 0; i < MQTT_OBSERVER_MAX_BROKERS; i++) {
      BrokerRuntime& runtime = _brokers[i];
      const MQTTObserverBrokerConfig& broker = _work_config.brokers[i];
      if (!ready(_work_config, i)) {
        disconnectBroker(i);
        runtime.state.store(broker.enabled ? STATE_CONFIG : STATE_OFF, std::memory_order_relaxed);
        continue;
      }
      bool connection_changed = !runtime_config_initialized || wifi_changed ||
          !sameBrokerConnection(broker, _runtime_config.brokers[i]);
      if (connection_changed || !runtime.configured) {
        disconnectBroker(i);
        runtime.configured = false;
        runtime.status_retry_pending = false;
        runtime.last_status_ms = 0;
        if (!configureClient(i, broker)) {
          runtime.state.store(STATE_ERROR, std::memory_order_relaxed);
          continue;
        }
        runtime.configured = true;
        runtime.next_connect_ms = now;
        runtime.backoff_index = 0;
        runtime.client_started = false;
      }

      bool connected = runtime.client && runtime.client->connected();
      if (connected) {
        if (!runtime.connected.exchange(true, std::memory_order_relaxed))
          runtime.connected_at.store(now, std::memory_order_relaxed);
        runtime.state.store(STATE_CONNECTED, std::memory_order_relaxed);
        if (static_cast<uint32_t>(now - runtime.connected_at.load(std::memory_order_relaxed)) >= 120000)
          runtime.backoff_index = 0;

        epoch = time(nullptr);
        if (runtime.jwt_expiry && epoch + 300 >= runtime.jwt_expiry) {
          disconnectBroker(i);
          runtime.configured = false;
          if (configureClient(i, broker)) {
            runtime.configured = true;
            runtime.next_connect_ms = now;
          } else runtime.state.store(STATE_ERROR, std::memory_order_relaxed);
          continue;
        }
        bool status_due = runtime.status_pending.exchange(false, std::memory_order_relaxed) ||
            runtime.status_retry_pending || !runtime.last_status_ms ||
            static_cast<uint32_t>(now - runtime.last_status_ms) >=
                static_cast<uint32_t>(_work_config.status_interval_minutes) * 60000UL;
        bool status_retry_ready = MQTTObserverQueuePolicy::retryReady(
            now, runtime.status_retry_ms, runtime.status_retry_pending ? 1 : 0);
        if (_work_config.status_enabled && status_due && status_retry_ready) {
          if (publishStatus(i)) {
            runtime.last_status_ms = now ? now : 1;
            runtime.status_retry_pending = false;
          } else {
            _publish_drops.fetch_add(1, std::memory_order_relaxed);
            runtime.status_retry_ms = now + 30000;
            runtime.status_retry_pending = true;
          }
        }
        any_connected = true;
        continue;
      }

      runtime.connected.store(false, std::memory_order_relaxed);
      if (runtime.client_started &&
          static_cast<uint32_t>(now - runtime.attempt_started) >= 30000) {
        runtime.client->disconnect();
        runtime.client_started = false;
        uint8_t index = runtime.backoff_index < 5 ? runtime.backoff_index : 4;
        runtime.next_connect_ms = now + kBackoffMs[index];
        if (runtime.backoff_index < 4) runtime.backoff_index++;
        runtime.state.store(STATE_RETRY, std::memory_order_relaxed);
      }
      if (!runtime.client_started && static_cast<int32_t>(now - runtime.next_connect_ms) >= 0) {
        runtime.client->connect();
        runtime.client_started = true;
        runtime.attempt_started = now;
        runtime.state.store(STATE_CONNECTING, std::memory_order_relaxed);
      }
      State state = runtime.state.load(std::memory_order_relaxed);
      any_connecting = any_connecting || state == STATE_CONNECTING || state == STATE_RETRY;
    }

    _runtime_config = _work_config;
    runtime_config_initialized = true;
    if (any_connected) _state.store(STATE_CONNECTED, std::memory_order_relaxed);
    else if (any_connecting) _state.store(STATE_CONNECTING, std::memory_order_relaxed);
    else _state.store(STATE_RETRY, std::memory_order_relaxed);

    if (queueSize() == 0) {
      _queue_disconnected_since = 0;
    } else if (!any_connected) {
      if (!_queue_disconnected_since) _queue_disconnected_since = now ? now : 1;
      else if (MQTTObserverQueuePolicy::shouldFlushDisconnected(now, _queue_disconnected_since)) {
        flushQueue();
        _queue_disconnected_since = now ? now : 1;
      }
    } else {
      _queue_disconnected_since = 0;
      typename MQTTObserverEventQueue<Event, MQTT_OBSERVER_QUEUE_CAPACITY>::Entry queued;
      bool have_event = false;
      portENTER_CRITICAL(&_queue_mux);
      have_event = _queue.beginAttempt(millis(), queued);
      portEXIT_CRITICAL(&_queue_mux);
      if (have_event) {
        bool published = publishEvent(queued.event);
        MQTTObserverQueuePolicy::RetryDecision retry =
            MQTTObserverQueuePolicy::retryDecision(published, queued.retry_attempts, millis());
        portENTER_CRITICAL(&_queue_mux);
        if (retry.action == MQTTObserverQueuePolicy::RetryAction::Complete ||
            retry.action == MQTTObserverQueuePolicy::RetryAction::Drop) {
          _queue.completeAttempt();
        } else {
          _queue.retryAttempt(retry.retry_attempts, retry.next_retry_ms);
        }
        portEXIT_CRITICAL(&_queue_mux);
        if (retry.action == MQTTObserverQueuePolicy::RetryAction::Drop)
          _publish_drops.fetch_add(1, std::memory_order_relaxed);
      }
    }
    vTaskDelay(pdMS_TO_TICKS(queueSize() ? 5 : 50));
  }
}

bool MQTTObserver::publishStatus(size_t slot) {
  if (slot >= MQTT_OBSERVER_MAX_BROKERS) return false;
  BrokerRuntime& runtime = _brokers[slot];
  if (!runtime.client || !runtime.client->connected() || !_runtime_config.status_enabled) return false;
  const MQTTObserverBrokerConfig& broker = _runtime_config.brokers[slot];
  char topic[128];
  if (!MQTTObserverConfigCodec::buildTopic(_runtime_config, broker, _device_id, "status",
                                           topic, sizeof(topic))) return false;
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
  return runtime.client->publish(topic, 1, false, _json, written, true) >= 0;
}

bool MQTTObserver::publishEvent(const Event& event) {
  bool any_published = false;
  char origin[sizeof(_origin)];
  portENTER_CRITICAL(&_config_mux);
  memcpy(origin, _origin, sizeof(origin));
  portEXIT_CRITICAL(&_config_mux);

  for (size_t slot = 0; slot < MQTT_OBSERVER_MAX_BROKERS; slot++) {
    BrokerRuntime& runtime = _brokers[slot];
    const MQTTObserverBrokerConfig& broker = _runtime_config.brokers[slot];
    if (!broker.enabled || !runtime.client || !runtime.client->connected()) continue;
    char topic[128];
    size_t written = 0;
    if (_runtime_config.packets_enabled) {
      uint8_t serialized[MAX_TRANS_UNIT];
      const uint8_t* wire = event.raw;
      size_t wire_len = event.raw_len;
      if (!wire_len) {
        int expected = event.packet.getRawLength();
        if (expected <= 0 || expected > MAX_TRANS_UNIT ||
            event.packet.payload_len > MAX_PACKET_PAYLOAD) {
          _publish_drops.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        wire_len = event.packet.writeTo(serialized);
        if (!wire_len || wire_len > sizeof(serialized)) {
          _publish_drops.fetch_add(1, std::memory_order_relaxed);
          continue;
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
      if (MQTTObserverConfigCodec::buildTopic(_runtime_config, broker, _device_id,
                                              "packets", topic, sizeof(topic)) &&
          MQTTObserverFormat::buildPacket(view, _json, sizeof(_json), written)) {
        if (runtime.client->publish(topic, 0, false, _json, written, false) >= 0) {
          _published.fetch_add(1, std::memory_order_relaxed);
          any_published = true;
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
          MQTTObserverConfigCodec::buildTopic(_runtime_config, broker, _device_id,
                                              "raw", topic, sizeof(topic)) &&
          MQTTObserverFormat::buildRaw(origin, _device_id, timestamp,
                                       event.raw, event.raw_len, _json, sizeof(_json), written)) {
        if (runtime.client->publish(topic, 0, false, _json, written, false) >= 0) {
          _published.fetch_add(1, std::memory_order_relaxed);
          any_published = true;
        } else {
          _publish_drops.fetch_add(1, std::memory_order_relaxed);
        }
      } else {
        _publish_drops.fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  return any_published;
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

bool MQTTObserver::commandSlot(const char* command, const char* operation, size_t& slot,
                               const char*& property) const {
  return MQTTObserverCommandPolicy::parseSlot(command, operation,
                                                MQTT_OBSERVER_MAX_BROKERS,
                                                slot, property);
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
  uint8_t toggle = 0;
  const char* common_property = nullptr;
  const char* common_value = commandValue(command, "set mqtt.iata");
  if (common_value) common_property = "iata";
  else if ((common_value = commandValue(command, "set mqtt.origin"))) common_property = "origin";
  else if ((common_value = commandValue(command, "set mqtt.ntp"))) common_property = "ntp";
  else if ((common_value = commandValue(command, "set mqtt.rx"))) common_property = "rx";
  else if ((common_value = commandValue(command, "set mqtt.status"))) common_property = "status";
  else if ((common_value = commandValue(command, "set mqtt.packets"))) common_property = "packets";
  else if ((common_value = commandValue(command, "set mqtt.raw"))) common_property = "raw";
  else if ((common_value = commandValue(command, "set mqtt.interval"))) common_property = "interval";
  if (common_property) {
    if (strcmp(common_property, "iata") == 0) {
      char iata[sizeof(_config.iata)];
      if (strlen(common_value) >= sizeof(iata)) {
        snprintf(reply, 160, "Err - IATA must be 3 letters/digits; XXX is reserved");
        return true;
      }
      for (size_t i = 0; i <= strlen(common_value); i++)
        iata[i] = common_value[i] >= 'a' && common_value[i] <= 'z' ?
                  common_value[i] - 'a' + 'A' : common_value[i];
      if (!MQTTObserverConfigCodec::validIata(iata)) {
        snprintf(reply, 160, "Err - IATA must be 3 letters/digits; XXX is reserved");
        return true;
      }
      portENTER_CRITICAL(&_config_mux);
      changed = strcmp(_config.iata, iata) != 0 &&
                setString(_config.iata, sizeof(_config.iata), iata);
      portEXIT_CRITICAL(&_config_mux);
    } else if (strcmp(common_property, "origin") == 0) {
      if (strlen(common_value) >= sizeof(_config.origin)) {
        snprintf(reply, 160, "Err - origin too long");
        return true;
      }
      portENTER_CRITICAL(&_config_mux);
      changed = strcmp(_config.origin, common_value) != 0 &&
                setString(_config.origin, sizeof(_config.origin), common_value);
      portEXIT_CRITICAL(&_config_mux);
    } else if (strcmp(common_property, "ntp") == 0) {
      const char* ntp = strcmp(common_value, "none") == 0 ? "" : common_value;
      if (!MQTTObserverConfigCodec::validNtpServer(ntp) ||
          strlen(ntp) >= sizeof(_config.ntp_server)) {
        snprintf(reply, 160, "Err - invalid NTP server");
        return true;
      }
      portENTER_CRITICAL(&_config_mux);
      changed = strcmp(_config.ntp_server, ntp) != 0 &&
                setString(_config.ntp_server, sizeof(_config.ntp_server), ntp);
      portEXIT_CRITICAL(&_config_mux);
    } else if (strcmp(common_property, "interval") == 0) {
      unsigned minutes;
      if (!parseUnsigned(common_value, 1, 60, minutes)) {
        snprintf(reply, 160, "Err - interval must be 1-60 minutes");
        return true;
      }
      portENTER_CRITICAL(&_config_mux);
      changed = _config.status_interval_minutes != minutes;
      _config.status_interval_minutes = static_cast<uint8_t>(minutes);
      portEXIT_CRITICAL(&_config_mux);
    } else {
      if (!parseToggle(common_value, toggle)) {
        snprintf(reply, 160, "Err - expected on or off");
        return true;
      }
      portENTER_CRITICAL(&_config_mux);
      uint8_t* field = strcmp(common_property, "rx") == 0 ? &_config.rx_enabled :
                       strcmp(common_property, "status") == 0 ? &_config.status_enabled :
                       strcmp(common_property, "packets") == 0 ? &_config.packets_enabled :
                       &_config.raw_enabled;
      changed = *field != toggle;
      *field = toggle;
      portEXIT_CRITICAL(&_config_mux);
    }
    if (changed) markDirty();
    snprintf(reply, 160, "OK - MQTT setting saved");
    return true;
  }

  size_t slot = 0;
  const char* property = nullptr;
  const char* value = nullptr;
  bool has_slot = commandSlot(command, "set ", slot, property);
  const char* space = has_slot ? strchr(property, ' ') : nullptr;
  size_t property_len = space ? static_cast<size_t>(space - property) : strlen(property ? property : "");
  if (has_slot && space) {
    value = space + 1;
    while (*value == ' ') value++;
  }
#define MQTT_PROP_IS(name) (has_slot && property_len == sizeof(name) - 1 && strncmp(property, name, sizeof(name) - 1) == 0)

  if (MQTT_PROP_IS("enabled")) {
    uint8_t enabled;
    if (!parseToggle(value ? value : "", enabled)) {
      snprintf(reply, 160, "Err - expected on or off");
      return true;
    }
    portENTER_CRITICAL(&_config_mux);
    changed = _config.brokers[slot].enabled != enabled;
    _config.brokers[slot].enabled = enabled;
    portEXIT_CRITICAL(&_config_mux);
    if (changed) markDirty();
    snprintf(reply, 160, "OK - MQTT%u %s", static_cast<unsigned>(slot + 1),
             enabled ? "enabled" : "disabled");
    return true;
  }

  if (MQTT_PROP_IS("port")) {
    unsigned port;
    if (!parseUnsigned(value, 1, 65535, port)) {
      snprintf(reply, 160, "Err - port must be 1-65535");
      return true;
    }
    portENTER_CRITICAL(&_config_mux);
    changed = _config.brokers[slot].port != port;
    _config.brokers[slot].port = static_cast<uint16_t>(port);
    portEXIT_CRITICAL(&_config_mux);
    if (changed) markDirty();
    snprintf(reply, 160, "OK - MQTT%u port saved", static_cast<unsigned>(slot + 1));
    return true;
  }

  if (MQTT_PROP_IS("server") || MQTT_PROP_IS("username") ||
      MQTT_PROP_IS("password") || MQTT_PROP_IS("audience") ||
      MQTT_PROP_IS("topic") || MQTT_PROP_IS("token") ||
      MQTT_PROP_IS("owner") || MQTT_PROP_IS("email")) {
    if (!value) value = "";
    size_t capacity = 0;
    enum Validation { NONE, SERVER, AUDIENCE, TOPIC, OWNER, EMAIL } validation = NONE;
    if (MQTT_PROP_IS("server")) { capacity = sizeof(_config.brokers[slot].server); validation = SERVER; }
    else if (MQTT_PROP_IS("username")) capacity = sizeof(_config.brokers[slot].username);
    else if (MQTT_PROP_IS("password")) capacity = sizeof(_config.brokers[slot].password);
    else if (MQTT_PROP_IS("audience")) { capacity = sizeof(_config.brokers[slot].audience); validation = AUDIENCE; }
    else if (MQTT_PROP_IS("topic")) { capacity = sizeof(_config.brokers[slot].topic); validation = TOPIC; }
    else if (MQTT_PROP_IS("token")) { capacity = sizeof(_config.brokers[slot].token); validation = TOPIC; }
    else if (MQTT_PROP_IS("owner")) { capacity = sizeof(_config.brokers[slot].owner); validation = OWNER; }
    else { capacity = sizeof(_config.brokers[slot].email); validation = EMAIL; }
    const char* setting = validation == SERVER && !*value ? "" : value;
    if (strlen(setting) >= capacity ||
        (validation == SERVER && *setting && !MQTTObserverConfigCodec::validServer(setting)) ||
        (validation == AUDIENCE && !MQTTObserverConfigCodec::validAudience(setting)) ||
        (validation == TOPIC && !MQTTObserverConfigCodec::validTopic(setting)) ||
        (validation == OWNER && !MQTTObserverConfigCodec::validOwner(setting)) ||
        (validation == EMAIL && !MQTTObserverConfigCodec::validEmail(setting))) {
      snprintf(reply, 160, "Err - invalid or oversized MQTT setting");
      return true;
    }
    portENTER_CRITICAL(&_config_mux);
    MQTTObserverBrokerConfig& broker = _config.brokers[slot];
    char* field = MQTT_PROP_IS("server") ? broker.server :
                  MQTT_PROP_IS("username") ? broker.username :
                  MQTT_PROP_IS("password") ? broker.password :
                  MQTT_PROP_IS("audience") ? broker.audience :
                  MQTT_PROP_IS("topic") ? broker.topic :
                  MQTT_PROP_IS("token") ? broker.token :
                  MQTT_PROP_IS("owner") ? broker.owner : broker.email;
    changed = strcmp(field, setting) != 0;
    if (changed) setString(field, capacity, setting);
    portEXIT_CRITICAL(&_config_mux);
    if (changed) markDirty();
    if (MQTT_PROP_IS("password") || MQTT_PROP_IS("token"))
      snprintf(reply, 160, "OK - credential saved");
    else
      snprintf(reply, 160, "OK - MQTT%u setting saved", static_cast<unsigned>(slot + 1));
    return true;
  }
#undef MQTT_PROP_IS

  if (startsWith(command, "set wifi.powersave ")) {
    const char* ps = command + strlen("set wifi.powersave ");
    uint8_t power_save;
    if (strcmp(ps, "min") == 0) power_save = 0;
    else if (strcmp(ps, "none") == 0) power_save = 1;
    else if (strcmp(ps, "max") == 0) power_save = 2;
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

  const char* wifi_value = commandValue(command, "set wifi.ssid");
  bool wifi_secret = false;
  char* wifi_field = _config.wifi_ssid;
  size_t wifi_capacity = sizeof(_config.wifi_ssid);
  if (!wifi_value) {
    wifi_value = commandValue(command, "set wifi.pwd");
    if (wifi_value) {
      wifi_field = _config.wifi_password;
      wifi_capacity = sizeof(_config.wifi_password);
      wifi_secret = true;
    }
  }
  if (wifi_value) {
    if (strlen(wifi_value) >= wifi_capacity) {
      snprintf(reply, 160, "Err - setting too long");
      return true;
    }
    portENTER_CRITICAL(&_config_mux);
    changed = strcmp(wifi_field, wifi_value) != 0;
    if (changed) setString(wifi_field, wifi_capacity, wifi_value);
    portEXIT_CRITICAL(&_config_mux);
    if (changed) markDirty();
    snprintf(reply, 160, wifi_secret ? "OK - credential saved" : "OK - WiFi SSID saved");
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
  if (strcmp(command, "get wifi.powersave") == 0 || strcmp(command, "get wifi.ssid") == 0 ||
      strcmp(command, "get wifi.pwd") == 0) {
    const char* value;
    portENTER_CRITICAL(&_config_mux);
    if (strcmp(command, "get wifi.powersave") == 0)
      value = _config.wifi_power_save == 1 ? "none" :
              _config.wifi_power_save == 2 ? "max" : "min";
    else if (strcmp(command, "get wifi.ssid") == 0)
      value = _config.wifi_ssid[0] ? _config.wifi_ssid : "not set";
    else value = _config.wifi_password[0] ? "configured" : "not set";
    snprintf(reply, 160, "%s", value);
    portEXIT_CRITICAL(&_config_mux);
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

  enum CommonGet { COMMON_NONE, COMMON_IATA, COMMON_ORIGIN, COMMON_NTP, COMMON_RX,
                   COMMON_STATUS, COMMON_PACKETS, COMMON_RAW, COMMON_INTERVAL } common = COMMON_NONE;
  if (strcmp(command, "get mqtt.iata") == 0) common = COMMON_IATA;
  else if (strcmp(command, "get mqtt.origin") == 0) common = COMMON_ORIGIN;
  else if (strcmp(command, "get mqtt.ntp") == 0) common = COMMON_NTP;
  else if (strcmp(command, "get mqtt.rx") == 0) common = COMMON_RX;
  else if (strcmp(command, "get mqtt.status.enabled") == 0) common = COMMON_STATUS;
  else if (strcmp(command, "get mqtt.packets") == 0) common = COMMON_PACKETS;
  else if (strcmp(command, "get mqtt.raw") == 0) common = COMMON_RAW;
  else if (strcmp(command, "get mqtt.interval") == 0) common = COMMON_INTERVAL;
  if (common != COMMON_NONE) {
    char common_value[160];
    portENTER_CRITICAL(&_config_mux);
    switch (common) {
      case COMMON_IATA:
        snprintf(common_value, sizeof(common_value), "%s", _config.iata[0] ? _config.iata : "not configured");
        break;
      case COMMON_ORIGIN:
        snprintf(common_value, sizeof(common_value), "%s", _config.origin[0] ? _config.origin :
                 (_origin_source && *_origin_source ? _origin_source : "not configured"));
        break;
      case COMMON_NTP:
        snprintf(common_value, sizeof(common_value), "%s", _config.ntp_server[0] ? _config.ntp_server : "pool.ntp.org");
        break;
      case COMMON_RX: snprintf(common_value, sizeof(common_value), "%s", _config.rx_enabled ? "on" : "off"); break;
      case COMMON_STATUS: snprintf(common_value, sizeof(common_value), "%s", _config.status_enabled ? "on" : "off"); break;
      case COMMON_PACKETS: snprintf(common_value, sizeof(common_value), "%s", _config.packets_enabled ? "on" : "off"); break;
      case COMMON_RAW: snprintf(common_value, sizeof(common_value), "%s", _config.raw_enabled ? "on" : "off"); break;
      case COMMON_INTERVAL: snprintf(common_value, sizeof(common_value), "%u", static_cast<unsigned>(_config.status_interval_minutes)); break;
      default: common_value[0] = 0; break;
    }
    portEXIT_CRITICAL(&_config_mux);
    snprintf(reply, 160, "%s", common_value);
    return true;
  }

  size_t slot = 0;
  const char* property = nullptr;
  if (!commandSlot(command, "get ", slot, property)) return false;
  size_t property_len = strcspn(property, " ");
#define MQTT_GET_IS(name) (property_len == sizeof(name) - 1 && strncmp(property, name, sizeof(name) - 1) == 0)
  char value[160] = {};
  portENTER_CRITICAL(&_config_mux);
  const MQTTObserverBrokerConfig& broker = _config.brokers[slot];
  const char* result = nullptr;
  if (MQTT_GET_IS("server")) {
    MQTTObserverConfigCodec::hostLabel(broker.server, value, sizeof(value));
    result = value[0] ? value : "not configured";
  } else if (MQTT_GET_IS("port")) {
    snprintf(value, sizeof(value), "%u", static_cast<unsigned>(MQTTObserverConfigCodec::configuredPort(broker)));
    result = value;
  } else if (MQTT_GET_IS("audience")) result = broker.audience[0] ? broker.audience : "not configured";
  else if (MQTT_GET_IS("topic")) result = broker.topic[0] ? broker.topic : "meshcore/{iata}/{device}/{type}";
  else if (MQTT_GET_IS("owner")) result = broker.owner[0] ? broker.owner : "not configured";
  else if (MQTT_GET_IS("email")) result = broker.email[0] ? broker.email : "not configured";
  else if (MQTT_GET_IS("enabled")) result = broker.enabled ? "on" : "off";
  else if (MQTT_GET_IS("password")) result = broker.password[0] ? "configured" : "not set";
  else if (MQTT_GET_IS("username")) result = broker.username[0] ? "configured" : "not set";
  else if (MQTT_GET_IS("token")) result = broker.token[0] ? "configured" : "not set";
  if (result && result != value) {
    snprintf(value, sizeof(value), "%s", result);
    result = value;
  }
  portEXIT_CRITICAL(&_config_mux);

  if (result) {
    snprintf(reply, 160, "%s", result);
    return true;
  }
  if (MQTT_GET_IS("status")) {
    BrokerRuntime& runtime = _brokers[slot];
    snprintf(reply, 160, "mqtt%u %s wifi:%s q:%u",
             static_cast<unsigned>(slot + 1), stateName(runtime.state.load(std::memory_order_relaxed)),
             WiFi.status() == WL_CONNECTED ? "up" : "down", static_cast<unsigned>(queueSize()));
    return true;
  }
#undef MQTT_GET_IS
  return false;
}

const char* MQTTObserver::stateName(State state) const {
  switch (state) {
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
  size_t connected = 0;
  for (size_t i = 0; i < MQTT_OBSERVER_MAX_BROKERS; i++)
    if (_brokers[i].connected.load(std::memory_order_relaxed)) connected++;
  const char* wifi = WiFi.status() == WL_CONNECTED ? "up" : "down";
  time_t epoch = time(nullptr);
  const char* time_state = epoch >= static_cast<time_t>(kMinimumValidTime) ? "ok" : "wait";
  uint32_t drops = _disconnected_drops.load(std::memory_order_relaxed) +
                   _queue_drops.load(std::memory_order_relaxed) +
                   _publish_drops.load(std::memory_order_relaxed);
  snprintf(reply, capacity,
      "mqtt wifi:%s time:%s slots:%u/%u rx:%lu pub:%lu drop:%lu q:%u/%u heap:%u/%u",
      wifi, time_state, static_cast<unsigned>(connected),
      static_cast<unsigned>(MQTT_OBSERVER_MAX_BROKERS),
      static_cast<unsigned long>(_received.load(std::memory_order_relaxed)),
      static_cast<unsigned long>(_published.load(std::memory_order_relaxed)),
      static_cast<unsigned long>(drops), static_cast<unsigned>(queueSize()),
      static_cast<unsigned>(MQTT_OBSERVER_QUEUE_CAPACITY),
      static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
}

#endif  // WITH_MQTT_OBSERVER