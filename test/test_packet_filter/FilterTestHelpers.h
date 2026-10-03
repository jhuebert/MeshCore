// FilterTestHelpers.h — shared fixture and packet builders for the packet
// filter native tests.

#pragma once

#include <gtest/gtest.h>

#include <string>

#include "NativeShim.h"
#include <Arduino.h>        // g_mock_millis
#include <helpers/TxtDataHelpers.h>   // TXT_TYPE_PLAIN
#include "PacketFilter.h"
#include "CliUtil.h"        // CLI_REPLY_MAX

// payload offsets the advert rate limiter samples its 4-byte origin key from
// (mirrors the fixed KEY_OFFSETS in PacketFilter.cpp)
static const uint8_t ADV_KEY_OFFSETS[4] = { 8, 14, 20, 26 };

// Build a packet with the given header fields. mesh::Packet's constructor only
// initializes header/path_len/payload_len, so the remaining fields a matcher or
// the packet hash can read are set here explicitly: an uninitialized packet made
// tests hash whatever the stack happened to hold. Path entries are filled with
// a recognisable pattern: entry e, byte b = (e+1)*16 + b, so with hash size 1
// the entries are 0x10, 0x20, 0x30, ...; with size 2 entry 0 is 0x10 0x11.
//
// payload_len is clamped to the packet's real buffer: there is no packet the
// firmware could process with more payload than that, and a larger value reads
// past payload[] (which is what the probability hash used to do).
inline mesh::Packet makePacket(uint8_t route, uint8_t type, uint16_t payload_len = 10,
                               uint8_t hsz = 1, uint8_t hops = 0, int8_t snr_qdb = 0) {
  mesh::Packet p;
  p.header = (route & PH_ROUTE_MASK) | ((type & PH_TYPE_MASK) << PH_TYPE_SHIFT);
  if (payload_len > MAX_PACKET_PAYLOAD) payload_len = MAX_PACKET_PAYLOAD;
  if (hsz > MAX_PATH_SIZE) hsz = MAX_PATH_SIZE;
  if (hops * hsz > MAX_PATH_SIZE) hops = hsz ? (uint8_t)(MAX_PATH_SIZE / hsz) : 0;
  p.setPathHashSizeAndCount(hsz, hops);
  p.payload_len = payload_len;
  p._snr = snr_qdb;
  memset(p.payload, 0, sizeof(p.payload));
  memset(p.path, 0, sizeof(p.path));
  p.transport_codes[0] = p.transport_codes[1] = 0;
  for (uint8_t e = 0; e < hops && e * hsz < MAX_PATH_SIZE; e++) {
    for (uint8_t b = 0; b < hsz && e * hsz + b < MAX_PATH_SIZE; b++) {
      p.path[e * hsz + b] = (uint8_t)(((e + 1) << 4) | b);
    }
  }
  return p;
}

inline mesh::Packet makeAdvert(const uint8_t key_prefix[4], int8_t snr_qdb = 0) {
  mesh::Packet p = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_ADVERT, 64, 1, 2, snr_qdb);
  for (int i = 0; i < 4; i++) p.payload[ADV_KEY_OFFSETS[i]] = key_prefix[i];
  return p;
}

// GRP_TXT decrypted payload: ts(4) | txt_type(1) | "<sender>: <text>"
struct GroupTextPayload {
  uint8_t data[MAX_PACKET_PAYLOAD];
  size_t len;
};

inline GroupTextPayload makeGroupText(const char* sender, const char* text) {
  GroupTextPayload p;
  memset(&p, 0, sizeof(p));
  uint32_t ts = 12345;
  memcpy(p.data, &ts, 4);
  p.data[4] = TXT_TYPE_PLAIN;
  size_t off = 5;
  size_t slen = strlen(sender);
  memcpy(&p.data[off], sender, slen);
  off += slen;
  p.data[off++] = ':';
  if (text[0]) p.data[off++] = ' ';
  size_t tlen = strlen(text);
  memcpy(&p.data[off], text, tlen);
  off += tlen;
  p.len = off;
  return p;
}

// mesh::GroupChannel view of one store entry (for checkContent())
inline mesh::GroupChannel channelFromStore(FilterRules& filter, int idx) {
  mesh::GroupChannel c;
  memset(&c, 0, sizeof(c));
  auto ch = filter.getChannel(idx);
  c.hash[0] = ch->hash;
  memcpy(c.secret, ch->secret, sizeof(c.secret));
  return c;
}

// Run one CLI command (with the "filter" prefix already stripped) and return
// the reply. The buffer is the real CLI_REPLY_MAX with a canary byte after it,
// so a reply that overruns the transport limit is caught here rather than
// silently passing. `regions` is a real RegionMap: the CLI calls members on it,
// and calling through a null pointer is undefined behaviour.
inline std::string cli(FilterRules& filter, const char* command) {
  static const uint8_t CANARY = 0xA5;
  static TransportKeyStore store;
  static RegionMap regions(store);

  uint8_t raw[CLI_REPLY_MAX + 1];
  memset(raw, CANARY, sizeof(raw));
  char* reply = (char*)raw;
  reply[0] = 0;
  filterCLI(filter, command, reply, &regions);
  EXPECT_EQ(raw[CLI_REPLY_MAX], CANARY) << "reply overran " << CLI_REPLY_MAX << " bytes: " << command;
  EXPECT_NE(memchr(reply, 0, CLI_REPLY_MAX), nullptr)
      << "reply not NUL-terminated in " << CLI_REPLY_MAX << " bytes: " << command;
  return std::string(reply);
}

// Run a CLI command expected to succeed (reply begins "OK ").
inline void expectOk(FilterRules& filter, const char* command) {
  std::string reply = cli(filter, command);
  ASSERT_EQ(reply.substr(0, 3), "OK ") << command;
}

// Shared fixture: fresh filter with the persisted Public channel provisioned
// and no rules.
class FilterTest : public ::testing::Test {
protected:
  NativeFS fs;
  FilterRules filter;

  void SetUp() override {
    g_mock_millis = 0;
    filter.begin(&fs);
    filter.clearRules();
  }
};
