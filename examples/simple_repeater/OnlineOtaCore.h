// OnlineOtaCore.h — RAM-only async state machine for a release download.
// Fork-owned. Three CLI steps: set the version, set the expected SHA-256,
// then start. Nothing is persisted; abort or reboot drops the job.

#ifndef _ONLINE_OTA_CORE_H
#define _ONLINE_OTA_CORE_H

#include <stddef.h>
#include <stdint.h>

#include "OnlineOtaNet.h"
#include "OnlineOtaPolicy.h"

class OtaUpdate;

class OnlineOtaCore {
public:
  enum class Phase : uint8_t { Idle, Wifi, Clock, Asset, Verify, Ready, Error };
  static const char* phaseName(Phase phase);

  OnlineOtaCore();
  void begin(OnlineOtaNet* net, OtaUpdate* ota, const char* env, uint32_t slot_size);

  bool setVersion(const char* version, char* reply);
  bool setSha(const char* sha, char* reply);
  bool start(char* reply);
  void loop();
  void abort();
  bool needsAwake() const { return _phase != Phase::Idle && _phase != Phase::Ready; }
  void formatNet(char* reply, size_t cap) const;

  Phase phase() const { return _phase; }
  uint8_t error() const { return _err; }
  const char* version() const { return _version; }
  uint32_t received() const { return _got; }
  uint32_t expected() const { return _size; }

private:
  void fail(uint8_t err);

  OnlineOtaNet* _net;
  OtaUpdate* _ota;
  char _env[ONLINE_OTA_MAX_ENV];
  uint32_t _slot_size;
  Phase _phase;
  uint8_t _err;
  bool _ver_set, _sha_set;
  char _version[ONLINE_OTA_MAX_VERSION + 1];
  uint8_t _sha[32];
  char _url[ONLINE_OTA_MAX_URL];
  uint32_t _size, _got;
  uint32_t _deadline;
};

// The online verbs, dispatched from MyMesh::handleCommand() before otaCLI().
void onlineOtaCLI(OnlineOtaCore& core, const char* sub, char* reply);

#endif // _ONLINE_OTA_CORE_H
