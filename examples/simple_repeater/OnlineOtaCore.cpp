// OnlineOtaCore.cpp — async release download state machine.
// Only millis() comes from Arduino, so the whole machine is host-tested with
// a fake network. Compiled only into observer builds.

#include "OtaUpdate.h"   // defines OTA_HAS_ONLINE_FETCH for observer builds

#ifdef OTA_HAS_ONLINE_FETCH

#include "OnlineOtaCore.h"

#include <Arduino.h>
#include <stdio.h>
#include <string.h>

#include "CliUtil.h"

namespace {
constexpr uint32_t kStationTimeoutMs = 30000;
constexpr uint32_t kClockTimeoutMs = 45000;

bool expired(uint32_t deadline) {
  return (uint32_t)(millis() - deadline) < 0x80000000u;
}
} // namespace

OnlineOtaCore::OnlineOtaCore()
    : _net(nullptr), _ota(nullptr), _slot_size(0), _phase(Phase::Idle), _err(0),
      _ver_set(false), _sha_set(false), _size(0), _got(0), _deadline(0) {
  _env[0] = 0;
  _version[0] = 0;
  _url[0] = 0;
  memset(_sha, 0, sizeof(_sha));
}

const char* OnlineOtaCore::phaseName(Phase phase) {
  switch (phase) {
    case Phase::Wifi:   return "wifi";
    case Phase::Clock:  return "clock";
    case Phase::Asset:  return "asset";
    case Phase::Verify: return "verify";
    case Phase::Ready:  return "ready";
    case Phase::Error:  return "error";
    default:            return "idle";
  }
}

void OnlineOtaCore::begin(OnlineOtaNet* net, OtaUpdate* ota,
                          const char* env, uint32_t slot_size) {
  _net = net;
  _ota = ota;
  _slot_size = slot_size;
  snprintf(_env, sizeof(_env), "%s", env != nullptr ? env : "");
}

bool OnlineOtaCore::setVersion(const char* version, char* reply) {
  char canonical[ONLINE_OTA_MAX_VERSION + 1];
  if (!OnlineOtaPolicy::normalizeVersion(version, canonical, sizeof(canonical))) {
    snprintf(reply, CLI_REPLY_MAX, "Err - invalid version token");
    return false;
  }
  snprintf(_version, sizeof(_version), "%s", canonical);
  _ver_set = true;
  snprintf(reply, CLI_REPLY_MAX, "OK - online version set ver=%s", _version);
  return true;
}

bool OnlineOtaCore::setSha(const char* sha, char* reply) {
  uint8_t digest[32];
  if (!OnlineOtaPolicy::parseSha(sha, digest)) {
    snprintf(reply, CLI_REPLY_MAX, "Err - sha256 must be 64 hex chars");
    return false;
  }
  memcpy(_sha, digest, sizeof(_sha));
  _sha_set = true;
  snprintf(reply, CLI_REPLY_MAX, "OK - online sha set sha=%02x%02x%02x%02x%02x%02x%02x%02x",
           _sha[0], _sha[1], _sha[2], _sha[3], _sha[4], _sha[5], _sha[6], _sha[7]);
  return true;
}

bool OnlineOtaCore::start(char* reply) {
  if (!_ver_set || !_sha_set) {
    snprintf(reply, CLI_REPLY_MAX, "Err - set ver and sha first");
    return false;
  }
  if (_env[0] == 0 || _net == nullptr || !_net->available() || _ota == nullptr ||
      _slot_size == 0) {
    snprintf(reply, CLI_REPLY_MAX, "Err - unsupported board");
    return false;
  }
  if (_ota->state() == OtaUpdate::State::Staging ||
      _ota->state() == OtaUpdate::State::Ready) {
    snprintf(reply, CLI_REPLY_MAX, "Err - staging active; abort first");
    return false;
  }
  if (_ota->batterySuspended()) {
    snprintf(reply, CLI_REPLY_MAX, "Err - battery gate active; staging refused");
    return false;
  }
  if (!OnlineOtaPolicy::buildAssetUrl(_version, _env, _url, sizeof(_url))) {
    snprintf(reply, CLI_REPLY_MAX, "Err - invalid version token");
    return false;
  }

  _err = 0;
  _size = 0;
  _got = 0;
  _phase = Phase::Wifi;
  _deadline = millis() + kStationTimeoutMs;
  _net->requestStation(true);
  snprintf(reply, CLI_REPLY_MAX, "OK - online fetch armed ver=%s", _version);
  return true;
}

void OnlineOtaCore::fail(uint8_t err) {
  if (_net != nullptr) {
    _net->cancel();
    _net->requestStation(false);
  }
  if (_ota != nullptr) {
    if (_ota->localStageActive()) _ota->cancelLocalStage();
    if (_ota->state() != OtaUpdate::State::Error || _ota->errorCode() != err) {
      _ota->noteOnlineError(err, nullptr);
    }
  }
  _err = err;
  _phase = Phase::Error;
}

void OnlineOtaCore::loop() {
  if (_phase == Phase::Idle || _phase == Phase::Ready) return;
  if (_net == nullptr || !_net->available()) { fail(ONLINE_ERR_CFG); return; }
  if (_ota != nullptr && _ota->batterySuspended()) { fail(OTA_ERR_BATTERY); return; }

  switch (_phase) {
    case Phase::Wifi:
      if (_net->stationConnected()) {
        _phase = Phase::Clock;
        _deadline = millis() + kClockTimeoutMs;
      } else if (expired(_deadline)) {
        fail(ONLINE_ERR_WIFI);
      }
      break;
    case Phase::Clock:
      if (!_net->heapOk()) {
        fail(ONLINE_ERR_HEAP);
      } else if (_net->clockValid()) {
        if (!_net->beginGet(_url, _slot_size)) fail(ONLINE_ERR_TLS);
        else _phase = Phase::Asset;
      } else if (expired(_deadline)) {
        fail(ONLINE_ERR_CLOCK);
      }
      break;
    case Phase::Asset: {
      const uint8_t* chunk = nullptr;
      size_t len = 0;
      uint8_t err = 0;
      const OnlineOtaNet::Step step = _net->step(&chunk, &len, &err);
      if (step == OnlineOtaNet::Step::Error) { fail(err != 0 ? err : ONLINE_ERR_HTTP); return; }
      // Content-Length is the image size: it is known once the headers are in.
      if (_size == 0 && _net->contentLength() != 0) {
        const uint32_t size = _net->contentLength();
        if (size > _slot_size) { fail(ONLINE_ERR_HTTP); return; }
        if (_ota == nullptr || !_ota->beginLocalStage(size, _sha, _version, nullptr)) {
          fail(_ota != nullptr ? _ota->errorCode() : ONLINE_ERR_DOWNLOAD);
          return;
        }
        _size = size;
      }
      if (step == OnlineOtaNet::Step::Busy) return;
      if (chunk != nullptr && len > 0) {
        if (_ota == nullptr || !_ota->appendLocalStage(chunk, len)) {
          fail(_ota != nullptr ? _ota->errorCode() : ONLINE_ERR_DOWNLOAD);
          return;
        }
        _got += (uint32_t)len;
      }
      if (step == OnlineOtaNet::Step::Done) {
        if (_size == 0 || _got != _size) { fail(ONLINE_ERR_DOWNLOAD); return; }
        _phase = Phase::Verify;
      }
      break;
    }
    case Phase::Verify:
      if (_ota != nullptr && _ota->finishLocalStage(nullptr)) {
        _phase = Phase::Ready;
        _net->cancel();
        _net->requestStation(false);
      } else {
        fail(_ota != nullptr ? _ota->errorCode() : ONLINE_ERR_DOWNLOAD);
      }
      break;
    default:
      break;
  }
}

void OnlineOtaCore::abort() {
  if (_net != nullptr) {
    _net->cancel();
    _net->requestStation(false);
  }
  if (_ota != nullptr && _ota->localStageActive()) _ota->cancelLocalStage();
  _phase = Phase::Idle;
  _err = 0;
  _size = 0;
  _got = 0;
  _ver_set = false;
  _sha_set = false;
  _version[0] = 0;
  _url[0] = 0;
  memset(_sha, 0, sizeof(_sha));
}

void OnlineOtaCore::formatNet(char* reply, size_t cap) const {
  snprintf(reply, cap, "net=%s ver=%s sha=%02x%02x%02x%02x got=%lu size=%lu err=%u",
           phaseName(_phase), _version,
           _sha[0], _sha[1], _sha[2], _sha[3],
           (unsigned long)_got, (unsigned long)_size, (unsigned)_err);
}

void onlineOtaCLI(OnlineOtaCore& core, const char* sub, char* reply) {
  char scratch[CLI_REPLY_BUFFER_MAX + 1];
  if (!cliPrepare(sub, scratch, sizeof(scratch), reply)) return;
  char* p = scratch;
  char* cmd = nextToken(&p);

  if (cmd == nullptr || strcmp(cmd, "net") == 0) {
    if (cmd != nullptr && !cliNoExtra(p, reply, OTA_USAGE)) return;
    core.formatNet(reply, CLI_REPLY_MAX);
    return;
  }
  if (strcmp(cmd, "fetch") == 0) {
    char* verb = nextToken(&p);
    if (verb == nullptr) { snprintf(reply, CLI_REPLY_MAX, "%s", OTA_USAGE); return; }
    if (strcmp(verb, "ver") == 0) {
      char* value = nextToken(&p);
      if (value == nullptr || nextToken(&p) != nullptr) {
        snprintf(reply, CLI_REPLY_MAX, "%s", OTA_USAGE);
        return;
      }
      core.setVersion(value, reply);
      return;
    }
    if (strcmp(verb, "sha") == 0) {
      char* value = nextToken(&p);
      if (value == nullptr || nextToken(&p) != nullptr) {
        snprintf(reply, CLI_REPLY_MAX, "%s", OTA_USAGE);
        return;
      }
      core.setSha(value, reply);
      return;
    }
    if (strcmp(verb, "start") == 0) {
      if (!cliNoExtra(p, reply, OTA_USAGE)) return;
      core.start(reply);
      return;
    }
  }
  snprintf(reply, CLI_REPLY_MAX, "%s", OTA_USAGE);
}

#endif // OTA_HAS_ONLINE_FETCH
