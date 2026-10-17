#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace MQTTObserverCommandPolicy {

inline bool parseUnsigned(const char* value, unsigned min, unsigned max, unsigned& parsed) {
  if (!value || !*value) return false;
  unsigned result = 0;
  for (const char* p = value; *p; p++) {
    if (*p < '0' || *p > '9') return false;
    unsigned digit = static_cast<unsigned>(*p - '0');
    if (digit > max || result > (max - digit) / 10) return false;
    result = result * 10 + digit;
  }
  if (result < min) return false;
  parsed = result;
  return true;
}

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

}  // namespace MQTTObserverCommandPolicy

namespace MQTTObserverConnectionPolicy {

inline bool jwtNeedsRenewal(int64_t epoch, uint32_t expiry) {
  return expiry != 0 && epoch + 300 >= expiry;
}

// A TLS client needs a contiguous mbedTLS allocation on the internal heap;
// starting one without headroom aborts a non-PSRAM device, so connect() waits
// for the heap to recover instead of crashing. The secure thresholds follow
// the reference observers (roughly 60 KB free and one ~17 KB TLS record);
// plaintext brokers need far less. Keeping the decision pure lets the host
// suite cover it.
static const size_t kSecureFreeInternalBytes = 64u * 1024u;
static const size_t kSecureLargestInternalBytes = 18u * 1024u;
static const size_t kPlainFreeInternalBytes = 24u * 1024u;
static const size_t kPlainLargestInternalBytes = 8u * 1024u;

inline bool hasConnectHeadroom(bool secure, size_t free_internal, size_t largest_internal) {
  const size_t free_needed = secure ? kSecureFreeInternalBytes : kPlainFreeInternalBytes;
  const size_t block_needed = secure ? kSecureLargestInternalBytes : kPlainLargestInternalBytes;
  return free_internal >= free_needed && largest_internal >= block_needed;
}

}  // namespace MQTTObserverConnectionPolicy

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

inline bool trackDisconnected(uint32_t now, bool empty, uint32_t& since) {
  if (empty) {
    since = 0;
    return false;
  }
  if (shouldFlushDisconnected(now, since)) {
    since = 0;
    return true;
  }
  if (!since) since = now ? now : 1;
  return false;
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
