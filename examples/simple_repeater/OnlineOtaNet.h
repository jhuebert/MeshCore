// OnlineOtaNet.h — the network surface the online-OTA state machine needs.
// Kept as a narrow interface so OnlineOtaCore is host-testable with a fake.
// The ESP32 observer adapter implements it in OnlineOtaNet.cpp; other builds
// compile no adapter and never install one.

#ifndef _ONLINE_OTA_NET_H
#define _ONLINE_OTA_NET_H

#include <stddef.h>
#include <stdint.h>

class OnlineOtaNet {
public:
  // Busy: no bytes yet. More: a chunk was delivered and the body continues.
  // Done: the final chunk (possibly empty) was delivered.
  enum class Step : uint8_t { Busy, More, Done, Error };

  virtual ~OnlineOtaNet() {}
  virtual bool available() const = 0;
  virtual void requestStation(bool want) = 0;
  virtual bool stationConnected() = 0;
  virtual bool clockValid() = 0;
  virtual bool heapOk() = 0;
  // 0 until the response headers have been parsed; then the Content-Length.
  virtual uint32_t contentLength() const = 0;
  virtual bool beginGet(const char* url, uint32_t max_bytes) = 0;
  // The chunk buffer belongs to the adapter and stays valid until cancel().
  virtual Step step(const uint8_t** chunk, size_t* len, uint8_t* err) = 0;
  virtual void cancel() = 0;
};

#if defined(ESP32) && defined(WITH_MQTT_OBSERVER)

#include <WiFiClientSecure.h>

class MQTTObserver;

// Compiled only into repeater observer firmware. It borrows the observer's
// station, clock and heap gate; it never owns WiFi. One reused 2 KiB buffer,
// one bounded chunk per loop() step.
class OnlineOtaNetEsp32 : public OnlineOtaNet {
public:
  void begin(MQTTObserver* observer);

  bool available() const override { return _observer != nullptr; }
  void requestStation(bool want) override;
  bool stationConnected() override;
  bool clockValid() override;
  bool heapOk() override;
  uint32_t contentLength() const override { return _body_length; }
  bool beginGet(const char* url, uint32_t max_bytes) override;
  Step step(const uint8_t** chunk, size_t* len, uint8_t* err) override;
  void cancel() override;

private:
  enum class HttpPhase : uint8_t { Idle, Connect, Headers, Body, Done };
  bool setUrl(const char* url);

  MQTTObserver* _observer = nullptr;
  WiFiClientSecure _client;
  HttpPhase _phase = HttpPhase::Idle;
  uint32_t _max_bytes = 0;
  uint32_t _received = 0;
  uint32_t _body_length = 0;
  uint8_t _redirects = 0;
  uint16_t _status = 0;
  char _host[128];
  // A GitHub release redirect carries a ~950 byte signed URL. These are
  // members, not stack frames, and overlong headers fail closed below.
  char _path[1024];
  char _location[1024];
  char _line[1024];
  char _request[1200];   // GET path + headers after a GitHub redirect
  uint8_t _buffer[2048];
};

#endif // ESP32 && WITH_MQTT_OBSERVER

#endif // _ONLINE_OTA_NET_H
