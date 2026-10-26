#ifdef WITH_MQTT_OBSERVER

#include "MQTTObserver.h"
#include "MQTTObserverFormat.h"
#include "MQTTObserverRecord.h"
#include "../simple_repeater/CliUtil.h"
#include <Utils.h>
#include <WiFi.h>
#include <freertos/task.h>
#include <esp_wifi.h>
#include <esp_heap_caps.h>
#include <mbedtls/base64.h>
#include <mbedtls/platform.h>
#include <sys/time.h>
#include <time.h>
#include <stdlib.h>
#include <new>

// arduino-esp32 2.x's esp_crt_bundle.h declares only its own bundle shim, which
// verifies nothing until a bundle is loaded; the ESP-IDF attach uses the
// certificate bundle embedded in the binary, so use it on IDF < 5.
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR < 5
extern "C" esp_err_t esp_crt_bundle_attach(void *conf);
#endif

namespace {

const char kConfigPath[] = "/mqtt_prefs";
const uint32_t kBackoffMs[] = {10000, 30000, 60000, 120000, 300000};

bool parseToggle(const char* value, uint8_t& enabled) {
  if (strcmp(value, "on") == 0) { enabled = 1; return true; }
  if (strcmp(value, "off") == 0) { enabled = 0; return true; }
  return false;
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
  size_t used = 0;
  return MQTTObserverFormat::appendEscaped(input, output, capacity, used);
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
    : client(nullptr), server_uri{}, client_id{}, username{}, password{}, jwt_username{},
      jwt_token{}, jwt_expiry(0),
      next_connect_ms(0), attempt_started(0), connected_at(0), last_status_ms(0),
      status_retry_ms(0), backoff_index(0), status_retry_pending(false),
      client_started(false), configured(false), connected(false), status_pending(false),
      connect_failed(false), state(STATE_OFF),
      diag_error_type(DIAG_ERR_NONE), diag_tls_err(0), diag_tls_stack(0),
      diag_tls_flags(0), diag_sock_errno(0), diag_error_ms(0), diag_disconnect_ms(0),
      diag_connected_ms(0), diag_next_retry_ms(0), diag_attempts(0), diag_failures(0) {}

MQTTObserver::MQTTObserver()
    : _fs(nullptr), _identity(nullptr), _origin_source(nullptr), _origin{}, _model{},
      _firmware{}, _status_stats_fn(nullptr), _status_stats_ctx(nullptr),
      _radio_freq(0), _radio_bw(0), _radio_sf(0), _radio_cr(0), _config{},
      _runtime_config{}, _work_config{}, _io_config{}, _save_record{}, _load_record{},
      _config_mux(portMUX_INITIALIZER_UNLOCKED), _queue_mux(portMUX_INITIALIZER_UNLOCKED),
      _queue(), _raw_stager(), _brokers{}, _wifi_started(false),
      _queue_disconnected_since(0), _last_ntp_request(0), _device_id{}, _json{},
      _state(STATE_OFF), _save_state(), _received(0), _published(0),
      _disconnected_drops(0), _queue_drops(0), _publish_drops(0) {
  MQTTObserverConfigCodec::setDefaults(_config);
  MQTTObserverConfigCodec::setDefaults(_runtime_config);
  MQTTObserverConfigCodec::setDefaults(_work_config);
  MQTTObserverConfigCodec::setDefaults(_io_config);
}

void MQTTObserver::begin(FILESYSTEM* fs, mesh::LocalIdentity* identity, const char* origin,
                         const char* model, const char* firmware, float freq, float bw,
                         uint8_t sf, uint8_t cr) {
  _fs = fs;
  _identity = identity;
  _origin_source = origin;
  // The broker and the feed only need the connection state, not the library's
  // transport logs; keep them off the shared CLI console.
  esp_log_level_set("MQTT_CLIENT", ESP_LOG_NONE);
  esp_log_level_set("esp-tls", ESP_LOG_NONE);
  esp_log_level_set("\xf0\x9f\x90\x99", ESP_LOG_NONE);   // PsychicMqttClient's tag
#if defined(BOARD_HAS_PSRAM)
  if (psramFound()) mbedtls_platform_set_calloc_free(observerMbedtlsCalloc, observerMbedtlsFree);
#endif
  _radio_freq = freq;
  _radio_bw = bw;
  _radio_sf = sf;
  _radio_cr = cr;
  strncpy(_origin, origin ? origin : "", sizeof(_origin) - 1);
  _origin[sizeof(_origin) - 1] = 0;
  strncpy(_model, model && *model ? model : "unknown", sizeof(_model) - 1);
  _model[sizeof(_model) - 1] = 0;
  strncpy(_firmware, firmware && *firmware ? firmware : "unknown", sizeof(_firmware) - 1);
  _firmware[sizeof(_firmware) - 1] = 0;
  mesh::Utils::toHex(_device_id, identity->pub_key, PUB_KEY_SIZE);

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

  if (xTaskCreatePinnedToCore(taskEntry, "mqttObserver", 8192, this, 1, nullptr, 0) != pdPASS)
    _state.store(STATE_ERROR, std::memory_order_relaxed);
}

void MQTTObserver::setStatusStatsFn(StatusStatsFn fn, void* context) {
  _status_stats_fn = fn;
  _status_stats_ctx = context;
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

bool MQTTObserver::hasConnectHeadroom(bool secure) const {
  bool tls_in_psram = false;
  size_t free_psram = 0;
  size_t largest_psram = 0;
#if defined(BOARD_HAS_PSRAM)
  // begin() installs the mbedTLS PSRAM hooks only when psramFound() is true,
  // so that is also when the secure-pool numbers apply to PSRAM.
  tls_in_psram = psramFound();
  if (tls_in_psram) {
    free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    largest_psram = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
  }
#endif
  return MQTTObserverConnectionPolicy::hasConnectHeadroom(
      secure, tls_in_psram, heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
      heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
      free_psram, largest_psram);
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
  portENTER_CRITICAL(&_config_mux);
  const char* configured_origin = _config.origin[0] ? _config.origin :
                                  (_origin_source ? _origin_source : "");
  char origin[sizeof(_origin)];
  strncpy(origin, configured_origin, sizeof(origin) - 1);
  origin[sizeof(origin) - 1] = 0;
  if (strncmp(_origin, origin, sizeof(_origin)) != 0) memcpy(_origin, origin, sizeof(_origin));
  portEXIT_CRITICAL(&_config_mux);
  if (_save_state.due()) saveConfig();
}

bool MQTTObserver::hasEnabledBrokerLocked() const {
  for (size_t i = 0; i < MQTT_OBSERVER_MAX_BROKERS; i++) {
    const MQTTObserverBrokerConfig& broker = _config.brokers[i];
    if (broker.enabled && broker.server[0]) return true;
  }
  return false;
}

void MQTTObserver::stageRaw(float snr, float rssi, const uint8_t raw[], int len) {
  // stage() rejects a zero length on its own, so a missing frame simply leaves
  // the next capture to report its packet without raw bytes.
  _raw_stager.stage(raw, len > 0 ? static_cast<size_t>(len) : 0, snr, rssi);
}

bool MQTTObserver::stampEvent(Event& event) {
  // A frame the feed cannot place on a timeline is not worth publishing, so a
  // capture before the clock is valid is dropped and counted.
  struct timeval now;
  gettimeofday(&now, nullptr);
  if (now.tv_sec < kMinimumValidTime) {
    _publish_drops.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  event.timestamp = now.tv_sec;
  event.timestamp_usec = now.tv_usec;
  return true;
}

void MQTTObserver::enqueueEvent(const Event& event) {
  portENTER_CRITICAL(&_queue_mux);
  EventQueue::OfferResult result = _queue.offer(event);
  portEXIT_CRITICAL(&_queue_mux);
  if (result != EventQueue::QUEUED)
    _queue_drops.fetch_add(1, std::memory_order_relaxed);
}

void MQTTObserver::capture(mesh::Packet* packet, float score) {
  if (!packet) return;
  _received.fetch_add(1, std::memory_order_relaxed);
  bool should_capture = false;
  portENTER_CRITICAL(&_config_mux);
  if (_config.rx_enabled && (_config.packets_enabled || _config.raw_enabled))
    should_capture = hasEnabledBrokerLocked();
  portEXIT_CRITICAL(&_config_mux);
  if (!should_capture) return;

  Event event{};
  bool has_raw = _raw_stager.consume(event.raw, sizeof(event.raw), event.raw_len,
                                     event.snr, event.rssi);
  if (!stampEvent(event)) return;
  event.packet = *packet;
  if (!has_raw) {
    event.raw_len = 0;
    event.snr = packet->getSNR();
    event.rssi = 0;
  }
  event.score = score;
  enqueueEvent(event);
}

void MQTTObserver::captureTx(mesh::Packet* packet) {
  if (!packet) return;
  uint8_t mode = 0;
  bool should_capture = false;
  portENTER_CRITICAL(&_config_mux);
  mode = _config.tx_enabled;
  should_capture = mode != 0 && hasEnabledBrokerLocked();
  portEXIT_CRITICAL(&_config_mux);
  if (!should_capture) return;
  if (mode == 2) {
    // Own adverts only: the advert payload begins with the originator's public
    // key, so a match identifies the frame this node sent.
    bool self_advert = _identity && packet->getPayloadType() == PAYLOAD_TYPE_ADVERT &&
        packet->payload_len >= PUB_KEY_SIZE &&
        memcmp(packet->payload, _identity->pub_key, PUB_KEY_SIZE) == 0;
    if (!self_advert) return;
  }

  Event event{};
  if (!stampEvent(event)) return;
  event.packet = *packet;
  event.raw_len = 0;   // no received frame applies to our own transmission
  event.snr = 0;
  event.rssi = 0;
  event.score = 0;
  event.is_tx = true;
  enqueueEvent(event);
}

void MQTTObserver::flushQueue() {
  portENTER_CRITICAL(&_queue_mux);
  size_t discarded = _queue.size();
  _queue.clear();
  portEXIT_CRITICAL(&_queue_mux);
  if (discarded) _disconnected_drops.fetch_add(discarded, std::memory_order_relaxed);
}

void MQTTObserver::trackDisconnectedQueue(uint32_t now) {
  if (MQTTObserverQueuePolicy::trackDisconnected(now, queueSize() == 0,
                                                 _queue_disconnected_since))
    flushQueue();
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
  mesh::Utils::toHex(public_key, _identity->pub_key, PUB_KEY_SIZE);
  snprintf(runtime.jwt_username, sizeof(runtime.jwt_username), "v1_%s", public_key);
  const char header_json[] = "{\"alg\":\"Ed25519\",\"typ\":\"JWT\"}";
  unsigned long issued_at = static_cast<unsigned long>(now);
  unsigned long expiry = issued_at + kJwtLifetimeSeconds;
  char owner[sizeof(config.owner)];
  strncpy(owner, config.owner, sizeof(owner) - 1);
  owner[sizeof(owner) - 1] = 0;
  for (char* p = owner; *p; p++) if (*p >= 'a' && *p <= 'f') *p -= 'a' - 'A';
  char client_version[96], escaped_client[193], escaped_email[129];
  if (!MQTTObserverFormat::buildClientVersion(_firmware, client_version,
                                              sizeof(client_version)) ||
      !jsonEscape(client_version, escaped_client, sizeof(escaped_client)) ||
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
  mesh::Utils::toHex(signature_hex, signature, sizeof(signature));
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

bool MQTTObserver::configureClient(size_t slot, const MQTTObserverConfig& config) {
  if (slot >= MQTT_OBSERVER_MAX_BROKERS) return false;
  const MQTTObserverBrokerConfig& broker = config.brokers[slot];
  BrokerRuntime& runtime = _brokers[slot];

  // Prepare everything that can fail before touching the client: a bad URI or
  // a failed token leaves the stopped client in place for the next retry.
  if (!MQTTObserverConfigCodec::buildServerUri(broker, runtime.server_uri,
                                                sizeof(runtime.server_uri)) ||
      !MQTTObserverConfigCodec::buildClientId(_device_id, slot, runtime.client_id,
                                              sizeof(runtime.client_id))) return false;
  if (broker.audience[0]) {
    runtime.username[0] = 0;
    runtime.password[0] = 0;
    if (!createJwt(slot, broker, runtime.jwt_expiry)) return false;
  } else {
    runtime.jwt_expiry = 0;
    strncpy(runtime.username, broker.username, sizeof(runtime.username) - 1);
    runtime.username[sizeof(runtime.username) - 1] = 0;
    strncpy(runtime.password, broker.password, sizeof(runtime.password) - 1);
    runtime.password[sizeof(runtime.password) - 1] = 0;
  }

  // The clean-session flag and credentials only take effect when the client is
  // initialized. A started client keeps its previous state when connect()
  // reuses it via esp_mqtt_set_config(), so rebuild the client for every
  // configuration change, including a JWT renewal.
  if (runtime.client) {
    delete runtime.client;
    runtime.client = nullptr;
  }
  runtime.client = new (std::nothrow) PsychicMqttClient();
  if (!runtime.client) return false;
  runtime.client->setAutoReconnect(false);
  runtime.client->onConnect([this, slot](bool) {
    BrokerRuntime& rt = _brokers[slot];
    rt.connected_at.store(millis(), std::memory_order_relaxed);
    rt.connected.store(true, std::memory_order_relaxed);
    rt.status_pending.store(true, std::memory_order_relaxed);
    rt.connect_failed.store(false, std::memory_order_relaxed);
    rt.diag_failures.store(0, std::memory_order_relaxed);
    rt.diag_disconnect_ms.store(0, std::memory_order_relaxed);
    rt.diag_connected_ms.store(millis(), std::memory_order_relaxed);
    rt.state.store(STATE_CONNECTED, std::memory_order_relaxed);
  });
  runtime.client->onDisconnect([this, slot](bool) {
    BrokerRuntime& rt = _brokers[slot];
    rt.connected.store(false, std::memory_order_relaxed);
    // A DISCONNECTED with no transport error is how a CONNACK refusal
    // reaches us: the pinned client forwards only transport errors. Record
    // it now so `diag` shows the refusal during the worker's 30 s wait; a
    // transport error immediately before the disconnect keeps its detail.
    uint32_t now = millis();
    uint32_t error_ms = rt.diag_error_ms.load(std::memory_order_relaxed);
    if (!MQTTObserverConnectionPolicy::disconnectBelongsToError(now, error_ms))
      noteDiagError(rt, DIAG_ERR_CLOSED, now);
    rt.diag_disconnect_ms.store(now, std::memory_order_relaxed);
    rt.state.store(STATE_RETRY, std::memory_order_relaxed);
  });
  runtime.client->onError([this, slot](esp_mqtt_error_codes_t error) {
    BrokerRuntime& rt = _brokers[slot];
    uint8_t type = static_cast<uint8_t>(error.error_type);
    // An MQTT event with no category is still an error worth reporting.
    if (type == DIAG_ERR_NONE) type = DIAG_ERR_MQTT;
    rt.diag_error_type.store(type, std::memory_order_relaxed);
    rt.diag_tls_err.store(static_cast<int32_t>(error.esp_tls_last_esp_err),
                          std::memory_order_relaxed);
    rt.diag_tls_stack.store(static_cast<int32_t>(error.esp_tls_stack_err),
                            std::memory_order_relaxed);
    rt.diag_tls_flags.store(static_cast<int32_t>(error.esp_tls_cert_verify_flags),
                            std::memory_order_relaxed);
    rt.diag_sock_errno.store(static_cast<int32_t>(error.esp_transport_sock_errno),
                             std::memory_order_relaxed);
    rt.diag_error_ms.store(millis(), std::memory_order_relaxed);
    rt.state.store(STATE_ERROR, std::memory_order_relaxed);
  });
  runtime.client->onPublish([this, slot](int msg_id) {
    _brokers[slot].probe.notePuback(msg_id);
  });

  runtime.client->setServer(runtime.server_uri);
  runtime.client->setClientId(runtime.client_id);
  runtime.client->setKeepAlive(60);
  esp_mqtt_client_config_t* mqtt_config = runtime.client->getMqttConfig();
  if (mqtt_config) {
#if ESP_IDF_VERSION_MAJOR == 5
    mqtt_config->network.timeout_ms = 2500;
#else
    mqtt_config->network_timeout_ms = 2500;
#endif
  }
  runtime.client->setBufferSize(1280);
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
  if (mqtt_config) mqtt_config->buffer.out_size = 1280;
#endif
  // Resume the broker session on reconnect rather than asking the broker to
  // discard it. A clean reconnect can be closed while the previous session is
  // still settling for the stable client id; a resume/takeover is accepted.
  runtime.client->setCleanSession(false);
#if ESP_IDF_VERSION_MAJOR == 5
  runtime.client->attachArduinoCACertBundle(secureUri(runtime.server_uri));
#else
  if (mqtt_config) {
    mqtt_config->crt_bundle_attach =
        secureUri(runtime.server_uri) ? esp_crt_bundle_attach : nullptr;
  }
#endif
  if (broker.audience[0]) {
    runtime.client->setCredentials(runtime.jwt_username, runtime.jwt_token);
  } else {
    runtime.client->setCredentials(runtime.username[0] ? runtime.username : nullptr,
                                   runtime.password[0] ? runtime.password : nullptr);
  }
  return true;
}

void MQTTObserver::disconnectBroker(size_t slot) {
  if (slot >= MQTT_OBSERVER_MAX_BROKERS) return;
  BrokerRuntime& runtime = _brokers[slot];
  if (runtime.client && (runtime.client_started || runtime.connected.load(std::memory_order_relaxed))) {
    // Graceful disconnect waits indefinitely for a disconnect event in the library.
    runtime.client->forceStop();
  }
  runtime.client_started = false;
  runtime.connected.store(false, std::memory_order_relaxed);
  runtime.status_pending.store(false, std::memory_order_relaxed);
  runtime.diag_next_retry_ms.store(0, std::memory_order_relaxed);
  runtime.probe.reset();   // a new connection restarts the packet ids
}

void MQTTObserver::noteDiagError(BrokerRuntime& runtime, uint8_t type, uint32_t now) {
  runtime.diag_error_type.store(type, std::memory_order_relaxed);
  runtime.diag_error_ms.store(now ? now : 1, std::memory_order_relaxed);
  // The transport details belong to the last MQTT event; an observer-decided
  // failure has none of its own.
  runtime.diag_tls_err.store(0, std::memory_order_relaxed);
  runtime.diag_tls_stack.store(0, std::memory_order_relaxed);
  runtime.diag_tls_flags.store(0, std::memory_order_relaxed);
  runtime.diag_sock_errno.store(0, std::memory_order_relaxed);
}

void MQTTObserver::deferReconnect(size_t slot, uint32_t now, uint8_t reason) {
  if (slot >= MQTT_OBSERVER_MAX_BROKERS) return;
  BrokerRuntime& runtime = _brokers[slot];
  if (reason != DIAG_ERR_NONE) noteDiagError(runtime, reason, now);
  uint8_t failures = runtime.diag_failures.load(std::memory_order_relaxed);
  if (failures < 0xFF)
    runtime.diag_failures.store(failures + 1, std::memory_order_relaxed);
  disconnectBroker(slot);
  uint8_t index = runtime.backoff_index < 5 ? runtime.backoff_index : 4;
  runtime.next_connect_ms = now + kBackoffMs[index];
  if (runtime.backoff_index < 4) runtime.backoff_index++;
  runtime.diag_next_retry_ms.store(runtime.next_connect_ms, std::memory_order_relaxed);
  runtime.state.store(STATE_RETRY, std::memory_order_relaxed);
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
      trackDisconnectedQueue(now);
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
      memcpy(active_ssid, _work_config.wifi_ssid, sizeof(active_ssid));
      memcpy(active_password, _work_config.wifi_password, sizeof(active_password));
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
      trackDisconnectedQueue(now);
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
      trackDisconnectedQueue(now);
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
      if (connection_changed || !runtime.configured ||
          MQTTObserverConnectionPolicy::jwtNeedsRenewal(epoch, runtime.jwt_expiry)) {
        disconnectBroker(i);
        runtime.configured = false;
        runtime.status_retry_pending = false;
        runtime.last_status_ms = 0;
        // a reconfigured slot gets a fresh attempt: the previous failure says
        // nothing about the new credentials
        runtime.connect_failed.store(false, std::memory_order_relaxed);
        runtime.diag_failures.store(0, std::memory_order_relaxed);
        if (!configureClient(i, _work_config)) {
          noteDiagError(runtime, DIAG_ERR_CONFIG, now);
          uint8_t failures = runtime.diag_failures.load(std::memory_order_relaxed);
          if (failures < 0xFF)
            runtime.diag_failures.store(failures + 1, std::memory_order_relaxed);
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

        if (MQTTObserverConnectionPolicy::livenessProbeTimedOut(
                runtime.probe.outstanding(), now, runtime.probe.sentMs())) {
          // An unacknowledged QoS 1 probe is the one failure the local connected
          // flag cannot see; reuse the reconnect ladder instead of looping.
          deferReconnect(i, now, DIAG_ERR_LIVENESS);
          continue;
        }

        bool status_due = runtime.status_pending.exchange(false, std::memory_order_relaxed) ||
            runtime.status_retry_pending || !runtime.last_status_ms ||
            static_cast<uint32_t>(now - runtime.last_status_ms) >=
                static_cast<uint32_t>(_work_config.status_interval_minutes) * 60000UL;
        bool status_retry_ready = MQTTObserverQueuePolicy::retryReady(
            now, runtime.status_retry_ms, runtime.status_retry_pending ? 1 : 0);
        if (_work_config.status_enabled && status_due && status_retry_ready) {
          if (publishStatus(i, _work_config)) {
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
      if (MQTTObserverConnectionPolicy::connectAttemptTimedOut(
              runtime.client_started, now, runtime.attempt_started)) {
        // The attempt never came up: the broker refused it (bad credentials,
        // wrong protocol) or was unreachable. The library only reports
        // transport errors to the application, so the outcome is decided here
        // — and the slot says so instead of cycling "connecting" -> "retry"
        // forever. The ladder keeps retrying in the background.
        runtime.connect_failed.store(true, std::memory_order_relaxed);
        // onError may already have recorded the real reason (a TLS/socket
        // error) during this attempt; keep it instead of replacing it with
        // the generic timeout.
        uint32_t started = runtime.attempt_started;
        uint32_t error_ms = runtime.diag_error_ms.load(std::memory_order_relaxed);
        uint32_t disconnect_ms = runtime.diag_disconnect_ms.load(std::memory_order_relaxed);
        auto source = MQTTObserverConnectionPolicy::attemptFailureSource(
            started, error_ms, disconnect_ms);
        uint8_t reason = DIAG_ERR_TIMEOUT;
        if (source == MQTTObserverConnectionPolicy::AttemptFailureSource::Reported)
          reason = DIAG_ERR_NONE;   // onError already recorded the detail
        else if (source == MQTTObserverConnectionPolicy::AttemptFailureSource::Closed)
          reason = DIAG_ERR_CLOSED;
        deferReconnect(i, now, reason);
        runtime.state.store(STATE_ERROR, std::memory_order_relaxed);
      }
      if (!runtime.client_started && static_cast<int32_t>(now - runtime.next_connect_ms) >= 0) {
        if (hasConnectHeadroom(secureUri(runtime.server_uri))) {
          // Stamp the attempt before starting the client: its task can
          // dispatch an error or disconnect before connect() returns, and
          // the callbacks use this stamp to tell this attempt's failure from
          // the previous one.
          runtime.attempt_started = now;
          runtime.client->connect();
          runtime.client_started = true;
          runtime.diag_attempts.fetch_add(1, std::memory_order_relaxed);
          runtime.diag_disconnect_ms.store(0, std::memory_order_relaxed);
          runtime.diag_next_retry_ms.store(0, std::memory_order_relaxed);
          runtime.state.store(STATE_CONNECTING, std::memory_order_relaxed);
        } else {
          // A second TLS client started without a contiguous internal-heap
          // allocation can abort the device. Defer and retry indefinitely
          // (never a terminal state) until the heap recovers.
          runtime.next_connect_ms = now + 30000;
          runtime.diag_next_retry_ms.store(runtime.next_connect_ms,
                                           std::memory_order_relaxed);
          runtime.state.store(STATE_RETRY, std::memory_order_relaxed);
        }
      } else if (!runtime.client_started && runtime.connect_failed.load(std::memory_order_relaxed)) {
        // waiting for the next attempt: the failed one is still the news
        runtime.state.store(STATE_ERROR, std::memory_order_relaxed);
      }
      State state = runtime.state.load(std::memory_order_relaxed);
      any_connecting = any_connecting || state == STATE_CONNECTING || state == STATE_RETRY;
    }

    _runtime_config = _work_config;
    runtime_config_initialized = true;
    if (any_connected) _state.store(STATE_CONNECTED, std::memory_order_relaxed);
    else if (any_connecting) _state.store(STATE_CONNECTING, std::memory_order_relaxed);
    else _state.store(STATE_RETRY, std::memory_order_relaxed);

    if (any_connected) {
      _queue_disconnected_since = 0;
      EventQueue::Entry queued;
      bool have_event = false;
      portENTER_CRITICAL(&_queue_mux);
      have_event = _queue.beginAttempt(millis(), queued);
      portEXIT_CRITICAL(&_queue_mux);
      if (have_event) {
        bool published = publishEvent(queued.event, _work_config);
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
    } else {
      trackDisconnectedQueue(now);
    }
    vTaskDelay(pdMS_TO_TICKS(queueSize() ? 5 : 50));
  }
}

bool MQTTObserver::buildStatusMessage(char* out, size_t capacity, size_t& written) {
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
  struct timeval now;
  gettimeofday(&now, nullptr);
  char timestamp[40];
  if (!MQTTObserverFormat::formatTimestamp(now.tv_sec, now.tv_usec, timestamp,
                                           sizeof(timestamp))) return false;
  MQTTObserverFormat::StatusStats stats{};
  if (_status_stats_fn) _status_stats_fn(_status_stats_ctx, stats);
  return MQTTObserverFormat::buildStatus(origin, _device_id, _model,
                                         _firmware, radio, timestamp, stats,
                                         out, capacity, written);
}

bool MQTTObserver::publishStatus(size_t slot, const MQTTObserverConfig& config) {
  if (slot >= MQTT_OBSERVER_MAX_BROKERS) return false;
  BrokerRuntime& runtime = _brokers[slot];
  if (!runtime.client || !runtime.client->connected() || !config.status_enabled) return false;
  const MQTTObserverBrokerConfig& broker = config.brokers[slot];
  char topic[128];
  if (!MQTTObserverConfigCodec::buildTopic(config, broker, _device_id, "status",
                                           topic, sizeof(topic))) return false;
  size_t written = 0;
  if (!buildStatusMessage(_json, sizeof(_json), written)) return false;
  int msg_id = runtime.client->publish(topic, 1, false, _json, written, true);
  if (msg_id >= 0) runtime.probe.noteProbe(msg_id, millis());
  return msg_id >= 0;
}

bool MQTTObserver::publishEvent(const Event& event, const MQTTObserverConfig& config) {
  bool any_published = false;
  // A drop is counted per event, not per broker: one event that no broker
  // accepted is one skipped event, whatever the number of brokers.
  bool dropped = false;
  char origin[sizeof(_origin)];
  portENTER_CRITICAL(&_config_mux);
  memcpy(origin, _origin, sizeof(origin));
  portEXIT_CRITICAL(&_config_mux);
  uint32_t now = millis();

  for (size_t slot = 0; slot < MQTT_OBSERVER_MAX_BROKERS; slot++) {
    BrokerRuntime& runtime = _brokers[slot];
    const MQTTObserverBrokerConfig& broker = config.brokers[slot];
    if (!broker.enabled || !runtime.client || !runtime.client->connected()) continue;
    // With status off, a packet (else raw) publish doubles as the liveness
    // probe; at most one QoS 1 message per broker is in flight at a time.
    bool probe_due = MQTTObserverConnectionPolicy::livenessProbeDue(
        config.status_enabled != 0, runtime.probe.outstanding(),
        now, runtime.probe.lastProbeMs(),
        static_cast<uint32_t>(config.status_interval_minutes) * 60000UL);
    int packet_qos = probe_due ? 1 : 0;
    int raw_qos = probe_due && !config.packets_enabled ? 1 : 0;
    char topic[128];
    size_t written = 0;
    if (config.packets_enabled) {
      uint8_t serialized[MAX_TRANS_UNIT];
      const uint8_t* wire = event.raw;
      size_t wire_len = event.raw_len;
      if (!wire_len) {
        int expected = event.packet.getRawLength();
        if (expected <= 0 || expected > MAX_TRANS_UNIT ||
            event.packet.payload_len > MAX_PACKET_PAYLOAD) {
          dropped = true;
          continue;
        }
        wire_len = event.packet.writeTo(serialized);
        if (!wire_len || wire_len > sizeof(serialized)) {
          dropped = true;
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
        event.packet.getPathHashSize(), event.is_tx
      };
      if (MQTTObserverConfigCodec::buildTopic(config, broker, _device_id,
                                              "packets", topic, sizeof(topic)) &&
          MQTTObserverFormat::buildPacket(view, _json, sizeof(_json), written)) {
        int msg_id = runtime.client->publish(topic, packet_qos, false, _json,
                                             written, false);
        if (msg_id >= 0) {
          if (packet_qos) runtime.probe.noteProbe(msg_id, millis());
          _published.fetch_add(1, std::memory_order_relaxed);
          any_published = true;
        } else {
          dropped = true;
        }
      } else {
        dropped = true;
      }
    }

    if (config.raw_enabled && event.raw_len) {
      char timestamp[40];
      if (MQTTObserverFormat::formatTimestamp(event.timestamp, event.timestamp_usec,
                                               timestamp, sizeof(timestamp)) &&
          MQTTObserverConfigCodec::buildTopic(config, broker, _device_id,
                                              "raw", topic, sizeof(topic)) &&
          MQTTObserverFormat::buildRaw(origin, _device_id, timestamp,
                                       event.raw, event.raw_len, _json, sizeof(_json), written)) {
        int msg_id = runtime.client->publish(topic, raw_qos, false, _json,
                                             written, false);
        if (msg_id >= 0) {
          if (raw_qos) runtime.probe.noteProbe(msg_id, millis());
          _published.fetch_add(1, std::memory_order_relaxed);
          any_published = true;
        } else {
          dropped = true;
        }
      } else {
        dropped = true;
      }
    }
  }
  if (dropped && !any_published) _publish_drops.fetch_add(1, std::memory_order_relaxed);
  return any_published;
}

void MQTTObserver::markDirty() {
  _save_state.markDirty();
}

// The device facts the CLI reports: the observer fills them in, the command
// handling itself lives in MQTTObserverCli (host-tested).
void MQTTObserver::cliFacts(MQTTObserverCliFacts& facts) {
  facts.wifi_up = WiFi.status() == WL_CONNECTED;
  facts.wifi_rssi = facts.wifi_up ? WiFi.RSSI() : 0;
  IPAddress ip = WiFi.localIP();
  for (int i = 0; i < 4; i++) facts.wifi_ip[i] = ip[i];
  facts.clock_valid = time(nullptr) >= static_cast<time_t>(kMinimumValidTime);
  facts.free_heap = ESP.getFreeHeap();
  facts.max_heap = ESP.getMaxAllocHeap();
  facts.queue_size = (uint32_t)queueSize();
  facts.queue_capacity = MQTT_OBSERVER_QUEUE_CAPACITY;
  facts.received = _received.load(std::memory_order_relaxed);
  facts.published = _published.load(std::memory_order_relaxed);
  facts.disconnected_drops = _disconnected_drops.load(std::memory_order_relaxed);
  facts.queue_drops = _queue_drops.load(std::memory_order_relaxed);
  facts.publish_drops = _publish_drops.load(std::memory_order_relaxed);
  uint32_t now = millis();
  for (size_t i = 0; i < MQTT_OBSERVER_MAX_BROKERS; i++) {
    const BrokerRuntime& broker = _brokers[i];
    MQTTObserverSlotState state = broker.state.load(std::memory_order_relaxed);
    bool connected = broker.connected.load(std::memory_order_relaxed);
    facts.slot_state[i] = state;
    facts.slot_connected[i] = connected;

    MQTTObserverSlotDiag& diag = facts.slot_diag[i];
    diag.error_type = broker.diag_error_type.load(std::memory_order_relaxed);
    diag.tls_err = broker.diag_tls_err.load(std::memory_order_relaxed);
    diag.tls_stack = broker.diag_tls_stack.load(std::memory_order_relaxed);
    diag.tls_flags = broker.diag_tls_flags.load(std::memory_order_relaxed);
    diag.sock_errno = broker.diag_sock_errno.load(std::memory_order_relaxed);
    diag.attempts = broker.diag_attempts.load(std::memory_order_relaxed);
    diag.failures = broker.diag_failures.load(std::memory_order_relaxed);
    uint32_t error_ms = broker.diag_error_ms.load(std::memory_order_relaxed);
    diag.error_age_s = error_ms ? (now - error_ms) / 1000 : 0;
    uint32_t connected_ms = broker.diag_connected_ms.load(std::memory_order_relaxed);
    diag.connected_age_s = connected && connected_ms ? (now - connected_ms) / 1000 : 0;
    uint32_t retry_ms = broker.diag_next_retry_ms.load(std::memory_order_relaxed);
    if ((state == STATE_RETRY || state == STATE_ERROR) && retry_ms) {
      int32_t remaining = static_cast<int32_t>(retry_ms - now);
      diag.retry_in_s = remaining > 0
                            ? (static_cast<uint32_t>(remaining) + 999) / 1000
                            : 0;
    }
  }
}

bool MQTTObserver::handleCommand(char* command, char* reply) {
  if (!command || !reply) return false;
  MQTTObserverCliFacts facts;
  cliFacts(facts);
  bool changed = false;
  // the CLI reads and writes the config the worker copies from: keep the whole
  // command under the same lock
  portENTER_CRITICAL(&_config_mux);
  bool handled =
      mqttObserverCliCommand(_config, _origin_source, facts, command, reply, &changed);
  portEXIT_CRITICAL(&_config_mux);
  if (changed) markDirty();
  return handled;
}

#endif  // WITH_MQTT_OBSERVER
