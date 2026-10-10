#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace MQTTObserverQueuePolicy {

static const uint32_t kMaxEventAgeMs = 10000;

inline bool eventExpired(uint32_t now, uint32_t received) {
  return static_cast<uint32_t>(now - received) > kMaxEventAgeMs;
}

}  // namespace MQTTObserverQueuePolicy

template <typename Event, size_t Capacity>
class MQTTObserverLiveQueue {
  static_assert(Capacity > 0, "queue capacity must be nonzero");
  Event _events[Capacity];
  size_t _head = 0;
  size_t _count = 0;

public:
  enum OfferResult { QUEUED, DROPPED_OLDEST, DROPPED_DISCONNECTED };

  OfferResult offer(bool connected, const Event& event) {
    if (!connected) return DROPPED_DISCONNECTED;
    OfferResult result = QUEUED;
    if (_count == Capacity) {
      _head = (_head + 1) % Capacity;
      _count--;
      result = DROPPED_OLDEST;
    }
    _events[(_head + _count) % Capacity] = event;
    _count++;
    return result;
  }

  bool pop(Event& event) {
    if (_count == 0) return false;
    event = _events[_head];
    _head = (_head + 1) % Capacity;
    _count--;
    return true;
  }

  void clear() {
    _head = 0;
    _count = 0;
  }

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
