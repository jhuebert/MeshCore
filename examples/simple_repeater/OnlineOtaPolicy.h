// OnlineOtaPolicy.h — pure, Arduino-free helpers for online OTA over GitHub
// Releases. Fork-owned. Release assets are named <env>-v<version>.bin, so the
// node builds the download URL from its version token and its own env. There
// is no manifest, no API lookup and no JSON in firmware.

#ifndef _ONLINE_OTA_POLICY_H
#define _ONLINE_OTA_POLICY_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Online-only errors, appended to the frozen LoRa OTA codes 0..9.
#define ONLINE_ERR_CFG      10
#define ONLINE_ERR_WIFI     11
#define ONLINE_ERR_CLOCK    12
#define ONLINE_ERR_TLS      13
#define ONLINE_ERR_HTTP     14
#define ONLINE_ERR_DOWNLOAD 15
#define ONLINE_ERR_HEAP     16

#define ONLINE_OTA_MAX_VERSION 63
#define ONLINE_OTA_MAX_ENV     64
#define ONLINE_OTA_MAX_URL     320
#define ONLINE_OTA_BASE_URL "https://github.com/jhuebert/MeshCore/releases/download/"
// The fork's release tag prefix. Kept here so the planned rename away from
// "filter" is one edit.
#define ONLINE_OTA_TAG_PREFIX "filter-v"

namespace OnlineOtaPolicy {

inline bool validVersionChar(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
         (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

// Strip surrounding spaces, one leading v/V and an optional -filter-<sha>,
// so a version copied out of `ota status` is accepted.
inline bool normalizeVersion(const char* input, char* out, size_t cap) {
  if (input == nullptr || out == nullptr || cap == 0) return false;
  while (*input == ' ') input++;
  size_t end = strlen(input);
  while (end > 0 && input[end - 1] == ' ') end--;
  const size_t start = (end > 0 && (*input == 'v' || *input == 'V')) ? 1 : 0;
  for (size_t i = start; i + 8 <= end; i++) {
    if (memcmp(input + i, "-filter-", 8) == 0) { end = i; break; }
  }
  const size_t len = end - start;
  if (len == 0 || len > ONLINE_OTA_MAX_VERSION || len + 1 > cap) return false;
  for (size_t i = 0; i < len; i++) {
    const char c = input[start + i];
    if (!validVersionChar(c)) return false;
    out[i] = c;
  }
  out[len] = 0;
  return true;
}

inline int nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Accepts an optional "sha256:" prefix followed by exactly 64 hex digits.
inline bool parseSha(const char* input, uint8_t out[32]) {
  if (input == nullptr) return false;
  const char* hex = input;
  if (strncmp(hex, "sha256:", 7) == 0 ||
      strncmp(hex, "SHA256:", 7) == 0) hex += 7;
  if (strlen(hex) != 64) return false;
  for (int i = 0; i < 32; i++) {
    const int hi = nibble(hex[i * 2]);
    const int lo = nibble(hex[i * 2 + 1]);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

inline bool buildAssetUrl(const char* version, const char* env, char* out, size_t cap) {
  if (version == nullptr || env == nullptr || out == nullptr || cap == 0) return false;
  const int n = snprintf(out, cap, ONLINE_OTA_BASE_URL ONLINE_OTA_TAG_PREFIX
                         "%s/%s-v%s.bin", version, env, version);
  return n > 0 && (size_t)n < cap;
}

} // namespace OnlineOtaPolicy

#endif // _ONLINE_OTA_POLICY_H
