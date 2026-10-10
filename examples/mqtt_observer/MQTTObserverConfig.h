#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef MQTT_OBSERVER_MAX_BROKERS
#define MQTT_OBSERVER_MAX_BROKERS 2  // Active broker clients configured by mqttN.*
#endif

// Reserve the full slot array so increasing the active count does not move the record layout.
static const size_t MQTT_OBSERVER_CONFIG_BROKER_CAPACITY = 6;

#ifndef MQTT_OBSERVER_QUEUE_CAPACITY
#define MQTT_OBSERVER_QUEUE_CAPACITY 8  // Captured packet events shared by all broker slots
#endif

static_assert(MQTT_OBSERVER_MAX_BROKERS > 0 &&
              MQTT_OBSERVER_MAX_BROKERS <= MQTT_OBSERVER_CONFIG_BROKER_CAPACITY,
              "active MQTT broker count exceeds stored slot capacity");
static_assert(MQTT_OBSERVER_QUEUE_CAPACITY > 0, "MQTT observer queue must have capacity");

struct MQTTObserverBrokerConfig {
  uint8_t enabled;
  uint16_t port;
  char server[129];
  char username[65];
  char password[65];
  char audience[65];
  char topic[129];
  char token[65];
  char owner[65];
  char email[65];
};

struct MQTTObserverConfig {
  uint8_t status_enabled;
  uint8_t packets_enabled;
  uint8_t raw_enabled;
  uint8_t rx_enabled;
  uint8_t status_interval_minutes;
  uint8_t wifi_power_save;
  char wifi_ssid[33];
  char wifi_password[65];
  char iata[4];
  char origin[33];
  char ntp_server[65];
  MQTTObserverBrokerConfig brokers[MQTT_OBSERVER_CONFIG_BROKER_CAPACITY];
};

namespace MQTTObserverConfigCodec {

static const uint8_t kVersion = 3;
static const size_t kCommonPayloadSize = 6 + 33 + 65 + 4 + 33 + 65;
static const size_t kBrokerPayloadSize = 1 + 2 + 129 + 65 + 65 + 65 + 129 + 65 + 65 + 65;
static const size_t kPayloadSize = kCommonPayloadSize +
                                   MQTT_OBSERVER_CONFIG_BROKER_CAPACITY * kBrokerPayloadSize;

inline void setDefaults(MQTTObserverConfig& config) {
  memset(&config, 0, sizeof(config));
  config.status_enabled = 1;
  config.packets_enabled = 1;
  config.raw_enabled = 1;
  config.rx_enabled = 1;
  config.status_interval_minutes = 5;
  config.wifi_power_save = 1;
  for (size_t i = 0; i < MQTT_OBSERVER_CONFIG_BROKER_CAPACITY; i++)
    config.brokers[i].port = 1883;
}

inline bool validServer(const char* server) {
  if (!server || !*server) return false;
  const char* remainder = server;
  const char* scheme = strstr(server, "://");
  if (scheme) {
    static const char* const schemes[] = {"mqtt", "mqtts", "ws", "wss"};
    bool supported = false;
    for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++) {
      if (static_cast<size_t>(scheme - server) == strlen(schemes[i]) &&
          strncmp(server, schemes[i], strlen(schemes[i])) == 0) {
        supported = true;
        break;
      }
    }
    if (!supported) return false;
    remainder = scheme + 3;
  }
  if (!*remainder || *remainder == '/' || *remainder == ':' ||
      *remainder == '?' || *remainder == '#') return false;
  if (!scheme) {
    for (const unsigned char* p = (const unsigned char*)remainder; *p; p++) {
      if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
            (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' ||
            *p == '[' || *p == ']')) return false;
    }
  }
  for (const unsigned char* p = (const unsigned char*)server; *p; p++) {
    if (*p <= 0x20 || *p == 0x7f || *p == '@') return false;
  }
  return true;
}

inline bool validAudience(const char* audience) {
  if (!audience) return false;
  for (const unsigned char* p = (const unsigned char*)audience; *p; p++) {
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '.' || *p == '-' ||
          *p == '_' || *p == ':' || *p == '/')) return false;
  }
  return true;
}

inline bool validIata(const char* iata) {
  if (!iata || !*iata) return true;
  if (strlen(iata) != 3) return false;
  for (int i = 0; i < 3; i++) {
    if (!(iata[i] >= 'A' && iata[i] <= 'Z') &&
        !(iata[i] >= '0' && iata[i] <= '9')) return false;
  }
  return strcmp(iata, "XXX") != 0;
}

inline bool validTopic(const char* topic) {
  if (!topic) return false;
  for (const unsigned char* p = (const unsigned char*)topic; *p; p++) {
    if (*p <= 0x20 || *p == 0x7f || *p == '+' || *p == '#') return false;
  }
  return true;
}

inline bool validOwner(const char* owner) {
  if (!owner || !*owner) return true;
  if (strlen(owner) != 64) return false;
  for (const unsigned char* p = (const unsigned char*)owner; *p; p++) {
    if (!((*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F') ||
          (*p >= '0' && *p <= '9'))) return false;
  }
  return true;
}

inline bool validEmail(const char* email) {
  if (!email) return false;
  for (const unsigned char* p = (const unsigned char*)email; *p; p++)
    if (*p < 0x20 || *p == 0x7f) return false;
  return true;
}

inline bool validNtpServer(const char* server) {
  if (!server) return false;
  for (const unsigned char* p = (const unsigned char*)server; *p; p++) {
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
          (*p >= '0' && *p <= '9') || *p == '.' || *p == '-')) return false;
  }
  return true;
}

inline bool validBroker(const MQTTObserverBrokerConfig& broker) {
  if (broker.enabled > 1 || broker.port == 0) return false;
  if (!memchr(broker.server, 0, sizeof(broker.server)) ||
      !memchr(broker.username, 0, sizeof(broker.username)) ||
      !memchr(broker.password, 0, sizeof(broker.password)) ||
      !memchr(broker.audience, 0, sizeof(broker.audience)) ||
      !memchr(broker.topic, 0, sizeof(broker.topic)) ||
      !memchr(broker.token, 0, sizeof(broker.token)) ||
      !memchr(broker.owner, 0, sizeof(broker.owner)) ||
      !memchr(broker.email, 0, sizeof(broker.email))) return false;
  if (!validAudience(broker.audience) || !validTopic(broker.topic) ||
      !validTopic(broker.token) || !validOwner(broker.owner) ||
      !validEmail(broker.email)) return false;
  return broker.server[0] == 0 || validServer(broker.server);
}

inline bool valid(const MQTTObserverConfig& config) {
  if (config.status_enabled > 1 || config.packets_enabled > 1 || config.raw_enabled > 1 ||
      config.rx_enabled > 1 || config.status_interval_minutes < 1 ||
      config.status_interval_minutes > 60 || config.wifi_power_save > 2) return false;
  if (!memchr(config.wifi_ssid, 0, sizeof(config.wifi_ssid)) ||
      !memchr(config.wifi_password, 0, sizeof(config.wifi_password)) ||
      !memchr(config.iata, 0, sizeof(config.iata)) ||
      !memchr(config.origin, 0, sizeof(config.origin)) ||
      !memchr(config.ntp_server, 0, sizeof(config.ntp_server)) ||
      !validIata(config.iata) || !validNtpServer(config.ntp_server)) return false;
  for (size_t i = 0; i < MQTT_OBSERVER_CONFIG_BROKER_CAPACITY; i++)
    if (!validBroker(config.brokers[i])) return false;
  return true;
}

inline size_t encode(const MQTTObserverConfig& config, uint8_t* out, size_t capacity) {
  if (!out || capacity < kPayloadSize || !valid(config)) return 0;
  size_t pos = 0;
  out[pos++] = config.status_enabled;
  out[pos++] = config.packets_enabled;
  out[pos++] = config.raw_enabled;
  out[pos++] = config.rx_enabled;
  out[pos++] = config.status_interval_minutes;
  out[pos++] = config.wifi_power_save;
#define MQTT_OBSERVER_COPY_GLOBAL(field) \
  memcpy(out + pos, config.field, sizeof(config.field)); \
  pos += sizeof(config.field)
  MQTT_OBSERVER_COPY_GLOBAL(wifi_ssid);
  MQTT_OBSERVER_COPY_GLOBAL(wifi_password);
  MQTT_OBSERVER_COPY_GLOBAL(iata);
  MQTT_OBSERVER_COPY_GLOBAL(origin);
  MQTT_OBSERVER_COPY_GLOBAL(ntp_server);
#undef MQTT_OBSERVER_COPY_GLOBAL
  for (size_t i = 0; i < MQTT_OBSERVER_CONFIG_BROKER_CAPACITY; i++) {
    const MQTTObserverBrokerConfig& broker = config.brokers[i];
    out[pos++] = broker.enabled;
    out[pos++] = broker.port & 0xff;
    out[pos++] = broker.port >> 8;
#define MQTT_OBSERVER_COPY_BROKER(field) \
    memcpy(out + pos, broker.field, sizeof(broker.field)); \
    pos += sizeof(broker.field)
    MQTT_OBSERVER_COPY_BROKER(server);
    MQTT_OBSERVER_COPY_BROKER(username);
    MQTT_OBSERVER_COPY_BROKER(password);
    MQTT_OBSERVER_COPY_BROKER(audience);
    MQTT_OBSERVER_COPY_BROKER(topic);
    MQTT_OBSERVER_COPY_BROKER(token);
    MQTT_OBSERVER_COPY_BROKER(owner);
    MQTT_OBSERVER_COPY_BROKER(email);
#undef MQTT_OBSERVER_COPY_BROKER
  }
  return pos;
}

inline bool decode(const uint8_t* data, size_t len, MQTTObserverConfig& config) {
  if (!data || len != kPayloadSize) return false;
  setDefaults(config);
  size_t pos = 0;
  config.status_enabled = data[pos++];
  config.packets_enabled = data[pos++];
  config.raw_enabled = data[pos++];
  config.rx_enabled = data[pos++];
  config.status_interval_minutes = data[pos++];
  config.wifi_power_save = data[pos++];
#define MQTT_OBSERVER_READ_GLOBAL(field) \
  memcpy(config.field, data + pos, sizeof(config.field)); \
  pos += sizeof(config.field)
  MQTT_OBSERVER_READ_GLOBAL(wifi_ssid);
  MQTT_OBSERVER_READ_GLOBAL(wifi_password);
  MQTT_OBSERVER_READ_GLOBAL(iata);
  MQTT_OBSERVER_READ_GLOBAL(origin);
  MQTT_OBSERVER_READ_GLOBAL(ntp_server);
#undef MQTT_OBSERVER_READ_GLOBAL
  for (size_t i = 0; i < MQTT_OBSERVER_CONFIG_BROKER_CAPACITY; i++) {
    MQTTObserverBrokerConfig& broker = config.brokers[i];
    broker.enabled = data[pos++];
    broker.port = static_cast<uint16_t>(data[pos]) |
                  (static_cast<uint16_t>(data[pos + 1]) << 8);
    pos += 2;
#define MQTT_OBSERVER_READ_BROKER(field) \
    memcpy(broker.field, data + pos, sizeof(broker.field)); \
    pos += sizeof(broker.field)
    MQTT_OBSERVER_READ_BROKER(server);
    MQTT_OBSERVER_READ_BROKER(username);
    MQTT_OBSERVER_READ_BROKER(password);
    MQTT_OBSERVER_READ_BROKER(audience);
    MQTT_OBSERVER_READ_BROKER(topic);
    MQTT_OBSERVER_READ_BROKER(token);
    MQTT_OBSERVER_READ_BROKER(owner);
    MQTT_OBSERVER_READ_BROKER(email);
#undef MQTT_OBSERVER_READ_BROKER
  }
  if (valid(config)) return true;
  setDefaults(config);
  return false;
}

inline void hostLabel(const char* server, char* out, size_t capacity) {
  if (!out || capacity == 0) return;
  out[0] = 0;
  if (!server) return;
  const char* host = strstr(server, "://");
  host = host ? host + 3 : server;
  size_t len = 0;
  while (host[len] && host[len] != '/' && host[len] != '?' && host[len] != '#') len++;
  if (len >= capacity) len = capacity - 1;
  memcpy(out, host, len);
  out[len] = 0;
}

inline uint16_t configuredPort(const MQTTObserverBrokerConfig& broker) {
  const char* scheme = strstr(broker.server, "://");
  if (!scheme) return broker.port;
  const char* authority = scheme + 3;
  const char* end = strpbrk(authority, "/?#");
  if (!end) end = broker.server + strlen(broker.server);
  const char* colon = nullptr;
  if (authority < end && *authority == '[') {
    const char* close = static_cast<const char*>(memchr(authority, ']', end - authority));
    if (close && close + 1 < end && close[1] == ':') colon = close + 1;
  } else {
    for (const char* p = authority; p < end; p++) if (*p == ':') colon = p;
  }
  if (!colon || colon + 1 == end) {
    if (broker.port == 1883) {
      size_t scheme_len = static_cast<size_t>(scheme - broker.server);
      if (scheme_len == 5 && strncmp(broker.server, "mqtts", 5) == 0) return 8883;
      if (scheme_len == 3 && strncmp(broker.server, "wss", 3) == 0) return 443;
      if (scheme_len == 2 && strncmp(broker.server, "ws", 2) == 0) return 80;
    }
    return broker.port;
  }
  unsigned port = 0;
  for (const char* p = colon + 1; p < end; p++) {
    if (*p < '0' || *p > '9') return broker.port;
    port = port * 10 + static_cast<unsigned>(*p - '0');
    if (port > 65535) return broker.port;
  }
  return port ? static_cast<uint16_t>(port) : broker.port;
}

inline bool buildServerUri(const MQTTObserverBrokerConfig& broker, char* out, size_t capacity) {
  const char* server = broker.server;
  const char* scheme = strstr(server, "://");
  if (scheme) {
    const char* authority = scheme + 3;
    const char* path = strpbrk(authority, "/?#");
    const char* end = path ? path : server + strlen(server);
    bool has_port = false;
    if (authority < end && *authority == '[') {
      const char* close = static_cast<const char*>(memchr(authority, ']', end - authority));
      has_port = close && close + 1 < end && close[1] == ':';
    } else {
      has_port = memchr(authority, ':', end - authority) != nullptr;
    }
    if (has_port) {
      int len = snprintf(out, capacity, "%s", server);
      return len > 0 && static_cast<size_t>(len) < capacity;
    }
    uint16_t port = broker.port;
    size_t scheme_len = static_cast<size_t>(scheme - server);
    if (port == 1883 && scheme_len == 5 && strncmp(server, "mqtts", 5) == 0) port = 8883;
    else if (port == 1883 && scheme_len == 3 && strncmp(server, "wss", 3) == 0) port = 443;
    else if (port == 1883 && scheme_len == 2 && strncmp(server, "ws", 2) == 0) port = 80;
    size_t prefix_len = static_cast<size_t>(end - server);
    int len = snprintf(out, capacity, "%.*s:%u%s", static_cast<int>(prefix_len), server,
                       static_cast<unsigned>(port), path ? path : "");
    return len > 0 && static_cast<size_t>(len) < capacity;
  }
  const char* protocol = broker.port == 8883 ? "mqtts" : broker.port == 443 ? "wss" : "mqtt";
  int len = snprintf(out, capacity, "%s://%s:%u", protocol, server,
                     static_cast<unsigned>(broker.port));
  return len > 0 && static_cast<size_t>(len) < capacity;
}

inline bool buildTopic(const MQTTObserverConfig& config,
                       const MQTTObserverBrokerConfig& broker,
                       const char* device_id, const char* leaf, char* out, size_t capacity) {
  const char* format = broker.topic[0] ? broker.topic : "meshcore/{iata}/{device}/{type}";
  size_t used = 0;
  if (!out || capacity == 0 || !device_id || !leaf) return false;
  out[0] = 0;
  while (*format) {
    const char* value = nullptr;
    size_t skip = 0;
    if (strncmp(format, "{iata}", 6) == 0) {
      value = config.iata;
      skip = 6;
    } else if (strncmp(format, "{device}", 8) == 0) {
      value = device_id;
      skip = 8;
    } else if (strncmp(format, "{token}", 7) == 0) {
      value = broker.token;
      skip = 7;
    } else if (strncmp(format, "{type}", 6) == 0) {
      value = leaf;
      skip = 6;
    }
    if (value) {
      size_t len = strlen(value);
      if (used + len >= capacity) return false;
      memcpy(out + used, value, len);
      used += len;
      out[used] = 0;
      format += skip;
    } else {
      if (used + 1 >= capacity) return false;
      out[used++] = *format++;
      out[used] = 0;
    }
  }
  return used > 0;
}

}  // namespace MQTTObserverConfigCodec