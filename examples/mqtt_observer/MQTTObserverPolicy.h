#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// The fork's shared CLI helpers: dependency-free (standard headers only), so
// including them keeps this header host-testable without an Arduino.
#include "../simple_repeater/CliUtil.h"

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

}  // namespace MQTTObserverCommandPolicy

namespace MQTTObserverConnectionPolicy {

inline bool jwtNeedsRenewal(int64_t epoch, uint32_t expiry) {
  return expiry != 0 && epoch + 300 >= expiry;
}

// A TLS client needs a contiguous mbedTLS allocation; starting one without
// headroom aborts a non-PSRAM device, so connect() waits for the heap to
// recover instead of crashing. Without PSRAM those buffers come from internal
// RAM. With PSRAM the observer redirects mbedTLS there, so the secure
// thresholds apply to the PSRAM pool and internal RAM only needs the plaintext
// floor for the MQTT/lwIP buffers. The secure thresholds follow the reference
// observers (roughly 60 KB free and one ~17 KB TLS record). Keeping the
// decision pure lets the host suite cover it.
static const size_t kSecureFreeBytes = 64u * 1024u;
static const size_t kSecureLargestBytes = 18u * 1024u;
static const size_t kPlainFreeInternalBytes = 24u * 1024u;
static const size_t kPlainLargestInternalBytes = 8u * 1024u;

inline bool hasConnectHeadroom(bool secure, bool tls_in_psram,
                               size_t free_internal, size_t largest_internal,
                               size_t free_psram, size_t largest_psram) {
  if (!secure)
    return free_internal >= kPlainFreeInternalBytes &&
           largest_internal >= kPlainLargestInternalBytes;
  if (!tls_in_psram)
    return free_internal >= kSecureFreeBytes && largest_internal >= kSecureLargestBytes;
  return free_psram >= kSecureFreeBytes && largest_psram >= kSecureLargestBytes &&
         free_internal >= kPlainFreeInternalBytes &&
         largest_internal >= kPlainLargestInternalBytes;
}

// Liveness probing. A QoS 1 publish proves the broker session is alive: the
// status message is the probe while status is enabled, and with status off at
// most one packets/raw publish per interval is promoted to QoS 1. An
// unacknowledged probe means the session is dead even though the local flag
// still reports connected, because keepalive timers are refreshed by outbound
// writes on half-open sockets. The cadence follows mqtt.interval so there is
// one setting for "how often we prove the connection".
static const uint32_t kLivenessProbeTimeoutMs = 60000UL;

inline bool livenessProbeDue(bool status_enabled, int outstanding_msg_id,
                             uint32_t now, uint32_t last_probe_ms,
                             uint32_t interval_ms) {
  if (status_enabled || outstanding_msg_id != 0 || interval_ms == 0) return false;
  // A slot that has never probed (fresh connect) probes on its first publish;
  // afterwards it waits a full interval.
  return last_probe_ms == 0 || now - last_probe_ms >= interval_ms;
}

inline bool livenessProbeTimedOut(int outstanding_msg_id, uint32_t now,
                                  uint32_t probe_sent_ms) {
  if (outstanding_msg_id == 0 || probe_sent_ms == 0) return false;
  return now - probe_sent_ms >= kLivenessProbeTimeoutMs;
}

// A connect attempt that has not come up within the timeout failed: the broker
// refused it (bad credentials, wrong protocol) or was unreachable. The client
// library only reports transport errors to the application, so the outcome of
// a connect attempt is decided here, from the attempt's own clock.
static const uint32_t kConnectAttemptTimeoutMs = 30000UL;

inline bool connectAttemptTimedOut(bool client_started, uint32_t now,
                                   uint32_t attempt_started) {
  if (!client_started) return false;
  return (uint32_t)(now - attempt_started) >= kConnectAttemptTimeoutMs;
}

}  // namespace MQTTObserverConnectionPolicy

// The outstanding liveness probe of one broker.
//
// The probe is a QoS 1 publish: its packet id is recorded when it goes out and
// cleared when its PUBACK arrives. The two do not happen in that order: the
// broker can answer while publish() is still returning, so the PUBACK can be
// delivered before the id is recorded. Recording it anyway would leave a probe
// outstanding that nothing will ever clear, and the liveness timeout would
// force a reconnect every 60 s — a permanent churn, not a one-off.
//
// So the last acknowledged id is remembered: a probe whose id was already
// acknowledged is not outstanding, and the liveness timeout stays quiet. The
// tracker is reset with the connection (the packet ids restart there, so a
// stale id from the previous session must not be able to match).
class MQTTObserverProbeTracker {
public:
  // A QoS 1 publish went out with this packet id.
  void noteProbe(int msg_id, uint32_t now) {
    if (msg_id <= 0) return;
    if (_last_puback_id.load(std::memory_order_relaxed) == msg_id) return;   // already acked
    int expected = 0;
    if (!_outstanding.compare_exchange_strong(expected, msg_id, std::memory_order_relaxed)) return;
    _sent_ms.store(now, std::memory_order_relaxed);
    _last_probe_ms = now;
  }

  // The broker acknowledged this packet id.
  void notePuback(int msg_id) {
    if (msg_id <= 0) return;
    _last_puback_id.store(msg_id, std::memory_order_relaxed);
    int expected = msg_id;
    if (_outstanding.compare_exchange_strong(expected, 0, std::memory_order_relaxed))
      _sent_ms.store(0, std::memory_order_relaxed);
  }

  // A new connection: the packet ids start over, so no id from the previous
  // session may be remembered.
  void reset() {
    _outstanding.store(0, std::memory_order_relaxed);
    _sent_ms.store(0, std::memory_order_relaxed);
    _last_puback_id.store(-1, std::memory_order_relaxed);
    _last_probe_ms = 0;
  }

  int outstanding() const { return _outstanding.load(std::memory_order_relaxed); }
  uint32_t sentMs() const { return _sent_ms.load(std::memory_order_relaxed); }
  uint32_t lastProbeMs() const { return _last_probe_ms; }

private:
  std::atomic<int> _outstanding{0};
  std::atomic<uint32_t> _sent_ms{0};
  std::atomic<int> _last_puback_id{-1};   // -1 = no acknowledgement seen yet
  uint32_t _last_probe_ms = 0;
};

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
