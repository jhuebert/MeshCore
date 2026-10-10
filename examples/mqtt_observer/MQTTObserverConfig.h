#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct MQTTObserverConfig {
  uint8_t enabled;
  uint8_t status_enabled;
  uint8_t packets_enabled;
  uint8_t raw_enabled;
  char wifi_ssid[33];
  char wifi_password[65];
  char server[129];
  char username[65];
  char password[65];
  char audience[65];
  char iata[4];
  uint8_t rx_enabled;
  uint8_t status_interval_minutes;
  uint16_t port;
  char origin[33];
  char topic[129];
  char token[65];
  char ntp_server[65];
  char owner[65];
  char email[65];
  uint8_t wifi_power_save;
};

namespace MQTTObserverConfigCodec {

static const uint8_t kVersion = 2;
static const size_t kLegacyPayloadSize = 4 + 33 + 65 + 129 + 65 + 65 + 65 + 4;
static const size_t kPayloadSize = kLegacyPayloadSize + 1 + 1 + 2 + 33 + 129 + 65 + 65 + 65 + 65 + 1;

inline void setDefaults(MQTTObserverConfig& config) {
  memset(&config, 0, sizeof(config));
  config.status_enabled = 1;
  config.packets_enabled = 1;
  config.raw_enabled = 1;
  config.rx_enabled = 1;
  config.status_interval_minutes = 5;
  config.port = 1883;
  config.wifi_power_save = 1;
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

inline bool valid(const MQTTObserverConfig& config) {
  if (config.enabled > 1 || config.status_enabled > 1 ||
      config.packets_enabled > 1 || config.raw_enabled > 1 || config.rx_enabled > 1 ||
      config.status_interval_minutes < 1 || config.status_interval_minutes > 60 ||
      config.port == 0 || config.wifi_power_save > 2) return false;
  if (!memchr(config.wifi_ssid, 0, sizeof(config.wifi_ssid)) ||
      !memchr(config.wifi_password, 0, sizeof(config.wifi_password)) ||
      !memchr(config.server, 0, sizeof(config.server)) ||
      !memchr(config.username, 0, sizeof(config.username)) ||
      !memchr(config.password, 0, sizeof(config.password)) ||
      !memchr(config.audience, 0, sizeof(config.audience)) ||
      !memchr(config.iata, 0, sizeof(config.iata)) ||
      !memchr(config.origin, 0, sizeof(config.origin)) ||
      !memchr(config.topic, 0, sizeof(config.topic)) ||
      !memchr(config.token, 0, sizeof(config.token)) ||
      !memchr(config.ntp_server, 0, sizeof(config.ntp_server)) ||
      !memchr(config.owner, 0, sizeof(config.owner)) ||
      !memchr(config.email, 0, sizeof(config.email))) return false;
  if (!validIata(config.iata) || !validAudience(config.audience) ||
      !validTopic(config.topic) || !validTopic(config.token) ||
      !validNtpServer(config.ntp_server) ||
      !validOwner(config.owner) || !validEmail(config.email)) return false;
  return config.server[0] == 0 || validServer(config.server);
}

inline size_t encode(const MQTTObserverConfig& config, uint8_t* out, size_t capacity) {
  if (!out || capacity < kPayloadSize || !valid(config)) return 0;
  size_t pos = 0;
  out[pos++] = config.enabled;
  out[pos++] = config.status_enabled;
  out[pos++] = config.packets_enabled;
  out[pos++] = config.raw_enabled;
#define MQTT_OBSERVER_COPY_FIELD(field) \
  memcpy(out + pos, config.field, sizeof(config.field)); \
  pos += sizeof(config.field)
  MQTT_OBSERVER_COPY_FIELD(wifi_ssid);
  MQTT_OBSERVER_COPY_FIELD(wifi_password);
  MQTT_OBSERVER_COPY_FIELD(server);
  MQTT_OBSERVER_COPY_FIELD(username);
  MQTT_OBSERVER_COPY_FIELD(password);
  MQTT_OBSERVER_COPY_FIELD(audience);
  MQTT_OBSERVER_COPY_FIELD(iata);
#undef MQTT_OBSERVER_COPY_FIELD
  out[pos++] = config.rx_enabled;
  out[pos++] = config.status_interval_minutes;
  out[pos++] = config.port & 0xff;
  out[pos++] = config.port >> 8;
#define MQTT_OBSERVER_COPY_TAIL(field) \
  memcpy(out + pos, config.field, sizeof(config.field)); \
  pos += sizeof(config.field)
  MQTT_OBSERVER_COPY_TAIL(origin);
  MQTT_OBSERVER_COPY_TAIL(topic);
  MQTT_OBSERVER_COPY_TAIL(token);
  MQTT_OBSERVER_COPY_TAIL(ntp_server);
  MQTT_OBSERVER_COPY_TAIL(owner);
  MQTT_OBSERVER_COPY_TAIL(email);
#undef MQTT_OBSERVER_COPY_TAIL
  out[pos++] = config.wifi_power_save;
  return pos;
}

inline bool decodeLegacyV1(const uint8_t* data, size_t len, MQTTObserverConfig& config) {
  if (!data || len != kLegacyPayloadSize) return false;
  MQTTObserverConfig decoded;
  setDefaults(decoded);
  size_t pos = 0;
  decoded.enabled = data[pos++];
  decoded.status_enabled = data[pos++];
  decoded.packets_enabled = data[pos++];
  decoded.raw_enabled = data[pos++];
#define MQTT_OBSERVER_READ_FIELD(field) \
  memcpy(decoded.field, data + pos, sizeof(decoded.field)); \
  pos += sizeof(decoded.field)
  MQTT_OBSERVER_READ_FIELD(wifi_ssid);
  MQTT_OBSERVER_READ_FIELD(wifi_password);
  MQTT_OBSERVER_READ_FIELD(server);
  MQTT_OBSERVER_READ_FIELD(username);
  MQTT_OBSERVER_READ_FIELD(password);
  MQTT_OBSERVER_READ_FIELD(audience);
  MQTT_OBSERVER_READ_FIELD(iata);
#undef MQTT_OBSERVER_READ_FIELD
  if (strcmp(decoded.iata, "XXX") == 0) decoded.iata[0] = 0;
  if (!valid(decoded)) return false;
  config = decoded;
  return true;
}

inline bool decode(const uint8_t* data, size_t len, MQTTObserverConfig& config) {
  if (!data || len != kPayloadSize) return false;
  MQTTObserverConfig decoded;
  setDefaults(decoded);
  size_t pos = 0;
  decoded.enabled = data[pos++];
  decoded.status_enabled = data[pos++];
  decoded.packets_enabled = data[pos++];
  decoded.raw_enabled = data[pos++];
#define MQTT_OBSERVER_READ_FIELD(field) \
  memcpy(decoded.field, data + pos, sizeof(decoded.field)); \
  pos += sizeof(decoded.field)
  MQTT_OBSERVER_READ_FIELD(wifi_ssid);
  MQTT_OBSERVER_READ_FIELD(wifi_password);
  MQTT_OBSERVER_READ_FIELD(server);
  MQTT_OBSERVER_READ_FIELD(username);
  MQTT_OBSERVER_READ_FIELD(password);
  MQTT_OBSERVER_READ_FIELD(audience);
  MQTT_OBSERVER_READ_FIELD(iata);
#undef MQTT_OBSERVER_READ_FIELD
  decoded.rx_enabled = data[pos++];
  decoded.status_interval_minutes = data[pos++];
  decoded.port = static_cast<uint16_t>(data[pos]) |
                 (static_cast<uint16_t>(data[pos + 1]) << 8);
  pos += 2;
#define MQTT_OBSERVER_READ_TAIL(field) \
  memcpy(decoded.field, data + pos, sizeof(decoded.field)); \
  pos += sizeof(decoded.field)
  MQTT_OBSERVER_READ_TAIL(origin);
  MQTT_OBSERVER_READ_TAIL(topic);
  MQTT_OBSERVER_READ_TAIL(token);
  MQTT_OBSERVER_READ_TAIL(ntp_server);
  MQTT_OBSERVER_READ_TAIL(owner);
  MQTT_OBSERVER_READ_TAIL(email);
#undef MQTT_OBSERVER_READ_TAIL
  decoded.wifi_power_save = data[pos++];
  if (!valid(decoded)) return false;
  config = decoded;
  return true;
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

inline uint16_t configuredPort(const MQTTObserverConfig& config) {
  const char* scheme = strstr(config.server, "://");
  if (!scheme) return config.port;
  const char* authority = scheme + 3;
  const char* end = strpbrk(authority, "/?#");
  if (!end) end = config.server + strlen(config.server);
  const char* colon = nullptr;
  if (authority < end && *authority == '[') {
    const char* close = static_cast<const char*>(memchr(authority, ']', end - authority));
    if (close && close + 1 < end && close[1] == ':') colon = close + 1;
  } else {
    for (const char* p = authority; p < end; p++) if (*p == ':') colon = p;
  }
  if (!colon || colon + 1 == end) {
    if (config.port == 1883) {
      size_t scheme_len = static_cast<size_t>(scheme - config.server);
      if (scheme_len == 5 && strncmp(config.server, "mqtts", 5) == 0) return 8883;
      if (scheme_len == 3 && strncmp(config.server, "wss", 3) == 0) return 443;
      if (scheme_len == 2 && strncmp(config.server, "ws", 2) == 0) return 80;
    }
    return config.port;
  }
  unsigned port = 0;
  for (const char* p = colon + 1; p < end; p++) {
    if (*p < '0' || *p > '9') return config.port;
    port = port * 10 + static_cast<unsigned>(*p - '0');
    if (port > 65535) return config.port;
  }
  return port ? static_cast<uint16_t>(port) : config.port;
}

inline bool buildServerUri(const MQTTObserverConfig& config, char* out, size_t capacity) {
  const char* server = config.server;
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
    uint16_t port = config.port;
    size_t scheme_len = static_cast<size_t>(scheme - server);
    if (port == 1883 && scheme_len == 5 && strncmp(server, "mqtts", 5) == 0) port = 8883;
    else if (port == 1883 && scheme_len == 3 && strncmp(server, "wss", 3) == 0) port = 443;
    else if (port == 1883 && scheme_len == 2 && strncmp(server, "ws", 2) == 0) port = 80;
    size_t prefix_len = static_cast<size_t>(end - server);
    int len = snprintf(out, capacity, "%.*s:%u%s", static_cast<int>(prefix_len), server,
                       static_cast<unsigned>(port), path ? path : "");
    return len > 0 && static_cast<size_t>(len) < capacity;
  }
  const char* protocol = config.port == 8883 ? "mqtts" : config.port == 443 ? "wss" : "mqtt";
  int len = snprintf(out, capacity, "%s://%s:%u", protocol, server,
                     static_cast<unsigned>(config.port));
  return len > 0 && static_cast<size_t>(len) < capacity;
}

inline bool buildTopic(const MQTTObserverConfig& config, const char* device_id,
                       const char* leaf, char* out, size_t capacity) {
  const char* format = config.topic[0] ? config.topic : "meshcore/{iata}/{device}/{type}";
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
      value = config.token;
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
