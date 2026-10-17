#pragma once

#include <stddef.h>
#include <stdint.h>
#include <time.h>

namespace MQTTObserverFormat {

struct PacketView {
  const char* origin;
  const char* origin_id;
  time_t timestamp;
  long timestamp_usec;
  const uint8_t* wire;
  size_t wire_len;
  uint8_t packet_type;
  const char* route;
  uint16_t payload_len;
  float snr;
  float rssi;
  float score;
  const uint8_t* hash;
  size_t hash_len;
  const uint8_t* path;
  uint8_t path_count;
  uint8_t path_hash_size;
  bool is_tx;
};

bool formatTimestamp(time_t timestamp, long usec, char* out, size_t capacity);
bool buildClientVersion(const char* firmware, char* out, size_t capacity);

// Appends the JSON-escaped body of text (no surrounding quotes) at out[length],
// keeping out NUL-terminated. Returns false when the escaped text does not fit,
// in which case out holds a truncated prefix. Shared by the JSON builders and
// the JWT claims so the escaping rules live in one place.
bool appendEscaped(const char* text, char* out, size_t capacity, size_t& length);
bool buildPacket(const PacketView& packet, char* out, size_t capacity, size_t& written);
bool buildRaw(const char* origin, const char* origin_id, const char* timestamp,
              const uint8_t* bytes, size_t len, char* out, size_t capacity, size_t& written);

// Live device stats for the periodic status message. Reported under "stats"
// (battery/uptime/noise floor) plus the top-level "repeat" relay flag, matching
// the schema other MeshCore observers publish.
struct StatusStats {
  uint16_t battery_mv;
  uint32_t uptime_secs;
  int16_t noise_floor;
  bool can_relay;
};

bool buildStatus(const char* origin, const char* origin_id, const char* model,
                 const char* firmware, const char* radio, const char* timestamp,
                 const StatusStats& stats, char* out, size_t capacity, size_t& written);

}  // namespace MQTTObserverFormat
