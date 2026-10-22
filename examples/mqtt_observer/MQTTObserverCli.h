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
