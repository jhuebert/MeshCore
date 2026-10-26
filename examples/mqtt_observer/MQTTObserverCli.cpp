// MQTTObserverCli.cpp — the observer's CLI command handling. See
// MQTTObserverCli.h for what this unit is and why it is separate from the
// observer itself.
//
// Every reply string here is the observer's documented API (MQTT_OBSERVER.md),
// and the device state the commands report arrives in MQTTObserverCliFacts
// instead of being read from the Wi-Fi stack and the broker clients.

#include "MQTTObserverCli.h"
#include "MQTTObserverPolicy.h"
#include "../simple_repeater/CliUtil.h"

namespace {

// Copy a value into a fixed-size field, refusing one that does not fit. The
// field is cleared first, so a shorter value leaves no stale bytes behind it
// (the config record writes the whole field).
bool setString(char* destination, size_t capacity, const char* value) {
  if (!destination || !value || strlen(value) >= capacity) return false;
  memset(destination, 0, capacity);
  memcpy(destination, value, strlen(value));
  return true;
}

bool parseToggle(const char* value, uint8_t& enabled) {
  if (strcmp(value, "on") == 0) { enabled = 1; return true; }
  if (strcmp(value, "off") == 0) { enabled = 0; return true; }
  return false;
}

// The broker slot a command names, and the property after it.
bool commandSlot(const char* command, const char* operation, size_t& slot,
                 const char*& property) {
  return MQTTObserverCommandPolicy::parseSlot(command, operation,
                                               MQTT_OBSERVER_MAX_BROKERS, slot, property);
}

}  // namespace

const char* mqttObserverStateName(MQTTObserverSlotState state) {
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

void mqttObserverCliStatus(const MQTTObserverConfig& config,
                            const MQTTObserverCliFacts& facts, char* reply, size_t capacity) {
  if (!reply || capacity == 0) return;
  size_t connected = 0;
  for (size_t i = 0; i < MQTT_OBSERVER_MAX_BROKERS; i++)
    if (facts.slot_connected[i]) connected++;
  // the configured slots, not the build-time maximum: one broker configured
  // reads "slots:0/1", not "slots:0/2"
  size_t configured = 0;
  for (size_t i = 0; i < MQTT_OBSERVER_MAX_BROKERS; i++)
    if (config.brokers[i].enabled) configured++;
  snprintf(reply, capacity,
      "mqtt wifi:%s time:%s slots:%u/%u rx:%lu pub:%lu drop:%lu q:%u/%u heap:%u/%u",
      facts.wifi_up ? "up" : "down", facts.clock_valid ? "ok" : "wait",
      static_cast<unsigned>(connected),
      static_cast<unsigned>(configured),
      static_cast<unsigned long>(facts.received),
      static_cast<unsigned long>(facts.published),
      static_cast<unsigned long>(facts.disconnected_drops + facts.queue_drops +
                                  facts.publish_drops),
      static_cast<unsigned>(facts.queue_size),
      static_cast<unsigned>(facts.queue_capacity),
      static_cast<unsigned>(facts.free_heap), static_cast<unsigned>(facts.max_heap));
}

namespace {

// ------------------------------------------------------------------- set

bool handleSetCommand(MQTTObserverConfig& config, const char* command, char* reply,
                      bool* changed) {
  uint8_t toggle = 0;
  const char* common_property = nullptr;
  const char* common_value = cliCommandValue(command, "set mqtt.iata");
  if (common_value) common_property = "iata";
  else if ((common_value = cliCommandValue(command, "set mqtt.origin"))) common_property = "origin";
  else if ((common_value = cliCommandValue(command, "set mqtt.ntp"))) common_property = "ntp";
  else if ((common_value = cliCommandValue(command, "set mqtt.rx"))) common_property = "rx";
  else if ((common_value = cliCommandValue(command, "set mqtt.status"))) common_property = "status";
  else if ((common_value = cliCommandValue(command, "set mqtt.packets"))) common_property = "packets";
  else if ((common_value = cliCommandValue(command, "set mqtt.raw"))) common_property = "raw";
  else if ((common_value = cliCommandValue(command, "set mqtt.tx"))) common_property = "tx";
  else if ((common_value = cliCommandValue(command, "set mqtt.interval"))) common_property = "interval";
  if (common_property) {
    if (strcmp(common_property, "iata") == 0) {
      char iata[sizeof(config.iata)];
      if (strlen(common_value) >= sizeof(iata)) {
        snprintf(reply, CLI_REPLY_MAX, "Err - IATA must be 3 letters/digits; XXX is reserved");
        return true;
      }
      for (size_t i = 0; i <= strlen(common_value); i++)
        iata[i] = common_value[i] >= 'a' && common_value[i] <= 'z' ?
                  common_value[i] - 'a' + 'A' : common_value[i];
      if (!MQTTObserverConfigCodec::validIata(iata)) {
        snprintf(reply, CLI_REPLY_MAX, "Err - IATA must be 3 letters/digits; XXX is reserved");
        return true;
      }
      *changed = strcmp(config.iata, iata) != 0 &&
                 setString(config.iata, sizeof(config.iata), iata);
    } else if (strcmp(common_property, "origin") == 0) {
      if (strlen(common_value) >= sizeof(config.origin)) {
        snprintf(reply, CLI_REPLY_MAX, "Err - origin too long");
        return true;
      }
      *changed = strcmp(config.origin, common_value) != 0 &&
                 setString(config.origin, sizeof(config.origin), common_value);
    } else if (strcmp(common_property, "ntp") == 0) {
      const char* ntp = strcmp(common_value, "none") == 0 ? "" : common_value;
      if (!MQTTObserverConfigCodec::validNtpServer(ntp) ||
          strlen(ntp) >= sizeof(config.ntp_server)) {
        snprintf(reply, CLI_REPLY_MAX, "Err - invalid NTP server");
        return true;
      }
      *changed = strcmp(config.ntp_server, ntp) != 0 &&
                 setString(config.ntp_server, sizeof(config.ntp_server), ntp);
    } else if (strcmp(common_property, "interval") == 0) {
      long minutes;
      if (!parseIntRange(common_value, 1, 60, &minutes)) {
        snprintf(reply, CLI_REPLY_MAX, "Err - interval must be 1-60 minutes");
        return true;
      }
      *changed = config.status_interval_minutes != minutes;
      config.status_interval_minutes = static_cast<uint8_t>(minutes);
    } else if (strcmp(common_property, "tx") == 0) {
      uint8_t mode;
      if (strcmp(common_value, "off") == 0) mode = 0;
      else if (strcmp(common_value, "on") == 0) mode = 1;
      else if (strcmp(common_value, "advert") == 0) mode = 2;
      else {
        snprintf(reply, CLI_REPLY_MAX, "Err - tx must be off, on, or advert");
        return true;
      }
      *changed = config.tx_enabled != mode;
      config.tx_enabled = mode;
    } else {
      if (!parseToggle(common_value, toggle)) {
        snprintf(reply, CLI_REPLY_MAX, "Err - expected on or off");
        return true;
      }
      uint8_t* field = strcmp(common_property, "rx") == 0 ? &config.rx_enabled :
                       strcmp(common_property, "status") == 0 ? &config.status_enabled :
                       strcmp(common_property, "packets") == 0 ? &config.packets_enabled :
                       &config.raw_enabled;
      *changed = *field != toggle;
      *field = toggle;
    }
    snprintf(reply, CLI_REPLY_MAX, "OK - MQTT setting saved");
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
    if (!value) value = "";
    if (!parseToggle(value, enabled)) {
      snprintf(reply, CLI_REPLY_MAX, "Err - expected on or off");
      return true;
    }
    *changed = config.brokers[slot].enabled != enabled;
    config.brokers[slot].enabled = enabled;
    snprintf(reply, CLI_REPLY_MAX, "OK - MQTT%u %s", static_cast<unsigned>(slot + 1),
             enabled ? "enabled" : "disabled");
    return true;
  }

  if (MQTT_PROP_IS("port")) {
    long port;
    if (!value) value = "";
    if (!parseIntRange(value, 1, 65535, &port)) {
      snprintf(reply, CLI_REPLY_MAX, "Err - port must be 1-65535");
      return true;
    }
    *changed = config.brokers[slot].port != port;
    config.brokers[slot].port = static_cast<uint16_t>(port);
    snprintf(reply, CLI_REPLY_MAX, "OK - MQTT%u port saved", static_cast<unsigned>(slot + 1));
    return true;
  }

  if (MQTT_PROP_IS("server") || MQTT_PROP_IS("username") ||
      MQTT_PROP_IS("password") || MQTT_PROP_IS("audience") ||
      MQTT_PROP_IS("topic") || MQTT_PROP_IS("token") ||
      MQTT_PROP_IS("owner") || MQTT_PROP_IS("email")) {
    if (!value) value = "";
    size_t capacity = 0;
    enum Validation { NONE, SERVER, AUDIENCE, TOPIC, TOPIC_TEMPLATE, OWNER, EMAIL } validation = NONE;
    if (MQTT_PROP_IS("server")) { capacity = sizeof(config.brokers[slot].server); validation = SERVER; }
    else if (MQTT_PROP_IS("username")) capacity = sizeof(config.brokers[slot].username);
    else if (MQTT_PROP_IS("password")) capacity = sizeof(config.brokers[slot].password);
    else if (MQTT_PROP_IS("audience")) { capacity = sizeof(config.brokers[slot].audience); validation = AUDIENCE; }
    else if (MQTT_PROP_IS("topic")) { capacity = sizeof(config.brokers[slot].topic); validation = TOPIC_TEMPLATE; }
    else if (MQTT_PROP_IS("token")) { capacity = sizeof(config.brokers[slot].token); validation = TOPIC; }
    else if (MQTT_PROP_IS("owner")) { capacity = sizeof(config.brokers[slot].owner); validation = OWNER; }
    else { capacity = sizeof(config.brokers[slot].email); validation = EMAIL; }
    const char* setting = value;   // empty value clears the setting
    if (strlen(setting) >= capacity ||
        (validation == SERVER && *setting && !MQTTObserverConfigCodec::validServer(setting)) ||
        (validation == AUDIENCE && !MQTTObserverConfigCodec::validAudience(setting)) ||
        (validation == TOPIC && !MQTTObserverConfigCodec::validTopic(setting)) ||
        (validation == TOPIC_TEMPLATE && *setting &&
         !MQTTObserverConfigCodec::validTopicTemplate(setting)) ||
        (validation == OWNER && !MQTTObserverConfigCodec::validOwner(setting)) ||
        (validation == EMAIL && !MQTTObserverConfigCodec::validEmail(setting))) {
      // an unknown placeholder would publish the whole feed to a topic nobody
      // reads, so the template is checked before it is stored
      if (validation == TOPIC_TEMPLATE && *setting)
        snprintf(reply, CLI_REPLY_MAX,
                 "Err - topic placeholder must be {iata}, {device}, {token} or {type}");
      else
        snprintf(reply, CLI_REPLY_MAX, "Err - invalid or oversized MQTT setting");
      return true;
    }
    MQTTObserverBrokerConfig& broker = config.brokers[slot];
    char* field = MQTT_PROP_IS("server") ? broker.server :
                  MQTT_PROP_IS("username") ? broker.username :
                  MQTT_PROP_IS("password") ? broker.password :
                  MQTT_PROP_IS("audience") ? broker.audience :
                  MQTT_PROP_IS("topic") ? broker.topic :
                  MQTT_PROP_IS("token") ? broker.token :
                  MQTT_PROP_IS("owner") ? broker.owner : broker.email;
    *changed = strcmp(field, setting) != 0;
    if (*changed) setString(field, capacity, setting);
    if (MQTT_PROP_IS("password") || MQTT_PROP_IS("token"))
      snprintf(reply, CLI_REPLY_MAX, "OK - credential saved");
    else
      snprintf(reply, CLI_REPLY_MAX, "OK - MQTT%u setting saved", static_cast<unsigned>(slot + 1));
    return true;
  }
#undef MQTT_PROP_IS

  if (cliStartsWith(command, "set wifi.powersave ")) {
    const char* ps = command + strlen("set wifi.powersave ");
    uint8_t power_save;
    if (strcmp(ps, "min") == 0) power_save = 0;
    else if (strcmp(ps, "none") == 0) power_save = 1;
    else if (strcmp(ps, "max") == 0) power_save = 2;
    else {
      snprintf(reply, CLI_REPLY_MAX, "Err - expected none, min, or max");
      return true;
    }
    *changed = config.wifi_power_save != power_save;
    config.wifi_power_save = power_save;
    snprintf(reply, CLI_REPLY_MAX, "OK - WiFi power save saved");
    return true;
  }

  const char* wifi_value = cliCommandValue(command, "set wifi.ssid");
  bool wifi_secret = false;
  char* wifi_field = config.wifi_ssid;
  size_t wifi_capacity = sizeof(config.wifi_ssid);
  if (!wifi_value) {
    wifi_value = cliCommandValue(command, "set wifi.pwd");
    if (wifi_value) {
      wifi_field = config.wifi_password;
      wifi_capacity = sizeof(config.wifi_password);
      wifi_secret = true;
    }
  }
  if (wifi_value) {
    if (strlen(wifi_value) >= wifi_capacity) {
      snprintf(reply, CLI_REPLY_MAX, "Err - setting too long");
      return true;
    }
    *changed = strcmp(wifi_field, wifi_value) != 0;
    if (*changed) setString(wifi_field, wifi_capacity, wifi_value);
    snprintf(reply, CLI_REPLY_MAX, "%s", wifi_secret ? "OK - credential saved" : "OK - WiFi SSID saved");
    return true;
  }

  snprintf(reply, CLI_REPLY_MAX, "Err - unsupported MQTT/WiFi setting");
  return true;
}

// ------------------------------------------------------------------- get

bool handleGetCommand(const MQTTObserverConfig& config, const char* origin_source,
                      const MQTTObserverCliFacts& facts, const char* command, char* reply) {
  if (strcmp(command, "get mqtt.status") == 0) {
    mqttObserverCliStatus(config, facts, reply, CLI_REPLY_MAX);
    return true;
  }
  if (strcmp(command, "get wifi.powersave") == 0 || strcmp(command, "get wifi.ssid") == 0 ||
      strcmp(command, "get wifi.pwd") == 0) {
    const char* value;
    if (strcmp(command, "get wifi.powersave") == 0)
      value = config.wifi_power_save == 1 ? "none" :
              config.wifi_power_save == 2 ? "max" : "min";
    else if (strcmp(command, "get wifi.ssid") == 0)
      value = config.wifi_ssid[0] ? config.wifi_ssid : "not set";
    else value = config.wifi_password[0] ? config.wifi_password : "not set";
    snprintf(reply, CLI_REPLY_MAX, "%s", value);
    return true;
  }
  if (strcmp(command, "get wifi.status") == 0) {
    snprintf(reply, CLI_REPLY_MAX, "wifi %s ip:%u.%u.%u.%u rssi:%d",
             facts.wifi_up ? "connected" : "disconnected",
             static_cast<unsigned>(facts.wifi_ip[0]), static_cast<unsigned>(facts.wifi_ip[1]),
             static_cast<unsigned>(facts.wifi_ip[2]), static_cast<unsigned>(facts.wifi_ip[3]),
             facts.wifi_up ? facts.wifi_rssi : 0);
    return true;
  }

  enum CommonGet { COMMON_NONE, COMMON_IATA, COMMON_ORIGIN, COMMON_NTP, COMMON_RX,
                   COMMON_STATUS, COMMON_PACKETS, COMMON_RAW, COMMON_TX,
                   COMMON_INTERVAL } common = COMMON_NONE;
  if (strcmp(command, "get mqtt.iata") == 0) common = COMMON_IATA;
  else if (strcmp(command, "get mqtt.origin") == 0) common = COMMON_ORIGIN;
  else if (strcmp(command, "get mqtt.ntp") == 0) common = COMMON_NTP;
  else if (strcmp(command, "get mqtt.rx") == 0) common = COMMON_RX;
  else if (strcmp(command, "get mqtt.status.enabled") == 0) common = COMMON_STATUS;
  else if (strcmp(command, "get mqtt.packets") == 0) common = COMMON_PACKETS;
  else if (strcmp(command, "get mqtt.raw") == 0) common = COMMON_RAW;
  else if (strcmp(command, "get mqtt.tx") == 0) common = COMMON_TX;
  else if (strcmp(command, "get mqtt.interval") == 0) common = COMMON_INTERVAL;
  if (common != COMMON_NONE) {
    char common_value[160];
    switch (common) {
      case COMMON_IATA:
        snprintf(common_value, sizeof(common_value), "%s", config.iata[0] ? config.iata : "not configured");
        break;
      case COMMON_ORIGIN:
        snprintf(common_value, sizeof(common_value), "%s", config.origin[0] ? config.origin :
                 (origin_source && *origin_source ? origin_source : "not configured"));
        break;
      case COMMON_NTP:
        snprintf(common_value, sizeof(common_value), "%s", config.ntp_server[0] ? config.ntp_server : "pool.ntp.org");
        break;
      case COMMON_RX: snprintf(common_value, sizeof(common_value), "%s", config.rx_enabled ? "on" : "off"); break;
      case COMMON_STATUS: snprintf(common_value, sizeof(common_value), "%s", config.status_enabled ? "on" : "off"); break;
      case COMMON_PACKETS: snprintf(common_value, sizeof(common_value), "%s", config.packets_enabled ? "on" : "off"); break;
      case COMMON_RAW: snprintf(common_value, sizeof(common_value), "%s", config.raw_enabled ? "on" : "off"); break;
      case COMMON_TX: snprintf(common_value, sizeof(common_value), "%s",
                               config.tx_enabled == 2 ? "advert" : config.tx_enabled == 1 ? "on" : "off"); break;
      case COMMON_INTERVAL: snprintf(common_value, sizeof(common_value), "%u", static_cast<unsigned>(config.status_interval_minutes)); break;
      default: common_value[0] = 0; break;
    }
    snprintf(reply, CLI_REPLY_MAX, "%s", common_value);
    return true;
  }

  size_t slot = 0;
  const char* property = nullptr;
  if (!commandSlot(command, "get ", slot, property)) return false;
  size_t property_len = strcspn(property, " ");
#define MQTT_GET_IS(name) (property_len == sizeof(name) - 1 && strncmp(property, name, sizeof(name) - 1) == 0)
  char value[160] = {};
  const MQTTObserverBrokerConfig& broker = config.brokers[slot];
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
  else if (MQTT_GET_IS("password")) result = broker.password[0] ? broker.password : "not set";
  else if (MQTT_GET_IS("username")) result = broker.username[0] ? broker.username : "not set";
  else if (MQTT_GET_IS("token")) result = broker.token[0] ? broker.token : "not set";
  if (result && result != value) {
    snprintf(value, sizeof(value), "%s", result);
    result = value;
  }

  if (result) {
    snprintf(reply, CLI_REPLY_MAX, "%s", result);
    return true;
  }
  if (MQTT_GET_IS("diag")) {
    const MQTTObserverSlotDiag& diag = facts.slot_diag[slot];
    char error[24];
    switch (diag.error_type) {
      case DIAG_ERR_TRANSPORT: snprintf(error, sizeof(error), "transport"); break;
      case DIAG_ERR_MQTT: snprintf(error, sizeof(error), "mqtt"); break;
      case DIAG_ERR_TIMEOUT: snprintf(error, sizeof(error), "timeout"); break;
      case DIAG_ERR_CONFIG: snprintf(error, sizeof(error), "config"); break;
      case DIAG_ERR_LIVENESS: snprintf(error, sizeof(error), "liveness"); break;
      case DIAG_ERR_CLOSED: snprintf(error, sizeof(error), "closed"); break;
      case DIAG_ERR_NONE: snprintf(error, sizeof(error), "none"); break;
      default:
        snprintf(error, sizeof(error), "other/%u",
                 static_cast<unsigned>(diag.error_type));
        break;
    }
    snprintf(reply, CLI_REPLY_MAX,
             "mqtt%u diag %s err:%s tls:0x%X stk:0x%X cert:0x%X sock:%d "
             "att:%u fail:%u age:%lus up:%lus retry:%lus",
             static_cast<unsigned>(slot + 1), mqttObserverStateName(facts.slot_state[slot]),
             error, static_cast<unsigned>(diag.tls_err),
             static_cast<unsigned>(diag.tls_stack),
             static_cast<unsigned>(diag.tls_flags), static_cast<int>(diag.sock_errno),
             static_cast<unsigned>(diag.attempts),
             static_cast<unsigned>(diag.failures),
             static_cast<unsigned long>(diag.error_age_s),
             static_cast<unsigned long>(diag.connected_age_s),
             static_cast<unsigned long>(diag.retry_in_s));
    return true;
  }
  if (MQTT_GET_IS("status")) {
    snprintf(reply, CLI_REPLY_MAX, "mqtt%u %s wifi:%s q:%u",
             static_cast<unsigned>(slot + 1), mqttObserverStateName(facts.slot_state[slot]),
             facts.wifi_up ? "up" : "down", static_cast<unsigned>(facts.queue_size));
    return true;
  }
#undef MQTT_GET_IS
  return false;
}

}  // namespace

bool mqttObserverCliCommand(MQTTObserverConfig& config, const char* origin_source,
                            const MQTTObserverCliFacts& facts, const char* command,
                            char* reply, bool* changed) {
  if (!command || !reply) return false;
  if (handleGetCommand(config, origin_source, facts, command, reply)) return true;
  if (cliStartsWith(command, "set mqtt") || cliStartsWith(command, "set wifi")) {
    bool local_changed = false;
    bool handled = handleSetCommand(config, command, reply, &local_changed);
    if (changed && local_changed) *changed = true;
    return handled;
  }
  return false;
}
