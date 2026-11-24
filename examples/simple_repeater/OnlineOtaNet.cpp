// OnlineOtaNet.cpp — ESP32 observer adapter for online OTA.
// Compiled only by repeater observer builds. It borrows the observer's WiFi
// station, clock and heap gate, and streams one bounded chunk per loop() step.
// The HTTPS GET is written directly on WiFiClientSecure: no extra library, no
// large response buffer, no new include path in observer_targets.ini.

#include "OnlineOtaNet.h"

#if defined(ESP32) && defined(WITH_MQTT_OBSERVER)

#include <Arduino.h>
#include <WiFi.h>
#include <ctype.h>
#include <esp_crt_bundle.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../mqtt_observer/MQTTObserver.h"
#include "OnlineOtaPolicy.h"

#if !defined(ESP_IDF_VERSION_MAJOR) || ESP_IDF_VERSION_MAJOR < 5
extern "C" {
extern const uint8_t x509_crt_bundle[];
}
#endif

namespace {
constexpr uint32_t kSocketTimeoutSec = 5;
constexpr uint16_t kMaxRedirects = 5;
constexpr uint32_t kMinValidTime = 1735689600UL;   // the observer's clock floor

bool startsWith(const char* s, const char* p) {
  return strncmp(s, p, strlen(p)) == 0;
}

bool startsWithCI(const char* s, const char* p) {
  while (*p) {
    if (tolower((unsigned char)*s) != tolower((unsigned char)*p)) return false;
    s++; p++;
  }
  return true;
}
} // namespace

void OnlineOtaNetEsp32::begin(MQTTObserver* observer) {
  _observer = observer;
}

void OnlineOtaNetEsp32::requestStation(bool want) {
  if (_observer != nullptr) _observer->requestStation(want);
}

bool OnlineOtaNetEsp32::stationConnected() {
  return WiFi.status() == WL_CONNECTED;
}

bool OnlineOtaNetEsp32::clockValid() {
  return (uint32_t)time(nullptr) >= kMinValidTime;
}

bool OnlineOtaNetEsp32::heapOk() {
  return _observer != nullptr && _observer->hasConnectHeadroom(true);
}

bool OnlineOtaNetEsp32::setUrl(const char* url) {
  const char* prefix = "https://";
  if (url == nullptr || strncmp(url, prefix, 8) != 0) return false;
  const char* host = url + 8;
  const char* slash = strchr(host, '/');
  if (slash == nullptr || slash == host) return false;
  const size_t hlen = (size_t)(slash - host);
  if (hlen == 0 || hlen >= sizeof(_host) || strlen(slash) >= sizeof(_path)) return false;
  memcpy(_host, host, hlen);
  _host[hlen] = 0;
  snprintf(_path, sizeof(_path), "%s", slash);
  return true;
}

bool OnlineOtaNetEsp32::beginGet(const char* url, uint32_t max_bytes) {
  cancel();
  if (url == nullptr || !heapOk() || !setUrl(url)) return false;
  _max_bytes = max_bytes;
  _redirects = 0;
  _phase = HttpPhase::Connect;
  return true;
}

OnlineOtaNet::Step OnlineOtaNetEsp32::step(const uint8_t** out, size_t* len, uint8_t* err) {
  *out = nullptr;
  *len = 0;
  if (_phase == HttpPhase::Idle || _phase == HttpPhase::Done) return Step::Done;

  if (_phase == HttpPhase::Connect) {
    // DNS, TCP and TLS use the framework's socket timeout; later phases poll
    // available() and never wait on the link.
    _client.setTimeout(kSocketTimeoutSec);
#if defined(ESP_IDF_VERSION_MAJOR) && ESP_IDF_VERSION_MAJOR >= 5
    _client.attachArduinoCACertBundle();
#else
    _client.setCACertBundle(x509_crt_bundle);
#endif
    if (!_client.connect(_host, 443)) {
      *err = ONLINE_ERR_TLS;
      cancel();
      return Step::Error;
    }
    char request[512];
    const int n = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\nHost: %s\r\nUser-Agent: MeshCore-OTA/1\r\n"
        "Accept: */*\r\nConnection: close\r\n\r\n", _path, _host);
    if (n <= 0 || n >= (int)sizeof(request) ||
        _client.write((const uint8_t*)request, (size_t)n) != (size_t)n) {
      *err = ONLINE_ERR_HTTP;
      cancel();
      return Step::Error;
    }
    _phase = HttpPhase::Headers;
    return Step::Busy;
  }

  if (_phase == HttpPhase::Headers) {
    if (_client.available() == 0) {
      if (!_client.connected()) { *err = ONLINE_ERR_HTTP; cancel(); return Step::Error; }
      return Step::Busy;
    }
    char line[384];   // a GitHub redirect Location can be long
    const int got = _client.readBytesUntil('\n', line, sizeof(line) - 1);
    if (got <= 0) return Step::Busy;
    line[got] = 0;
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\r' || line[n - 1] == '\n')) line[--n] = 0;
    if (n == 0) { _phase = HttpPhase::Body; return Step::Busy; }

    if (startsWith(line, "HTTP/")) {
      const char* sp = strchr(line, ' ');
      _status = sp != nullptr ? (uint16_t)strtoul(sp + 1, nullptr, 10) : 0;
    } else if (startsWithCI(line, "location:")) {
      const char* v = line + 9;
      while (*v == ' ') v++;
      snprintf(_location, sizeof(_location), "%s", v);
    } else if (startsWithCI(line, "content-length:")) {
      const char* v = line + 15;
      while (*v == ' ') v++;
      _body_length = strtoul(v, nullptr, 10);
      if (_body_length > _max_bytes) { *err = ONLINE_ERR_HTTP; cancel(); return Step::Error; }
    } else if (startsWithCI(line, "transfer-encoding:")) {
      // Refuse a chunked body rather than mis-stage it: release assets carry
      // a Content-Length.
      if (strstr(line + 18, "chunked") != nullptr) {
        *err = ONLINE_ERR_HTTP;
        cancel();
        return Step::Error;
      }
    }

    if (_status >= 300 && _status < 400 && _location[0] != 0) {
      if (++_redirects > kMaxRedirects) { *err = ONLINE_ERR_HTTP; cancel(); return Step::Error; }
      char next[sizeof(_location)];
      snprintf(next, sizeof(next), "%s", _location);
      const uint32_t max_bytes = _max_bytes;
      cancel();
      _max_bytes = max_bytes;   // closing the old socket must not drop the cap
      if (!setUrl(next)) { *err = ONLINE_ERR_HTTP; return Step::Error; }
      _phase = HttpPhase::Connect;
      return Step::Busy;
    }
    if (_status >= 400) { *err = ONLINE_ERR_HTTP; cancel(); return Step::Error; }
    return Step::Busy;
  }

  // Body: at most one reused buffer per call, only bytes already arrived.
  const size_t available = (size_t)_client.available();
  if (available == 0) {
    if (_client.connected()) return Step::Busy;
    const bool complete = _body_length == 0 || _received >= _body_length;
    cancel();
    if (!complete) { *err = ONLINE_ERR_DOWNLOAD; return Step::Error; }
    return Step::Done;
  }
  size_t want = available < sizeof(_buffer) ? available : sizeof(_buffer);
  if (_received < _max_bytes) {
    const size_t room = (size_t)(_max_bytes - _received);
    if (want > room) want = room;
  }
  const int read = _client.read(_buffer, want);
  if (read <= 0) return Step::Busy;
  _received += (uint32_t)read;
  *out = _buffer;
  *len = (size_t)read;
  if (_body_length != 0 && _received >= _body_length) {
    cancel();
    return Step::Done;
  }
  return Step::More;
}

void OnlineOtaNetEsp32::cancel() {
  if (_phase != HttpPhase::Idle) _client.stop();
  _phase = HttpPhase::Idle;
  _received = 0;
  _max_bytes = 0;
  _body_length = 0;
  _status = 0;
  _location[0] = 0;
}

#endif // ESP32 && WITH_MQTT_OBSERVER
