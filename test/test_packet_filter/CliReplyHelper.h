// CliReplyHelper.h — the shared "run a CLI command and check the reply"
// helper for the fork's host suites.
//
// A CLI reply travels in a 160-byte buffer, and a handler that overruns it
// would write into whatever follows on the stack. The check belongs in one
// place: every suite that drives a CLI entry point wants the same guarantee,
// and two copies of it had already drifted (the fleet's copy said so in a
// comment).
//
// Test-only support, shared by the suites that drive a CLI — not a mock, but
// it lives with the other shared native test pieces.

#pragma once

#include <cstdint>
#include <cstring>
#include <string>

#include <gtest/gtest.h>

#include "CliUtil.h"   // CLI_REPLY_MAX: the transport bound every reply must hold

// A canaried reply buffer: the real CLI_REPLY_MAX plus a canary byte after
// it, so a reply that overran the transport limit is caught here instead of
// silently passing. The reply is NUL-terminated inside the buffer, and
// check() proves both.
struct CliReplyBuf {
  uint8_t raw[CLI_REPLY_MAX + 1];

  CliReplyBuf() {
    memset(raw, 0xA5, sizeof(raw));
    reply()[0] = 0;
  }

  char* reply() { return (char*)raw; }

  void check(const char* what) const {
    EXPECT_EQ(raw[CLI_REPLY_MAX], (uint8_t)0xA5)
        << "reply overran " << CLI_REPLY_MAX << " bytes: " << what;
    EXPECT_NE(memchr(raw, 0, CLI_REPLY_MAX), nullptr)
        << "reply not NUL-terminated in " << CLI_REPLY_MAX << " bytes: " << what;
  }
};
