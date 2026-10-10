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
};

bool formatTimestamp(time_t timestamp, long usec, char* out, size_t capacity);
bool buildClientVersion(const char* firmware, char* out, size_t capacity);
bool buildPacket(const PacketView& packet, char* out, size_t capacity, size_t& written);
bool buildRaw(const char* origin, const char* origin_id, const char* timestamp,
              const uint8_t* bytes, size_t len, char* out, size_t capacity, size_t& written);
bool buildStatus(const char* origin, const char* origin_id, const char* model,
                 const char* firmware, const char* radio, const char* timestamp,
                 char* out, size_t capacity, size_t& written);

}  // namespace MQTTObserverFormat
