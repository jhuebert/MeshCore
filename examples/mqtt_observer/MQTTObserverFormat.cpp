#include "MQTTObserverFormat.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace MQTTObserverFormat {
namespace {

class JsonWriter {
  char* _out;
  size_t _capacity;
  size_t _length;
  bool _valid;

  void appendChar(char c) {
    if (!_valid || _length + 1 >= _capacity) {
      _valid = false;
      return;
    }
    _out[_length++] = c;
    _out[_length] = 0;
  }

public:
  JsonWriter(char* out, size_t capacity)
      : _out(out), _capacity(capacity), _length(0), _valid(out && capacity) {
    if (_valid) out[0] = 0;
  }

  void append(const char* text) {
    if (!_valid || !text) return;
    size_t len = strlen(text);
    if (len >= _capacity - _length) {
      _valid = false;
      return;
    }
    memcpy(_out + _length, text, len);
    _length += len;
    _out[_length] = 0;
  }

  void string(const char* text) {
    static const char hex[] = "0123456789abcdef";
    append("\"");
    if (!text) text = "";
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text); *p && _valid; p++) {
      if (*p == '"' || *p == '\\') {
        append("\\");
        appendChar(static_cast<char>(*p));
      } else if (*p < 0x20) {
        char escaped[7] = {'\\', 'u', '0', '0', hex[*p >> 4], hex[*p & 0x0f], 0};
        append(escaped);
      } else {
        appendChar(static_cast<char>(*p));
      }
    }
    append("\"");
  }

  void hexString(const uint8_t* bytes, size_t len, bool lowercase = false) {
    static const char upper[] = "0123456789ABCDEF";
    static const char lower[] = "0123456789abcdef";
    const char* digits = lowercase ? lower : upper;
    append("\"");
    if (!bytes && len) {
      _valid = false;
      return;
    }
    for (size_t i = 0; i < len && _valid; i++) {
      appendChar(digits[bytes[i] >> 4]);
      appendChar(digits[bytes[i] & 0x0f]);
    }
    append("\"");
  }

  bool finish(size_t& written) {
    written = _length;
    if (!_valid) {
      if (_out && _capacity) _out[0] = 0;
      return false;
    }
    return true;
  }
};

void appendKey(JsonWriter& writer, const char* key, const char* value, bool comma = true) {
  if (comma) writer.append(",");
  writer.string(key);
  writer.append(":");
  writer.string(value);
}

}  // namespace

bool formatTimestamp(time_t timestamp, long usec, char* out, size_t capacity) {
  if (!out || capacity < 1) return false;
  if (usec < 0) usec = 0;
  if (usec > 999999) usec = 999999;
  struct tm utc;
  if (!gmtime_r(&timestamp, &utc)) {
    out[0] = 0;
    return false;
  }
  size_t len = strftime(out, capacity, "%Y-%m-%dT%H:%M:%S", &utc);
  if (!len || len >= capacity) {
    out[0] = 0;
    return false;
  }
  int suffix = snprintf(out + len, capacity - len, ".%06ld+00:00", usec);
  if (suffix < 0 || static_cast<size_t>(suffix) >= capacity - len) {
    out[0] = 0;
    return false;
  }
  return true;
}

bool buildPacket(const PacketView& packet, char* out, size_t capacity, size_t& written) {
  if (!packet.origin || !packet.origin_id || !packet.route || !packet.wire ||
      !packet.hash || packet.hash_len == 0 || packet.wire_len == 0) return false;
  if (packet.path_count && (!packet.path || packet.path_hash_size == 0 ||
      packet.path_hash_size > 3 ||
      static_cast<size_t>(packet.path_count) * packet.path_hash_size > 64)) return false;

  char timestamp[40];
  if (!formatTimestamp(packet.timestamp, packet.timestamp_usec, timestamp, sizeof(timestamp))) return false;
  char time[16], date[16];
  struct tm utc;
  if (!gmtime_r(&packet.timestamp, &utc) ||
      !strftime(time, sizeof(time), "%H:%M:%S", &utc) ||
      !strftime(date, sizeof(date), "%d/%m/%Y", &utc)) return false;

  char len[16], type[16], payload_len[16], snr[16], rssi[16], score[16];
  snprintf(len, sizeof(len), "%u", static_cast<unsigned>(packet.wire_len));
  snprintf(type, sizeof(type), "%u", static_cast<unsigned>(packet.packet_type));
  snprintf(payload_len, sizeof(payload_len), "%u", static_cast<unsigned>(packet.payload_len));
  snprintf(snr, sizeof(snr), "%.1f", packet.snr);
  snprintf(rssi, sizeof(rssi), "%d", static_cast<int>(packet.rssi));

  JsonWriter writer(out, capacity);
  writer.append("{");
  appendKey(writer, "timestamp", timestamp, false);
  writer.append(",\"hash\":");
  writer.hexString(packet.hash, packet.hash_len);
  appendKey(writer, "origin", packet.origin);
  appendKey(writer, "type", "PACKET");
  appendKey(writer, "direction", "rx");
  appendKey(writer, "time", time);
  appendKey(writer, "date", date);
  appendKey(writer, "len", len);
  appendKey(writer, "packet_type", type);
  appendKey(writer, "route", packet.route);
  appendKey(writer, "payload_len", payload_len);
  writer.append(",\"raw\":");
  writer.hexString(packet.wire, packet.wire_len);
  appendKey(writer, "origin_id", packet.origin_id);
  appendKey(writer, "SNR", snr);
  appendKey(writer, "RSSI", rssi);
  if (!isnan(packet.score)) {
    snprintf(score, sizeof(score), "%d", static_cast<int>(packet.score * 1000));
    appendKey(writer, "score", score);
  }
  if (packet.path_count) {
    writer.append(",\"path\":[");
    for (uint8_t i = 0; i < packet.path_count; i++) {
      if (i) writer.append(",");
      writer.hexString(packet.path + static_cast<size_t>(i) * packet.path_hash_size,
                       packet.path_hash_size, true);
    }
    writer.append("]");
  }
  writer.append("}");
  return writer.finish(written);
}

bool buildRaw(const char* origin, const char* origin_id, const char* timestamp,
              const uint8_t* bytes, size_t len, char* out, size_t capacity, size_t& written) {
  if (!origin || !origin_id || !timestamp || (!bytes && len)) return false;
  JsonWriter writer(out, capacity);
  writer.append("{");
  appendKey(writer, "origin", origin, false);
  appendKey(writer, "origin_id", origin_id);
  appendKey(writer, "timestamp", timestamp);
  appendKey(writer, "type", "RAW");
  writer.append(",\"data\":");
  writer.hexString(bytes, len);
  writer.append("}");
  return writer.finish(written);
}

bool buildStatus(const char* origin, const char* origin_id, const char* model,
                 const char* firmware, const char* radio, const char* timestamp,
                 char* out, size_t capacity, size_t& written) {
  if (!origin || !origin_id || !model || !firmware || !radio || !timestamp) return false;
  JsonWriter writer(out, capacity);
  writer.append("{");
  appendKey(writer, "status", "online", false);
  appendKey(writer, "timestamp", timestamp);
  appendKey(writer, "origin", origin);
  appendKey(writer, "origin_id", origin_id);
  appendKey(writer, "model", model);
  appendKey(writer, "firmware_version", firmware);
  appendKey(writer, "radio", radio);
  appendKey(writer, "client_version", "MeshCore");
  writer.append("}");
  return writer.finish(written);
}

}  // namespace MQTTObserverFormat
