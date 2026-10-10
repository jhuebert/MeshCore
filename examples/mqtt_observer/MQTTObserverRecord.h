#pragma once

#include "MQTTObserverConfig.h"
#include "../simple_repeater/PersistUtil.h"

namespace MQTTObserverRecord {

static const size_t kHeaderSize = 7;
static const size_t kRecordSize = kHeaderSize + MQTTObserverConfigCodec::kPayloadSize + 2;
static const size_t kLegacyV1RecordSize = kHeaderSize + MQTTObserverConfigCodec::kLegacyPayloadSize + 2;
static const uint8_t kMagic[4] = {'M', 'Q', 'O', '1'};

inline size_t encode(const MQTTObserverConfig& config, uint8_t* out, size_t capacity) {
  if (!out || capacity < kRecordSize) return 0;
  memcpy(out, kMagic, sizeof(kMagic));
  out[4] = MQTTObserverConfigCodec::kVersion;
  out[5] = MQTTObserverConfigCodec::kPayloadSize & 0xff;
  out[6] = MQTTObserverConfigCodec::kPayloadSize >> 8;
  if (MQTTObserverConfigCodec::encode(config, out + kHeaderSize,
                                      MQTTObserverConfigCodec::kPayloadSize) !=
      MQTTObserverConfigCodec::kPayloadSize) return 0;
  uint16_t crc = crc16_ccitt(out, kRecordSize - 2);
  out[kRecordSize - 2] = crc & 0xff;
  out[kRecordSize - 1] = crc >> 8;
  return kRecordSize;
}

inline bool decode(const uint8_t* data, size_t len, MQTTObserverConfig& config) {
  if (!data || len < kHeaderSize + 2 || memcmp(data, kMagic, sizeof(kMagic)) != 0) return false;
  size_t payload_len = static_cast<size_t>(data[5]) |
                       (static_cast<size_t>(data[6]) << 8);
  if (len != kHeaderSize + payload_len + 2) return false;
  uint16_t stored_crc = static_cast<uint16_t>(data[len - 2]) |
                        (static_cast<uint16_t>(data[len - 1]) << 8);
  if (stored_crc != crc16_ccitt(data, len - 2)) return false;
  if (data[4] == 1 && payload_len == MQTTObserverConfigCodec::kLegacyPayloadSize)
    return MQTTObserverConfigCodec::decodeLegacyV1(data + kHeaderSize, payload_len, config);
  if (data[4] == MQTTObserverConfigCodec::kVersion &&
      payload_len == MQTTObserverConfigCodec::kPayloadSize)
    return MQTTObserverConfigCodec::decode(data + kHeaderSize, payload_len, config);
  return false;
}

}  // namespace MQTTObserverRecord
