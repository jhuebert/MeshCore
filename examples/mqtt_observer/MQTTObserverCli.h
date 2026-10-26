#pragma once

// MQTTObserverCli.h — the observer's CLI command handling, separated from the
// radio, the Wi-Fi stack and the MQTT client so it can be host-tested.
//
// The commands are the observer's user-facing surface (see MQTT_OBSERVER.md):
// every reply string here is API. The unit owns the config and the facts the
// device reports; the observer fills the facts in and applies the changes.

#include <stdint.h>
#include <stddef.h>

#include "MQTTObserverConfig.h"

// The per-slot states the CLI reports. The observer's state machine uses the
// same values, so the names and the numbers live in one place.
enum MQTTObserverSlotState : uint8_t {
  STATE_OFF = 0,
  STATE_CONFIG,
  STATE_WIFI,
  STATE_TIME,
  STATE_CONNECTING,
  STATE_CONNECTED,
  STATE_RETRY,
  STATE_ERROR
};

// The name a slot state is reported under ("off", "config", ...).
const char* mqttObserverStateName(MQTTObserverSlotState state);

// The last failure a slot recorded, reported by `get mqttN.diag`. Value 1
// mirrors esp_mqtt_error_type_t (transport); DIAG_ERR_MQTT covers an MQTT
// error event with no more specific observer category; the high values are
// the observer's own outcomes.
enum MQTTObserverDiagError : uint8_t {
  DIAG_ERR_NONE = 0,
  DIAG_ERR_TRANSPORT = 1,
  DIAG_ERR_MQTT = 2,          // an MQTT error event with no more specific type
  DIAG_ERR_TIMEOUT = 0xF0,    // the connect attempt never came up in 30 s
  DIAG_ERR_CONFIG = 0xF1,     // configureClient()/createJwt() failed
  DIAG_ERR_LIVENESS = 0xF2,   // the broker stopped acknowledging the probe
  DIAG_ERR_CLOSED = 0xF3      // broker closed the session, no transport error
};

// One slot's last-error report. The observer fills this from the MQTT client
// callbacks and its own attempt bookkeeping; the CLI only formats it. The
// last error is kept after a reconnect so a recovered slot still explains
// what happened.
struct MQTTObserverSlotDiag {
  uint8_t error_type = DIAG_ERR_NONE;
  int32_t tls_err = 0;           // esp_tls_last_esp_err
  int32_t tls_stack = 0;         // esp_tls_stack_err
  int32_t tls_flags = 0;         // esp_tls_cert_verify_flags
  int32_t sock_errno = 0;        // esp_transport_sock_errno
  uint32_t error_age_s = 0;      // seconds since the last error; 0 = none
  uint32_t connected_age_s = 0;  // seconds since this connection began; 0 = not connected
  uint32_t retry_in_s = 0;       // seconds until the next attempt; 0 = due/attempting/connected
  uint16_t attempts = 0;         // connect() calls since boot
  uint8_t failures = 0;          // consecutive failed attempts
};

// What the device reports to the commands: the Wi-Fi link, the clock, the
// heap, the queue and the per-slot state. The observer fills this in; the
// command handling itself is pure.
struct MQTTObserverCliFacts {
  bool wifi_up = false;
  int32_t wifi_rssi = 0;
  uint8_t wifi_ip[4] = {0, 0, 0, 0};
  bool clock_valid = false;
  uint32_t free_heap = 0;
  uint32_t max_heap = 0;
  uint32_t queue_size = 0;
  uint32_t queue_capacity = 0;
  uint32_t received = 0;          // frames heard since boot
  uint32_t published = 0;         // publish successes since boot
  uint32_t disconnected_drops = 0;
  uint32_t queue_drops = 0;
  uint32_t publish_drops = 0;
  MQTTObserverSlotState slot_state[MQTT_OBSERVER_MAX_BROKERS] = {STATE_OFF};
  bool slot_connected[MQTT_OBSERVER_MAX_BROKERS] = {false};
  MQTTObserverSlotDiag slot_diag[MQTT_OBSERVER_MAX_BROKERS];
};

// Handle one CLI command.
//
// Returns true when the command was one of the observer's (the reply is then
// filled in); false when it was not (the caller answers "unknown command").
// A "set" command always answers, even when the setting is not one of the
// observer's. `changed` reports whether the config was modified — the observer
// marks its config dirty on a change.
bool mqttObserverCliCommand(MQTTObserverConfig& config, const char* origin_source,
                            const MQTTObserverCliFacts& facts, const char* command,
                            char* reply, bool* changed);

// The status dashboard ("get mqtt.status").
void mqttObserverCliStatus(const MQTTObserverConfig& config,
                            const MQTTObserverCliFacts& facts, char* reply, size_t capacity);
