// KeyedChannel.h — keyed-channel primitives shared by the repeater's packet
// filter, fleet channel and chat bot. Fork-owned; the hash-match, duplicate-key
// and candidate-slot rules live here once so the three features cannot drift.
//
// A keyed channel is a 16- or 32-byte PSK, zero-padded in a 32-byte buffer,
// with a 1-byte on-air hash sha256(secret)[0]. Hashtag channels derive their
// secret from their name the way the companion apps do.

#ifndef _KEYED_CHANNEL_H
#define _KEYED_CHANNEL_H

#include <string.h>
#include <Mesh.h>
#include <Utils.h>
#include "CliUtil.h"

// A channel name has to survive a round trip through a rule's chan= list,
// which is comma-separated and made of `key=value` tokens. ',', '=' and '"'
// therefore cannot be represented there, and a control character (a NUL above
// all) would truncate the stored name. Ordinary spaces are fine: the value is
// quoted where it is printed. Names already in the store are never re-validated
// — an old config stays loadable even if it predates this rule.
inline bool validChannelName(const char* name) {
  for (const unsigned char* c = (const unsigned char*)name; *c; c++) {
    if (*c < 0x20 || *c == 0x7f) return false;
    if (*c == ',' || *c == '=' || *c == '"') return false;
  }
  return true;
}

// Derive a hashtag channel's 16-byte secret and its 1-byte on-air hash from
// the name (including the leading '#'), per the companion protocol.
inline void deriveChannelKey(const char* name, uint8_t* secret, uint8_t* hash) {
  uint8_t digest[32];
  mesh::Utils::sha256(digest, sizeof(digest), (const uint8_t*)name, strlen(name));
  memcpy(secret, digest, 16);
  mesh::Utils::sha256(hash, 1, secret, 16);
}

// Resolve a user-supplied channel key spec into a padded secret, its length and
// its 1-byte on-air hash. "#name" derives the PSK from the name (companion-app
// convention); anything else must be 32 or 64 hex digits. The secret is ALWAYS
// zero-padded to 32 bytes: core detects a 128-bit key by testing bytes 16..31
// for zero (BaseChatMesh.cpp) and HMACs all 32 bytes, so a garbage tail makes
// the channel match its hash but fail to decrypt. Zeroing before parsing keeps
// that invariant in one place instead of relying on every caller to memset.
inline bool parseChannelSpec(const char* spec, uint8_t* secret, uint8_t* len, uint8_t* hash) {
  if (spec == NULL || secret == NULL || len == NULL || hash == NULL || spec[0] == 0) return false;
  memset(secret, 0, PUB_KEY_SIZE);
  if (spec[0] == '#') {
    size_t n = strlen(spec);
    if (n < 2 || n > 15 || !validChannelName(spec)) return false;
    deriveChannelKey(spec, secret, hash);
    *len = 16;
  } else {
    int n = 0;
    if (!cliDecodePskHex(spec, secret, PUB_KEY_SIZE, &n)) return false;
    *len = (uint8_t)n;
    mesh::Utils::sha256(hash, 1, secret, n);
  }
  return true;
}

// Offer one key to core's candidate list. Only the `filled` prefix of dest is
// initialised (core passes raw stack), and a key already offered there must
// not burn another of core's few candidate slots. Returns 1 when a slot was
// filled, 0 when the hash did not match, the list is full, or the key is a
// duplicate. Callers that need the "full" case for telemetry test it before
// calling here.
inline int appendKeyedChannel(const uint8_t* hash, const uint8_t* secret, uint8_t chan_hash,
                              mesh::GroupChannel dest[], int max_matches, int filled) {
  if (chan_hash != hash[0] || filled >= max_matches) return 0;
  for (int i = 0; i < filled; i++) {
    if (memcmp(dest[i].secret, secret, sizeof(dest[i].secret)) == 0) return 0;
  }
  dest[filled].hash[0] = chan_hash;
  memcpy(dest[filled].secret, secret, sizeof(dest[filled].secret));
  return 1;
}

#endif // _KEYED_CHANNEL_H
