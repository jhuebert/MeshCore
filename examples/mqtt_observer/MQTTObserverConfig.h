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
};

namespace MQTTObserverConfigCodec {

static const uint8_t kVersion = 1;
static const size_t kPayloadSize = 4 + 33 + 65 + 129 + 65 + 65 + 65 + 4;

inline void setDefaults(MQTTObserverConfig& config) {
  memset(&config, 0, sizeof(config));
  config.status_enabled = 1;
  config.packets_enabled = 1;
  config.raw_enabled = 1;
}

inline bool validServer(const char* server) {
  if (!server || !*server) return false;
  const char* remainder = nullptr;
  static const char* const schemes[] = {"mqtt://", "mqtts://", "ws://", "wss://"};
  for (size_t i = 0; i < sizeof(schemes) / sizeof(schemes[0]); i++) {
    size_t len = strlen(schemes[i]);
    if (strncmp(server, schemes[i], len) == 0) {
      remainder = server + len;
      break;
    }
  }
  if (!remainder || !*remainder || *remainder == '/' || *remainder == ':' ||
      *remainder == '?' || *remainder == '#') return false;
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
    if (iata[i] < 'A' || iata[i] > 'Z') return false;
  }
  return true;
}

inline bool valid(const MQTTObserverConfig& config) {
  if (config.enabled > 1 || config.status_enabled > 1 ||
      config.packets_enabled > 1 || config.raw_enabled > 1) return false;
  if (!memchr(config.wifi_ssid, 0, sizeof(config.wifi_ssid)) ||
      !memchr(config.wifi_password, 0, sizeof(config.wifi_password)) ||
      !memchr(config.server, 0, sizeof(config.server)) ||
      !memchr(config.username, 0, sizeof(config.username)) ||
      !memchr(config.password, 0, sizeof(config.password)) ||
      !memchr(config.audience, 0, sizeof(config.audience)) ||
      !memchr(config.iata, 0, sizeof(config.iata))) return false;
  if (!validIata(config.iata) || !validAudience(config.audience)) return false;
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
  return pos;
}

inline bool decode(const uint8_t* data, size_t len, MQTTObserverConfig& config) {
  if (!data || len != kPayloadSize) return false;
  MQTTObserverConfig decoded;
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

}  // namespace MQTTObserverConfigCodec
