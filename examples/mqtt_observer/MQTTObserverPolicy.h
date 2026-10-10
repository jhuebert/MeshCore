#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace MQTTObserverCommandPolicy {

inline bool parseSlot(const char* command, const char* operation, size_t max_slots,
                      size_t& slot, const char*& property) {
  size_t operation_len = strlen(operation);
  if (!command || strncmp(command, operation, operation_len) != 0) return false;
  const char* name = command + operation_len;
  if (strncmp(name, "mqtt", 4) != 0) return false;
  const char* p = name + 4;
  if (*p < '0' || *p > '9') return false;
  size_t number = 0;
  while (*p >= '0' && *p <= '9') {
    number = number * 10 + static_cast<size_t>(*p++ - '0');
    if (number > max_slots) return false;
  }
  if (*p++ != '.' || number == 0) return false;  // number <= max_slots held by the loop
  slot = number - 1;
  property = p;
  return true;
}

// True when a `set` line carries a credential value that must not reach the
// fleet-script log: `set wifi.pwd <...>` and `set mqttN.password|username|token
// <...>`. `get` lines are never masked — their replies hold only
// configured/not-set state, and the log is more useful with them visible.
inline bool isCredentialSetting(const char* line) {
  if (!line) return false;
  while (*line == ' ') line++;
  if (strncmp(line, "set ", 4) != 0) return false;
  line += 4;
  if (strncmp(line, "wifi.pwd", 8) == 0 && (line[8] == 0 || line[8] == ' ')) return true;
  if (strncmp(line, "mqtt", 4) != 0) return false;
  line += 4;
  while (*line >= '0' && *line <= '9') line++;
  if (*line++ != '.') return false;
  static const char* const fields[] = {"password", "username", "token"};
  for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
    size_t len = strlen(fields[i]);
    if (strncmp(line, fields[i], len) == 0 && (line[len] == 0 || line[len] == ' ')) return true;
  }
  return false;
}

}  // namespace MQTTObserverCommandPolicy

namespace MQTTObserverQueuePolicy {

static const uint32_t kDisconnectedStaleMs = 60000UL;
static const uint8_t kMaxQos0RetryAttempts = 3;
static const uint32_t kRetryDelayBaseMs = 300UL;
static const uint32_t kRetryDelayJitterMs = 200UL;

inline uint32_t elapsedMs(uint32_t now, uint32_t then) {
  return now - then;
}

inline bool shouldFlushDisconnected(uint32_t now, uint32_t disconnected_since) {
  return disconnected_since != 0 &&
         elapsedMs(now, disconnected_since) >= kDisconnectedStaleMs;
}

inline bool retryReady(uint32_t now, uint32_t next_retry_ms, uint8_t retry_attempts) {
  return retry_attempts == 0 || elapsedMs(now, next_retry_ms) < 0x80000000UL;
}

enum class RetryAction : uint8_t { Complete, Schedule, Drop };

struct RetryDecision {
  RetryAction action;
  uint8_t retry_attempts;
  uint32_t delay_ms;
  uint32_t next_retry_ms;
};

inline RetryDecision retryDecision(bool any_published, uint8_t retry_attempts,
                                   uint32_t now) {
  if (any_published) return {RetryAction::Complete, retry_attempts, 0, 0};
  if (retry_attempts >= kMaxQos0RetryAttempts)
    return {RetryAction::Drop, retry_attempts, 0, 0};
  const uint32_t delay = kRetryDelayBaseMs + (now % kRetryDelayJitterMs);
  return {RetryAction::Schedule, static_cast<uint8_t>(retry_attempts + 1),
          delay, now + delay};
}

}  // namespace MQTTObserverQueuePolicy

template <typename Event, size_t Capacity>
class MQTTObserverEventQueue {
  static_assert(Capacity > 0, "queue capacity must be nonzero");

public:
  struct Entry {
    Event event;
    uint32_t next_retry_ms;
    uint8_t retry_attempts;
  };

  enum OfferResult { QUEUED, DROPPED_OLDEST, DROPPED_NEWEST };

private:
  Entry _entries[Capacity];
  size_t _count = 0;
  bool _sending = false;

  void removeAt(size_t index) {
    for (size_t i = index; i + 1 < _count; i++) _entries[i] = _entries[i + 1];
    _count--;
  }

public:
  OfferResult offer(const Event& event) {
    OfferResult result = QUEUED;
    if (_count == Capacity) {
      if (_sending) {
        if (Capacity == 1) return DROPPED_NEWEST;
        removeAt(1);  // Keep the in-flight head stable until its send attempt finishes.
      } else {
        removeAt(0);
      }
      result = DROPPED_OLDEST;
    }
    _entries[_count].event = event;
    _entries[_count].next_retry_ms = 0;
    _entries[_count].retry_attempts = 0;
    _count++;
    return result;
  }

  bool beginAttempt(uint32_t now, Entry& entry) {
    if (_count == 0 || _sending ||
        !MQTTObserverQueuePolicy::retryReady(now, _entries[0].next_retry_ms,
                                             _entries[0].retry_attempts)) return false;
    _sending = true;
    entry = _entries[0];
    return true;
  }

  void completeAttempt() {
    if (!_sending || _count == 0) return;
    removeAt(0);
    _sending = false;
  }

  void retryAttempt(uint8_t attempts, uint32_t next_retry_ms) {
    if (!_sending || _count == 0) return;
    _entries[0].retry_attempts = attempts;
    _entries[0].next_retry_ms = next_retry_ms;
    _sending = false;
  }

  void clear() { _count = 0; _sending = false; }
  size_t size() const { return _count; }
};

class MQTTObserverRawStager {
public:
  static const size_t kCapacity = 256;
  struct Snapshot {
    uint8_t bytes[kCapacity];
    uint16_t length;
    float snr;
    float rssi;
  };

private:
  Snapshot _staged{};
  bool _valid = false;

public:
  void stage(const uint8_t* bytes, size_t len, float snr, float rssi) {
    _valid = bytes != nullptr && len > 0 && len <= kCapacity;
    if (!_valid) {
      _staged.length = 0;
      return;
    }
    memcpy(_staged.bytes, bytes, len);
    _staged.length = static_cast<uint16_t>(len);
    _staged.snr = snr;
    _staged.rssi = rssi;
  }

  bool consume(Snapshot& snapshot) {
    if (!_valid) return false;
    snapshot = _staged;
    _valid = false;
    return true;
  }

  bool consume(uint8_t* bytes, size_t capacity, uint16_t& length, float& snr, float& rssi) {
    if (!_valid) return false;
    if (!bytes || _staged.length > capacity) {
      _valid = false;
      length = 0;
      return false;
    }
    memcpy(bytes, _staged.bytes, _staged.length);
    length = _staged.length;
    snr = _staged.snr;
    rssi = _staged.rssi;
    _valid = false;
    return true;
  }
};
