// test_packet_filter.cpp — native unit tests for the simple_repeater packet
// filter (examples/simple_repeater/PacketFilter* + TinyRegex). Built by
// [env:native_packet_filter] in platformio.ini; see that env for the shim and
// stub notes.
//
// Sections: TinyRegex, packet-level matching, content rules, advert rate
// limiter, management/persistence, and the filter CLI.
//
// Coverage map (2026-09-13 host-native gap analysis; on-air semantics mirrored
// from the Phase-4 raw-packet suites):
//   hops exact/range/exclusive/unbounded/direct  -> HopsExactAndRange,
//       HopsExclusiveBounds, HopsUnboundedBelowAndAbove, HopsIgnoredForDirectTraffic
//   path anchors/chains/windows/clamping/0-hop   -> Path* tests
//   region lists + unscoped + exact-name         -> Region* tests
//   type/route/hsize/len/snr                     -> Type/Route/Hsize/Len/Snr* tests
//   content rules + keyed channels               -> content-rules section
//   limiter windows/cache/origin keys            -> advert rate limiter section
//   persistence roundtrip/upgrade/truncation     -> management/persistence section
//   CLI quoting/errors/capacity                  -> filter CLI surface section
//   regex budget/anchors/classes                 -> TinyRegex* tests
//   hook order (battery gate -> checkPacket -> disable_fwd, MyMesh.cpp
//       allowPacketForward; checkContent in onGroupDataRecv) is a MyMesh
//       wiring property, asserted by code review of the hook lines, not here

#include <gtest/gtest.h>

#include <string>

#include "FilterTestHelpers.h"

// ============================================================
// UNIT TESTS: TinyRegex (vendored tiny-regex-c + step budget)
// ============================================================

// Native tests for TinyRegex — the vendored tiny-regex-c with the fork's
// backtracking step budget and UTF-8-safe literal matching.

#include <gtest/gtest.h>

#include <cstring>
#include <string>

#include "TinyRegex.h"

static int match(const char* pattern, const char* text, int* len = nullptr) {
  int l;
  int idx = re_match(pattern, text, &l);
  if (len) *len = l;
  return idx;
}

TEST(TinyRegexLiterals, PlainMatch) {
  int len;
  EXPECT_EQ(match("hello", "say hello world", &len), 4);
  EXPECT_EQ(len, 5);
}

TEST(TinyRegexLiterals, NoMatch) {
  EXPECT_EQ(match("xyz", "say hello world"), -1);
}

TEST(TinyRegexLiterals, EmptyPatternMatchesEmptyAtStart) {
  EXPECT_EQ(match("", "abc"), 0);
}

TEST(TinyRegexAnchors, StartAnchor) {
  EXPECT_EQ(match("^hello", "hello world"), 0);
  EXPECT_EQ(match("^ello", "hello world"), -1);
}

TEST(TinyRegexAnchors, EndAnchor) {
  EXPECT_EQ(match("world$", "hello world"), 6);
  EXPECT_EQ(match("worl$", "hello world"), -1);
}

TEST(TinyRegexAnchors, BothAnchors) {
  EXPECT_EQ(match("^abc$", "abc"), 0);
  EXPECT_EQ(match("^abc$", "abcd"), -1);
}

TEST(TinyRegexClasses, Dot) {
  EXPECT_EQ(match("h.llo", "hello"), 0);
  EXPECT_EQ(match("h.llo", "hallo"), 0);
}

TEST(TinyRegexClasses, CharClass) {
  EXPECT_EQ(match("[abc]+", "xxabbay"), 2);
  EXPECT_EQ(match("[abc]+", "xxxy"), -1);
}

TEST(TinyRegexClasses, InvertedClass) {
  EXPECT_EQ(match("[^abc]+", "abxyzab"), 2);
  EXPECT_EQ(match("[^abc]+", "abcabc"), -1);
}

TEST(TinyRegexClasses, RangeClass) {
  EXPECT_EQ(match("[a-f]+", "zzabcdefzz"), 2);
  EXPECT_EQ(match("[0-9]+", "abc123"), 3);
}

TEST(TinyRegexEscapes, DigitAndWord) {
  EXPECT_EQ(match("\\d+", "ab-1234"), 3);
  EXPECT_EQ(match("\\w+", "  foo_1"), 2);
  EXPECT_EQ(match("\\W+", "ab!!?"), 2);
}

TEST(TinyRegexEscapes, Whitespace) {
  EXPECT_EQ(match("\\s+", "ab  \t c"), 2);
  EXPECT_EQ(match("\\S+", "   abc"), 3);
}

TEST(TinyRegexEscapes, Utf8LiteralBytes) {
  // fork fix: literal bytes >= 0x80 compare as unsigned char (multi-byte
  // UTF-8, e.g. emoji) — subject/pattern are raw bytes
  const char* subject = "hi \xF0\x9F\x98\x80 there";
  EXPECT_EQ(match("i \xF0\x9F\x98\x80", subject), 1);
  EXPECT_EQ(match("\xF0\x9F\x98\x80 there", subject), 3);
}

TEST(TinyRegexQuantifiers, StarPlusQuestion) {
  EXPECT_EQ(match("ab*c", "ac"), 0);
  EXPECT_EQ(match("ab+c", "abc"), 0);
  EXPECT_EQ(match("ab+c", "ac"), -1);
  EXPECT_EQ(match("ab?c", "ac"), 0);
  EXPECT_EQ(match("ab?c", "abbc"), -1);
}

TEST(TinyRegexQuantifiers, GreedyStarScans) {
  int len;
  EXPECT_EQ(match("a*x", "baaaaaxb", &len), 1);
  EXPECT_EQ(len, 6);
}

TEST(TinyRegexBudget, AbortReportsExhaustion) {
  int len;
  re_set_step_budget(10);
  // nested quantifiers over a long non-matching subject exhaust the budget
  EXPECT_EQ(re_match("a*a*a*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &len), -1);
  EXPECT_TRUE(re_budget_exhausted());

  // budget reset per call: a cheap match afterwards is fine
  EXPECT_EQ(re_match("abc", "xxabcxx", &len), 2);
  EXPECT_FALSE(re_budget_exhausted());

  re_set_step_budget(0);   // unlimited
  EXPECT_EQ(re_match("a*a*a*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", &len), -1);
  EXPECT_FALSE(re_budget_exhausted());

  re_set_step_budget(5000);   // restore the compiled-in default
}

TEST(TinyRegexBudget, CompletableMatchWithinBudget) {
  int len;
  re_set_step_budget(10);
  EXPECT_EQ(re_match("a*b", "aaab", &len), 0);
  EXPECT_FALSE(re_budget_exhausted());
  re_set_step_budget(5000);
}

TEST(TinyRegexCompile, ValidPatternsCompile) {
  EXPECT_TRUE(re_compile("^hello world$") != nullptr);
  EXPECT_TRUE(re_compile("[a-zA-Z0-9_]+") != nullptr);
}

TEST(TinyRegexCompile, TrailingBackslashRejected) {
  // regression: a trailing '\' used to fall through compile into stale static
  // state and return a wild pointer (global-buffer-overflow, found by fuzzing)
  EXPECT_TRUE(re_compile("abc\\") == nullptr);
  EXPECT_EQ(match("abc\\", "abc"), -1);
}

TEST(TinyRegexCompile, OverlongPatternRejectedNotTruncated) {
  // the engine holds MAX_REGEXP_OBJECTS (30) symbols; longer patterns must
  // fail to compile, never silently match a truncated pattern
  std::string fits(29, 'a');
  EXPECT_TRUE(re_compile(fits.c_str()) != nullptr);
  std::string too_long(30, 'a');
  EXPECT_TRUE(re_compile(too_long.c_str()) == nullptr);
  EXPECT_EQ(match(too_long.c_str(), too_long.c_str()), -1);
}

TEST(TinyRegexClasses, UnterminatedClassRejected) {
  // missing ']': the end-of-pattern sentinel check rejects the pattern at
  // compile time (no memory-unsafe class match at runtime)
  EXPECT_TRUE(re_compile("[ab") == nullptr);
  EXPECT_EQ(match("[ab", "xaZb"), -1);
}

TEST(TinyRegexClasses, EdgeClasses) {
  EXPECT_TRUE(re_compile("[]") != nullptr);   // empty class compiles...
  EXPECT_EQ(match("[]", "abc"), -1);          // ...but matches nothing
  EXPECT_EQ(match("[-a]+", "--aa"), 0);       // leading '-' is a literal
  EXPECT_EQ(match("[a-]+", "a-a"), 0);        // trailing '-' is a literal
}

// ============================================================
// UNIT TESTS: packet-level rule matching (checkPacket)
// ============================================================

// Native tests for FilterRules packet-level matching (checkPacket): rule
// predicates, first-match-wins, actions, and the advert rate limiter hooks.

#include <gtest/gtest.h>

#include "FilterTestHelpers.h"

// ---------------------------------------------------------------- baseline

TEST_F(FilterTest, NoRulesAllowsEverything) {
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT);
  EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr), FILTER_ACT_ALLOW);
}

TEST_F(FilterTest, DisabledFilterAllowsEverything) {
  ASSERT_EQ(cli(filter, "off"), "OK - filter off");
  ASSERT_EQ(cli(filter, "add chanhash=AA"), "OK - rule 0 added");
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 0, 0);
  pkt.payload[0] = 0xAA;
  EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr), FILTER_ACT_ALLOW);
}

TEST_F(FilterTest, EmptyRuleMatchesEverything) {
  ASSERT_EQ(cli(filter, "add"), "OK - rule 0 added");
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_ADVERT);
  EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr), FILTER_ACT_DROP);
}

TEST_F(FilterTest, DisabledRuleIgnored) {
  expectOk(filter, "add type=advert");
  ASSERT_EQ(cli(filter, "disable 0"), "OK - rule 0 disabled");
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_ADVERT);
  EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr), FILTER_ACT_ALLOW);
}

// ---------------------------------------------------------------- actions

TEST_F(FilterTest, ForwardActionCountsButForwards) {
  expectOk(filter, "add type=advert action=forward");
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_ADVERT);
  EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr), FILTER_ACT_FORWARD);
  EXPECT_EQ(filter.getRule(0)->hits, 1u);
}

TEST_F(FilterTest, FirstMatchWins) {
  expectOk(filter, "add type=advert action=forward");
  expectOk(filter, "add type=advert action=drop");
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_ADVERT);
  EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr), FILTER_ACT_FORWARD);
  EXPECT_EQ(filter.getRule(0)->hits, 1u);
  EXPECT_EQ(filter.getRule(1)->hits, 0u);
}

TEST_F(FilterTest, HitsCounterAccumulates) {
  expectOk(filter, "add type=advert");
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_ADVERT);
  filter.checkPacket(&pkt, 0, nullptr);
  filter.checkPacket(&pkt, 0, nullptr);
  EXPECT_EQ(filter.getRule(0)->hits, 2u);
}

// ---------------------------------------------------------------- type=

TEST_F(FilterTest, TypePredicate) {
  expectOk(filter, "add type=advert,txt");
  auto adv = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_ADVERT);
  auto txt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT);
  auto dat = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_DATA);
  EXPECT_EQ(filter.checkPacket(&adv, 0, nullptr), FILTER_ACT_DROP);
  EXPECT_EQ(filter.checkPacket(&txt, 0, nullptr), FILTER_ACT_DROP);
  EXPECT_EQ(filter.checkPacket(&dat, 0, nullptr), FILTER_ACT_ALLOW);
}

// ---------------------------------------------------------------- route=

TEST_F(FilterTest, RoutePredicate) {
  expectOk(filter, "add route=direct");
  auto flood = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT);
  auto direct = makePacket(ROUTE_TYPE_DIRECT, PAYLOAD_TYPE_GRP_TXT);
  EXPECT_EQ(filter.checkPacket(&flood, 0, nullptr), FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.checkPacket(&direct, 0, nullptr), FILTER_ACT_DROP);
}

TEST_F(FilterTest, TransportFloodCountsAsFlood) {
  expectOk(filter, "add route=flood");
  auto pkt = makePacket(ROUTE_TYPE_TRANSPORT_FLOOD, PAYLOAD_TYPE_GRP_TXT);
  EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr), FILTER_ACT_DROP);
}

// The core calls allowPacketForward() for every relayed flood, plus direct
// TRACE/MULTIPART/ACK relays — so the filter sees payload types beyond
// advert/txt/data. type_mask only distinguishes advert/txt/data; every other
// type falls through to the generic predicates (hops/len/snr/...).
// Hook order in MyMesh::allowPacketForward(): battery gate -> checkPacket() ->
// disable_fwd, so with `set repeat off` packets still reach the filter and a
// DROP pre-empts the repeat check.
TEST_F(FilterTest, GenericPredicatesCoverNonAdvertTypes) {
  expectOk(filter, "add hops=[2,*]");
  auto ack = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_ACK, 10, 1, 3);
  EXPECT_EQ(filter.checkPacket(&ack, 0, nullptr), FILTER_ACT_DROP);
  auto trace = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_TRACE, 10, 1, 3);
  EXPECT_EQ(filter.checkPacket(&trace, 0, nullptr), FILTER_ACT_DROP);
  auto raw = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_RAW_CUSTOM, 10, 1, 3);
  EXPECT_EQ(filter.checkPacket(&raw, 0, nullptr), FILTER_ACT_DROP);

  // a type=advert rule does not hit non-advert payloads (0-hop so the hops
  // rule stays out of the way)
  expectOk(filter, "add type=advert");
  auto ack2 = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_ACK, 10, 1, 0);
  EXPECT_EQ(filter.checkPacket(&ack2, 0, nullptr), FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.getRule(1)->hits, 0u);
}

// ---------------------------------------------------------------- hops=

TEST_F(FilterTest, HopsExactAndRange) {
  expectOk(filter, "add hops=3");          // bare = exact
  expectOk(filter, "add hops=[2,4]");      // inclusive
  auto h1 = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 1);
  auto h2 = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 2);
  auto h3 = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 3);
  auto h4 = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 4);
  auto h5 = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 5);

  EXPECT_EQ(filter.checkPacket(&h1, 0, nullptr), FILTER_ACT_ALLOW);  // no rule
  EXPECT_EQ(filter.checkPacket(&h3, 0, nullptr), FILTER_ACT_DROP);   // rule 0: exact 3
  EXPECT_EQ(filter.checkPacket(&h2, 0, nullptr), FILTER_ACT_DROP);   // rule 1: [2,4]
  EXPECT_EQ(filter.checkPacket(&h4, 0, nullptr), FILTER_ACT_DROP);
  EXPECT_EQ(filter.checkPacket(&h5, 0, nullptr), FILTER_ACT_ALLOW);  // outside both
  EXPECT_EQ(filter.getRule(0)->hits, 1u);
  EXPECT_EQ(filter.getRule(1)->hits, 2u);
}

TEST_F(FilterTest, HopsExclusiveBounds) {
  expectOk(filter, "add hops=(2,4)");
  auto h2 = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 2);
  auto h3 = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 3);
  auto h4 = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 4);
  EXPECT_EQ(filter.checkPacket(&h2, 0, nullptr), FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.checkPacket(&h3, 0, nullptr), FILTER_ACT_DROP);
  EXPECT_EQ(filter.checkPacket(&h4, 0, nullptr), FILTER_ACT_ALLOW);
}

TEST_F(FilterTest, HopsUnboundedBelowAndAbove) {
  expectOk(filter, "add hops=[*,2]");
  expectOk(filter, "add hops=[5,*]");
  auto h1 = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 1);
  auto h3 = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 3);
  auto h6 = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 6);
  EXPECT_EQ(filter.checkPacket(&h1, 0, nullptr), FILTER_ACT_DROP);  // [*,2]
  EXPECT_EQ(filter.checkPacket(&h3, 0, nullptr), FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.checkPacket(&h6, 0, nullptr), FILTER_ACT_DROP);  // [5,*]
}

TEST_F(FilterTest, HopsIgnoredForDirectTraffic) {
  // hop count is meaningful for flood paths only
  expectOk(filter, "add hops=[2,4]");
  auto direct = makePacket(ROUTE_TYPE_DIRECT, PAYLOAD_TYPE_GRP_TXT, 10, 1, 2);
  EXPECT_EQ(filter.checkPacket(&direct, 0, nullptr), FILTER_ACT_ALLOW);
}

// ---------------------------------------------------------------- len=

TEST_F(FilterTest, LenInterval) {
  expectOk(filter, "add len=[180,*]");
  auto short_pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 20);
  auto edge = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 180);
  auto long_pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 200);
  EXPECT_EQ(filter.checkPacket(&short_pkt, 0, nullptr), FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.checkPacket(&edge, 0, nullptr), FILTER_ACT_DROP);
  EXPECT_EQ(filter.checkPacket(&long_pkt, 0, nullptr), FILTER_ACT_DROP);
}

TEST_F(FilterTest, LenBoundaries) {
  // payload_len as measured on the wire: 0..MAX_PACKET_PAYLOAD (184)
  expectOk(filter, "add len=[0,3]");
  expectOk(filter, "add len=184");
  for (uint16_t l = 0; l <= 4; l++) {
    auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, l);
    EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr),
              l <= 3 ? FILTER_ACT_DROP : FILTER_ACT_ALLOW) << "len " << l;
  }
  auto max_pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, MAX_PACKET_PAYLOAD);
  EXPECT_EQ(filter.checkPacket(&max_pkt, 0, nullptr), FILTER_ACT_DROP);
  EXPECT_EQ(filter.getRule(0)->hits, 4u);
  EXPECT_EQ(filter.getRule(1)->hits, 1u);
}

// ---------------------------------------------------------------- snr=

TEST_F(FilterTest, SnrPredicate) {
  // snr is in dB, stored in quarter-dB units: _snr=-9 -> -2.25 dB
  expectOk(filter, "add snr=-2.25");
  auto weak = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 0, -9);
  auto strong = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 0, 8);
  EXPECT_EQ(filter.checkPacket(&weak, 0, nullptr), FILTER_ACT_DROP);
  EXPECT_EQ(filter.checkPacket(&strong, 0, nullptr), FILTER_ACT_ALLOW);
}

TEST_F(FilterTest, SnrRangeWithOpenEnds) {
  expectOk(filter, "add snr=[*,-8.5]");   // quarter-dB: -34
  auto fringe = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 0, -34);
  auto edge = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 0, -33);
  EXPECT_EQ(filter.checkPacket(&fringe, 0, nullptr), FILTER_ACT_DROP);
  EXPECT_EQ(filter.checkPacket(&edge, 0, nullptr), FILTER_ACT_ALLOW);
}

// ---------------------------------------------------------------- hsize=

TEST_F(FilterTest, HashSizePredicate) {
  expectOk(filter, "add hsize=2,3");
  auto h1 = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 2);
  auto h2 = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 2, 2);
  auto h4 = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 4, 2);
  EXPECT_EQ(filter.checkPacket(&h1, 0, nullptr), FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.checkPacket(&h2, 0, nullptr), FILTER_ACT_DROP);
  EXPECT_EQ(filter.checkPacket(&h4, 0, nullptr), FILTER_ACT_ALLOW);
}

// ---------------------------------------------------------------- chanhash=

TEST_F(FilterTest, ChanHashPredicateOnGroupPayload) {
  expectOk(filter, "add chanhash=E6");
  auto txt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT);
  txt.payload[0] = 0xE6;
  auto other = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT);
  other.payload[0] = 0x11;
  auto adv = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_ADVERT);  // never matches
  adv.payload[0] = 0xE6;
  EXPECT_EQ(filter.checkPacket(&txt, 0, nullptr), FILTER_ACT_DROP);
  EXPECT_EQ(filter.checkPacket(&other, 0, nullptr), FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.checkPacket(&adv, 0, nullptr), FILTER_ACT_ALLOW);
}

// ---------------------------------------------------------------- path=

TEST_F(FilterTest, PathPrefixAnywhere) {
  // with hash size 1 the path entries are 0x10, 0x20, 0x30, ...
  expectOk(filter, "add path=20");
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 4);
  EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr), FILTER_ACT_DROP);
}

TEST_F(FilterTest, PathAnchors) {
  // rule 0 (30$): matches when the last path entry is 0x30, i.e. 3 hops
  expectOk(filter, "add path=30$");
  auto three = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 3);
  auto four = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 4);
  EXPECT_EQ(filter.checkPacket(&three, 0, nullptr), FILTER_ACT_DROP);
  EXPECT_EQ(filter.checkPacket(&four, 0, nullptr), FILTER_ACT_ALLOW);

  // rule 1 (^10): matches when the first path entry is 0x10 (any multi-hop
  // packet built by makePacket starts with 0x10)
  expectOk(filter, "add path=^10");
  auto two = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 2);
  EXPECT_EQ(filter.checkPacket(&two, 0, nullptr), FILTER_ACT_DROP);
}

TEST_F(FilterTest, PathChainWindowMatching) {
  // chain 20>30 must appear as adjacent entries somewhere in the path
  expectOk(filter, "add path=20>30");
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 4);  // 10 20 30 40
  EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr), FILTER_ACT_DROP);

  filter.getRule(0)->path.pos = FILTER_PATH_FIRST;   // 20>30 must start at entry 0
  EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr), FILTER_ACT_ALLOW);
}

TEST_F(FilterTest, PathLongerThanPathNoMatch) {
  // 5 entries exceeds FILTER_PATH_HASH_SLOTS (4): rejected at add time
  EXPECT_EQ(cli(filter, "add path=10>20>30>40>50"), "Err - bad path spec");
  EXPECT_EQ(filter.getNumRules(), 0);

  expectOk(filter, "add path=10>20");
  auto onestep = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 1);
  EXPECT_EQ(filter.checkPacket(&onestep, 0, nullptr), FILTER_ACT_ALLOW);
}

TEST_F(FilterTest, PathPrefixLengthClampedToPacketHashSize) {
  // rule entry carries 4 bytes (8 hex chars); packet hash size is 1, so only
  // the first byte is compared
  expectOk(filter, "add path=10111213");
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 2);  // entries 10 20
  EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr), FILTER_ACT_DROP);
}

TEST_F(FilterTest, PathNotMatchedOnDirectTraffic) {
  // 0-hop traffic (e.g. a direct neighbour) carries no path hash chain and
  // never matches a path predicate
  expectOk(filter, "add path=10");
  auto direct = makePacket(ROUTE_TYPE_DIRECT, PAYLOAD_TYPE_GRP_TXT, 10, 1, 0);
  EXPECT_EQ(filter.checkPacket(&direct, 0, nullptr), FILTER_ACT_ALLOW);
}

TEST_F(FilterTest, PathChainOutOfOrderNoMatch) {
  // 20>30 must appear as adjacent, in-order entries: 10 30 20 40 does not match
  expectOk(filter, "add path=20>30");
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 4);
  pkt.path[1] = 0x30;
  pkt.path[2] = 0x20;
  EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr), FILTER_ACT_ALLOW);
}

TEST_F(FilterTest, PathPrefixCompareWithHashSize2) {
  // hsz=2 packets carry 2-byte entries (10 11 / 20 21); rule entries are
  // prefix-compared with min(rule_len, packet hsz) bytes
  expectOk(filter, "add path=10");   // 1-byte prefix of entry 0
  auto hit = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 2, 2);
  EXPECT_EQ(filter.checkPacket(&hit, 0, nullptr), FILTER_ACT_DROP);

  // 2-byte compare: rule bytes 20 11 must not match entry 20 21 — a 1-byte
  // compare on the rule's first byte would falsely drop here
  expectOk(filter, "add path=2011");
  auto miss = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 2, 2);
  miss.path[0] = 0x20; miss.path[1] = 0x21;   // entries 20 21 / 30 31: no rule matches
  miss.path[2] = 0x30; miss.path[3] = 0x31;
  EXPECT_EQ(filter.checkPacket(&miss, 0, nullptr), FILTER_ACT_ALLOW);
}

TEST_F(FilterTest, PathBothAnchorsMatchWholePath) {
  // ^A$ (or ^A>B$): the window must be exactly the whole path — first entry
  // AND last entry anchored. A 1-hop path is the only match for a 1-entry rule.
  expectOk(filter, "add path=^10$");
  auto one = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 1);
  auto two = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 2);
  EXPECT_EQ(filter.checkPacket(&one, 0, nullptr), FILTER_ACT_DROP);
  EXPECT_EQ(filter.checkPacket(&two, 0, nullptr), FILTER_ACT_ALLOW);

  expectOk(filter, "add path=^10>20$");
  auto exact = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 2);
  auto longer = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10, 1, 3);
  EXPECT_EQ(filter.checkPacket(&exact, 0, nullptr), FILTER_ACT_DROP);
  EXPECT_EQ(filter.checkPacket(&longer, 0, nullptr), FILTER_ACT_ALLOW);   // 10 20 30: prefix, not whole
}

// ---------------------------------------------------------------- region=

TEST_F(FilterTest, RegionPredicate) {
  expectOk(filter, "add region=TestNorth");
  RegionEntry north{ 1, 0, 0, "TestNorth" };
  RegionEntry south{ 2, 0, 0, "TestSouth" };
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT);
  EXPECT_EQ(filter.checkPacket(&pkt, 0, &north), FILTER_ACT_DROP);
  EXPECT_EQ(filter.checkPacket(&pkt, 0, &south), FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr), FILTER_ACT_ALLOW);  // no region at all
}

TEST_F(FilterTest, RegionListAndUnscoped) {
  expectOk(filter, "add region=TestSouth,unscoped");
  RegionEntry south{ 2, 0, 0, "TestSouth" };
  RegionEntry unscoped{ 0, 0, 0, "" };   // wildcard region (id 0) = unscoped
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT);
  EXPECT_EQ(filter.checkPacket(&pkt, 0, &south), FILTER_ACT_DROP);
  EXPECT_EQ(filter.checkPacket(&pkt, 0, &unscoped), FILTER_ACT_DROP);
  RegionEntry north{ 1, 0, 0, "TestNorth" };
  EXPECT_EQ(filter.checkPacket(&pkt, 0, &north), FILTER_ACT_ALLOW);
}

TEST_F(FilterTest, RegionExactNameMatchOnly) {
  // "TestNorth" in the list must not match region "TestNorthEast"
  expectOk(filter, "add region=TestNorth");
  RegionEntry ne{ 3, 1, 0, "TestNorthEast" };
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT);
  EXPECT_EQ(filter.checkPacket(&pkt, 0, &ne), FILTER_ACT_ALLOW);
}

TEST_F(FilterTest, RegionPredicateIgnoresTransportFloodWithoutRegion) {
  // MyMesh passes recv_pkt_region = NULL for flood packets whose transport
  // codes match no configured region: no named region predicate may fire
  // (only an explicit "unscoped" list entry matches, see RegionListAndUnscoped)
  expectOk(filter, "add region=TestNorth");
  auto pkt = makePacket(ROUTE_TYPE_TRANSPORT_FLOOD, PAYLOAD_TYPE_GRP_TXT);
  EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr), FILTER_ACT_ALLOW);
}

// ---------------------------------------------------------------- deferral

TEST_F(FilterTest, ContentRulesAreDeferredNotDroppedAtPacketLevel) {
  expectOk(filter, "add chan=#defer");
  expectOk(filter, "add sender=^Bob");
  expectOk(filter, "add text=spam");
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 20, 1, 3);
  EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr), FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.getRule(0)->hits, 0u);
  EXPECT_EQ(filter.getRule(1)->hits, 0u);
  EXPECT_EQ(filter.getRule(2)->hits, 0u);
}

// ============================================================
// UNIT TESTS: content rules (checkContent) + channel search
// ============================================================

// Native tests for FilterRules content matching (checkContent): keyed
// channel/sender/text predicates on the decrypted group payload.

#include <gtest/gtest.h>

#include "FilterTestHelpers.h"

// ---------------------------------------------------------------- helpers

// add a rule via the CLI, then hand back the channel created for `name`
static FilterChannel* addChanRule(FilterRules& filter, const char* names) {
  std::string cmd = std::string("add chan=") + names;
  std::string reply = cli(filter, cmd.c_str());
  EXPECT_EQ(reply.substr(0, 3), "OK ") << reply;
  return filter.findChannel("#defer");
}

// ---------------------------------------------------------------- parseGroupText (behavioural)

TEST_F(FilterTest, SenderAndTextExtractedFromPayload) {
  ASSERT_EQ(cli(filter, "add sender=^Alice$"), "OK - rule 0 added");
  auto payload = makeGroupText("Alice", "hello world");
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, payload.len);
  auto chan = channelFromStore(filter, 0);   // Public (provisioned by begin())
  EXPECT_EQ(filter.checkContent(&pkt, PAYLOAD_TYPE_GRP_TXT, chan, payload.data,
                                payload.len, nullptr),
            FILTER_ACT_DROP);
}

TEST_F(FilterTest, SenderRegexIsUnanchoredByDefault) {
  expectOk(filter, "add sender=Spam");   // substring, case-sensitive
  auto payload = makeGroupText("theSpamBot", "hi");
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, payload.len);
  auto chan = channelFromStore(filter, 0);
  EXPECT_EQ(filter.checkContent(&pkt, PAYLOAD_TYPE_GRP_TXT, chan, payload.data,
                                payload.len, nullptr),
            FILTER_ACT_DROP);
}

TEST_F(FilterTest, NoColonMeansAllText) {
  expectOk(filter, "add text=^BEACON");
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 20);
  auto chan = channelFromStore(filter, 0);
  uint8_t raw[] = { 0, 0, 0, 0, TXT_TYPE_PLAIN, 'B', 'E', 'A', 'C', 'O', 'N', '!' };
  EXPECT_EQ(filter.checkContent(&pkt, PAYLOAD_TYPE_GRP_TXT, chan, raw, sizeof(raw), nullptr),
            FILTER_ACT_DROP);
}

TEST_F(FilterTest, ShortPayloadParsesToEmpty) {
  // len < 5: no sender, no text -> sender/text rules can never match
  expectOk(filter, "add sender=.*");
  expectOk(filter, "add text=.*");
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 20);
  auto chan = channelFromStore(filter, 0);
  uint8_t raw[] = { 1, 2, 3 };
  EXPECT_EQ(filter.checkContent(&pkt, PAYLOAD_TYPE_GRP_TXT, chan, raw, sizeof(raw), nullptr),
            FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.getRule(0)->hits, 0u);
  EXPECT_EQ(filter.getRule(1)->hits, 0u);
}

// ---------------------------------------------------------------- keyed channels

TEST_F(FilterTest, ChanPredicateMatchesDeliveredChannelSecret) {
  addChanRule(filter, "#defer");
  int pub_idx = 0, defer_idx = -1;
  for (int i = 0; i < filter.getNumChannels(); i++) {
    if (strcmp(filter.getChannel(i)->name, "#defer") == 0) defer_idx = i;
  }
  ASSERT_GE(defer_idx, 0);

  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 20);
  auto matched = channelFromStore(filter, defer_idx);
  auto other = channelFromStore(filter, pub_idx);   // Public

  EXPECT_EQ(filter.checkContent(&pkt, PAYLOAD_TYPE_GRP_TXT, matched, nullptr, 0, nullptr),
            FILTER_ACT_DROP);
  EXPECT_EQ(filter.checkContent(&pkt, PAYLOAD_TYPE_GRP_TXT, other, nullptr, 0, nullptr),
            FILTER_ACT_ALLOW);
}

TEST_F(FilterTest, ChanPredicateMultiChannel) {
  addChanRule(filter, "#a,#b");
  int a_idx = -1, b_idx = -1;
  for (int i = 0; i < filter.getNumChannels(); i++) {
    if (strcmp(filter.getChannel(i)->name, "#a") == 0) a_idx = i;
    if (strcmp(filter.getChannel(i)->name, "#b") == 0) b_idx = i;
  }
  ASSERT_GE(a_idx, 0);
  ASSERT_GE(b_idx, 0);
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 20);
  EXPECT_EQ(filter.checkContent(&pkt, PAYLOAD_TYPE_GRP_TXT, channelFromStore(filter, b_idx),
                                nullptr, 0, nullptr),
            FILTER_ACT_DROP);
  EXPECT_EQ(filter.checkContent(&pkt, PAYLOAD_TYPE_GRP_TXT, channelFromStore(filter, a_idx),
                                nullptr, 0, nullptr),
            FILTER_ACT_DROP);
}

TEST_F(FilterTest, ContentRuleAlsoRequiresPacketPredicates) {
  // a content rule whose packet-level predicates don't match is skipped
  expectOk(filter, "add chan=#defer hops=[5,9]");
  int defer_idx = -1;
  for (int i = 0; i < filter.getNumChannels(); i++) {
    if (strcmp(filter.getChannel(i)->name, "#defer") == 0) defer_idx = i;
  }
  ASSERT_GE(defer_idx, 0);
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 20, 1, 2);   // only 2 hops
  EXPECT_EQ(filter.checkContent(&pkt, PAYLOAD_TYPE_GRP_TXT, channelFromStore(filter, defer_idx),
                                nullptr, 0, nullptr),
            FILTER_ACT_ALLOW);
}

// ---------------------------------------------------------------- data payloads

TEST_F(FilterTest, GrpDataSkipsSenderTextButHonoursChan) {
  expectOk(filter, "add chan=#defer");
  expectOk(filter, "add sender=^Alice$");   // not applicable to data
  int defer_idx = -1;
  for (int i = 0; i < filter.getNumChannels(); i++) {
    if (strcmp(filter.getChannel(i)->name, "#defer") == 0) defer_idx = i;
  }
  ASSERT_GE(defer_idx, 0);
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_DATA, 20);
  uint8_t raw[] = { 0, 0, 0, 0, TXT_TYPE_PLAIN, 'x' };   // not even parsed for data

  // chan rule applies to data
  EXPECT_EQ(filter.checkContent(&pkt, PAYLOAD_TYPE_GRP_DATA,
                                channelFromStore(filter, defer_idx), raw, sizeof(raw), nullptr),
            FILTER_ACT_DROP);
  // sender rule cannot match (no parseable sender on data)
  EXPECT_EQ(filter.checkContent(&pkt, PAYLOAD_TYPE_GRP_DATA, channelFromStore(filter, 0),
                                raw, sizeof(raw), nullptr),
            FILTER_ACT_ALLOW);
}

TEST_F(FilterTest, NonGroupPayloadTypesBypassContentRules) {
  expectOk(filter, "add sender=^Alice$");
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_TXT_MSG, 20);
  auto chan = channelFromStore(filter, 0);
  uint8_t raw[] = { 0 };
  EXPECT_EQ(filter.checkContent(&pkt, PAYLOAD_TYPE_TXT_MSG, chan, raw, sizeof(raw), nullptr),
            FILTER_ACT_ALLOW);
}

TEST_F(FilterTest, ContentFirstMatchWinsAndForwardCounts) {
  expectOk(filter, "add sender=^Alice action=forward");
  expectOk(filter, "add sender=^Alice action=drop");
  auto payload = makeGroupText("Alice", "hi");
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, payload.len);
  auto chan = channelFromStore(filter, 0);
  EXPECT_EQ(filter.checkContent(&pkt, PAYLOAD_TYPE_GRP_TXT, chan, payload.data,
                                payload.len, nullptr),
            FILTER_ACT_FORWARD);
  EXPECT_EQ(filter.getRule(0)->hits, 1u);
  EXPECT_EQ(filter.getRule(1)->hits, 0u);
}

TEST_F(FilterTest, CombinedChanAndSenderMustBothMatch) {
  expectOk(filter, "add chan=#defer sender=^Alice$");
  int defer_idx = -1;
  for (int i = 0; i < filter.getNumChannels(); i++) {
    if (strcmp(filter.getChannel(i)->name, "#defer") == 0) defer_idx = i;
  }
  ASSERT_GE(defer_idx, 0);
  auto chan = channelFromStore(filter, defer_idx);

  auto from_alice = makeGroupText("Alice", "hi");
  auto pkt1 = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, from_alice.len);
  EXPECT_EQ(filter.checkContent(&pkt1, PAYLOAD_TYPE_GRP_TXT, chan, from_alice.data,
                                from_alice.len, nullptr),
            FILTER_ACT_DROP);

  auto from_bob = makeGroupText("Bob", "hi");
  auto pkt2 = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, from_bob.len);
  EXPECT_EQ(filter.checkContent(&pkt2, PAYLOAD_TYPE_GRP_TXT, chan, from_bob.data,
                                from_bob.len, nullptr),
            FILTER_ACT_ALLOW);
}

TEST_F(FilterTest, RegexBudgetAbortFailsOpenAndCounts) {
  expectOk(filter, "add text=a*a*a*a*a*a*b");
  re_set_step_budget(10);
  auto payload = makeGroupText("S", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, payload.len);
  auto chan = channelFromStore(filter, 0);
  EXPECT_EQ(filter.checkContent(&pkt, PAYLOAD_TYPE_GRP_TXT, chan, payload.data,
                                payload.len, nullptr),
            FILTER_ACT_ALLOW);   // fail-open: forwarded, not dropped
  EXPECT_EQ(filter.getBudgetAborts(), 1u);
  re_set_step_budget(5000);   // restore the compiled-in default
}

// ---------------------------------------------------------------- searchChannelsByHash

TEST_F(FilterTest, SearchChannelsByHash) {
  expectOk(filter, "chan add #zpx");
  auto ch = filter.findChannel("#zpx");
  ASSERT_NE(ch, nullptr);
  mesh::GroupChannel dest[4];

  uint8_t hit[1] = { ch->hash };
  EXPECT_EQ(filter.searchChannelsByHash(hit, dest, 4), 1);
  EXPECT_EQ(dest[0].hash[0], ch->hash);
  EXPECT_EQ(memcmp(dest[0].secret, ch->secret, sizeof(dest[0].secret)), 0);

  uint8_t miss[1] = { (uint8_t)(ch->hash ^ 0xFF) };
  EXPECT_EQ(filter.searchChannelsByHash(miss, dest, 4), 0);
}

TEST_F(FilterTest, SearchChannelsByHashDisabledWhenFilterOff) {
  expectOk(filter, "chan add #zpx");
  ASSERT_EQ(cli(filter, "off"), "OK - filter off");
  auto ch = filter.findChannel("#zpx");
  ASSERT_NE(ch, nullptr);
  mesh::GroupChannel dest[4];
  uint8_t hit[1] = { ch->hash };
  EXPECT_EQ(filter.searchChannelsByHash(hit, dest, 4), 0);
}

// ============================================================
// UNIT TESTS: per-origin advert rate limiter
// ============================================================

// Native tests for the per-origin advert rate limiter (RAM-only cache,
// ring eviction, window refresh, wrap-safe timing).

#include <gtest/gtest.h>

#include "FilterTestHelpers.h"

TEST_F(FilterTest, FirstAdvertRecordedRepeatDropped) {
  filter.setAdvertRatelimit(48);
  uint8_t key[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
  auto pkt = makeAdvert(key);

  EXPECT_EQ(filter.checkPacket(&pkt, 1000, nullptr), FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.getAdvertCacheCount(), 1);
  EXPECT_EQ(filter.getLimiterDrops(), 0u);

  // repeat flood advert within the 48h window (transport codes set to make a
  // distinct packet, same origin key)
  pkt.transport_codes[0] = 0x4242;
  EXPECT_EQ(filter.checkPacket(&pkt, 5000, nullptr), FILTER_ACT_DROP);
  EXPECT_EQ(filter.getLimiterDrops(), 1u);
  EXPECT_EQ(filter.getAdvertCacheCount(), 1);   // repeat not re-recorded
}

TEST_F(FilterTest, DistinctOriginsTrackedIndependently) {
  filter.setAdvertRatelimit(48);
  uint8_t k1[4] = { 1, 2, 3, 4 };
  uint8_t k2[4] = { 5, 6, 7, 8 };
  auto a1 = makeAdvert(k1);
  auto a2 = makeAdvert(k2)  ;
  EXPECT_EQ(filter.checkPacket(&a1, 1000, nullptr), FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.checkPacket(&a2, 1100, nullptr), FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.getAdvertCacheCount(), 2);

  auto a1b = makeAdvert(k1);
  EXPECT_EQ(filter.checkPacket(&a1b, 2000, nullptr), FILTER_ACT_DROP);
}

TEST_F(FilterTest, WindowExpiryAllowsAndRefreshes) {
  filter.setAdvertRatelimit(1);   // 1h window
  uint8_t key[4] = { 9, 9, 9, 9 };
  auto pkt = makeAdvert(key);

  uint32_t t0 = 1000;
  EXPECT_EQ(filter.checkPacket(&pkt, t0, nullptr), FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.checkPacket(&pkt, t0 + 3600UL * 1000 - 1, nullptr), FILTER_ACT_DROP);
  // at exactly the window boundary the advert is allowed again...
  EXPECT_EQ(filter.checkPacket(&pkt, t0 + 3600UL * 1000, nullptr), FILTER_ACT_ALLOW);
  // ...and the window restarts from that moment
  EXPECT_EQ(filter.checkPacket(&pkt, t0 + 3600UL * 1000 + 500, nullptr), FILTER_ACT_DROP);
}

TEST_F(FilterTest, TimingIsWrapSafe) {
  filter.setAdvertRatelimit(1);
  uint8_t key[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
  auto pkt = makeAdvert(key);

  uint32_t t0 = 0xFFFFFFF0u;   // just before the 32-bit millis() wrap
  EXPECT_EQ(filter.checkPacket(&pkt, t0, nullptr), FILTER_ACT_ALLOW);
  // now has wrapped past 0 to ~16s: still inside the 1h window
  EXPECT_EQ(filter.checkPacket(&pkt, 16000, nullptr), FILTER_ACT_DROP);
  // wrapped and past the window: allowed, entry refreshed
  EXPECT_EQ(filter.checkPacket(&pkt, 3600UL * 1000 + 16000, nullptr), FILTER_ACT_ALLOW);
}

TEST_F(FilterTest, RingCacheEvictsOldestOrigin) {
  filter.setAdvertRatelimit(48);
  uint8_t key[4] = { 0, 0x55, 0, 0 };
  auto pkt = makeAdvert(key);

  // fill the whole cache with distinct origins
  for (int i = 0; i < FILTER_ADVERT_CACHE_SIZE; i++) {
    pkt.payload[ADV_KEY_OFFSETS[0]] = (uint8_t)i;
    pkt.payload[ADV_KEY_OFFSETS[1]] = 0x55;
    pkt.payload[ADV_KEY_OFFSETS[2]] = (uint8_t)(i >> 8);
    pkt.payload[ADV_KEY_OFFSETS[3]] = (uint8_t)(i >> 16);
    EXPECT_EQ(filter.checkPacket(&pkt, 1000 + i, nullptr), FILTER_ACT_ALLOW);
  }
  EXPECT_EQ(filter.getAdvertCacheCount(), FILTER_ADVERT_CACHE_SIZE);

  // origin 0 is the oldest: one more advert evicts it
  uint8_t extra[4] = { 0xAA, 0xBB, 0xCC, 0xDD };
  auto extra_pkt = makeAdvert(extra);
  EXPECT_EQ(filter.checkPacket(&extra_pkt, 5000, nullptr), FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.getAdvertCacheCount(), FILTER_ADVERT_CACHE_SIZE);

  // origin 0 has been evicted, so it is recorded fresh instead of dropped
  pkt.payload[ADV_KEY_OFFSETS[0]] = 0;
  pkt.payload[ADV_KEY_OFFSETS[1]] = 0x55;
  pkt.payload[ADV_KEY_OFFSETS[2]] = 0;
  pkt.payload[ADV_KEY_OFFSETS[3]] = 0;
  EXPECT_EQ(filter.checkPacket(&pkt, 5100, nullptr), FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.getLimiterDrops(), 0u);
}

TEST_F(FilterTest, ClearEmptiesCacheButNotCounters) {
  filter.setAdvertRatelimit(48);
  uint8_t key[4] = { 7, 7, 7, 7 };
  auto pkt = makeAdvert(key);
  filter.checkPacket(&pkt, 1000, nullptr);
  pkt.transport_codes[0] = 1;
  filter.checkPacket(&pkt, 2000, nullptr);
  ASSERT_GT(filter.getLimiterDrops(), 0u);

  filter.clearAdvertCache();
  EXPECT_EQ(filter.getAdvertCacheCount(), 0);
  EXPECT_GT(filter.getLimiterDrops(), 0u);   // counters are sticky until reset

  // cleared cache: the origin is recorded again instead of dropped
  auto pkt2 = makeAdvert(key);
  EXPECT_EQ(filter.checkPacket(&pkt2, 3000, nullptr), FILTER_ACT_ALLOW);
}

TEST_F(FilterTest, RatelimitClearCliClearsCacheNotCounters) {
  // `ratelimit clear` clears the RAM origin cache (reboot does too) but never
  // the sticky drop/abort counters (only `filter stats` reset does)
  filter.setAdvertRatelimit(48);
  uint8_t key[4] = { 7, 7, 7, 7 };
  auto pkt = makeAdvert(key);
  filter.checkPacket(&pkt, 1000, nullptr);
  pkt.transport_codes[0] = 1;
  filter.checkPacket(&pkt, 2000, nullptr);
  ASSERT_EQ(filter.getLimiterDrops(), 1u);

  ASSERT_EQ(cli(filter, "ratelimit clear"), "OK - advert cache cleared");
  EXPECT_EQ(filter.getAdvertCacheCount(), 0);
  EXPECT_EQ(filter.getLimiterDrops(), 1u);   // sticky
}

TEST_F(FilterTest, RatelimitZeroIsOff) {
  uint8_t key[4] = { 3, 1, 4, 1 };
  auto pkt = makeAdvert(key);
  EXPECT_EQ(filter.checkPacket(&pkt, 1000, nullptr), FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.getAdvertCacheCount(), 0);   // nothing recorded while off
  pkt.transport_codes[0] = 2;
  EXPECT_EQ(filter.checkPacket(&pkt, 2000, nullptr), FILTER_ACT_ALLOW);
}

TEST_F(FilterTest, OnlyFloodAdvertsAreLimited) {
  filter.setAdvertRatelimit(48);
  uint8_t key[4] = { 0x11, 0x22, 0x33, 0x44 };
  auto direct_adv = makePacket(ROUTE_TYPE_DIRECT, PAYLOAD_TYPE_ADVERT, 64, 1, 0);
  for (int i = 0; i < 4; i++) direct_adv.payload[ADV_KEY_OFFSETS[i]] = key[i];
  EXPECT_EQ(filter.checkPacket(&direct_adv, 1000, nullptr), FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.getAdvertCacheCount(), 0);   // direct adverts not recorded

  auto flood_txt = makeAdvert(key);
  flood_txt.header = (ROUTE_TYPE_FLOOD & PH_ROUTE_MASK) | (PAYLOAD_TYPE_GRP_TXT << PH_TYPE_SHIFT);
  EXPECT_EQ(filter.checkPacket(&flood_txt, 1100, nullptr), FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.getAdvertCacheCount(), 0);   // non-advert types not recorded
}

TEST_F(FilterTest, ForwardAdvertStillHitsLimiter) {
  // the limiter runs even when a forward rule matched (only a DROP pre-empts)
  expectOk(filter, "add type=advert action=forward");
  filter.setAdvertRatelimit(48);
  uint8_t key[4] = { 0x0A, 0x0B, 0x0C, 0x0D };
  auto pkt = makeAdvert(key);
  EXPECT_EQ(filter.checkPacket(&pkt, 1000, nullptr), FILTER_ACT_FORWARD);
  pkt.transport_codes[0] = 5;
  EXPECT_EQ(filter.checkPacket(&pkt, 2000, nullptr), FILTER_ACT_DROP);   // limiter wins
  EXPECT_EQ(filter.getLimiterDrops(), 1u);
}

// ============================================================
// UNIT TESTS: rule/channel management + persistence
// ============================================================

// Native tests for FilterRules rule/channel management and persistence
// (binary /filter_cfg format, version upgrades, lazy save).

#include <gtest/gtest.h>

#include "FilterTestHelpers.h"

// /filter_cfg layout constants (mirrors PacketFilter.cpp: the v4 record is
// the struct up to `hits`; the v3 record is the same struct with no `regions`
// field, padded to uint32_t alignment; the lazy-save delay is 3000 ms)
static constexpr size_t V4_RULE_BYTES = offsetof(FilterRule, hits);
static constexpr size_t V3_RULE_BYTES =
    (offsetof(FilterRule, regions) + alignof(uint32_t) - 1) & ~(alignof(uint32_t) - 1);
static constexpr uint8_t CFG_VERSION = 4;
static constexpr unsigned long CFG_SAVE_DELAY_MS = 3000;
static const char* CFG_FILE = "/filter_cfg";

// ---------------------------------------------------------------- rule management

TEST_F(FilterTest, AddRuleDefaultsAndCapacity) {
  for (int i = 0; i < FILTER_MAX_RULES; i++) {
    FilterRule* r = filter.addRule();
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(r->enabled);
    EXPECT_EQ(r->action, FILTER_ACT_DROP);   // default action
  }
  EXPECT_EQ(filter.getNumRules(), FILTER_MAX_RULES);
  EXPECT_EQ(filter.addRule(), nullptr);   // store full
}

TEST_F(FilterTest, DelRuleShiftsLaterRules) {
  FilterRule* a = filter.addRule();
  FilterRule* b = filter.addRule();
  a->hops.lo = 11; a->hops.flags = FILTER_IV_LO_INC | FILTER_IV_HI_INC; a->hops.hi = 11;
  b->hops.lo = 22; b->hops.flags = FILTER_IV_LO_INC | FILTER_IV_HI_INC; b->hops.hi = 22;

  filter.delRule(0);
  ASSERT_EQ(filter.getNumRules(), 1);
  EXPECT_EQ(filter.getRule(0)->hops.lo, 22);   // b shifted into slot 0
}

TEST_F(FilterTest, ClearRulesKeepsChannels) {
  ASSERT_EQ(cli(filter, "chan add #keepme").substr(0, 3), "OK ");
  filter.addRule();
  filter.clearRules();
  EXPECT_EQ(filter.getNumRules(), 0);
  EXPECT_NE(filter.findChannel("#keepme"), nullptr);   // channels survive a clear
}

// ---------------------------------------------------------------- persistence roundtrip

TEST_F(FilterTest, SaveLoadRoundtripPreservesConfig) {
  filter.setAdvertRatelimit(5);
  ASSERT_EQ(cli(filter, "chan add #chan32 aabbccddeeff00112233445566778899aabbccddeeff00112233445566778899").substr(0, 3), "OK ");
  ASSERT_EQ(cli(filter, "add chan=#chan32 hops=[2,*] type=txt,data region=TestNorth,unscoped action=forward sender=^X text=^y"),
            "OK - rule 0 added");
  filter.save(&fs);

  FilterRules restored;
  restored.begin(&fs);   // existing config: no fresh provisioning

  EXPECT_EQ(restored.getAdvertRatelimit(), 5);
  EXPECT_TRUE(restored.isEnabled());
}

TEST_F(FilterTest, SaveLoadRoundtripPreservesPathAndIntervals) {
  ASSERT_EQ(cli(filter, "add path=^A1B2>CC$ hsize=1,2 len=[10,20] chanhash=AA snr=[-3,0.5]"),
            "OK - rule 0 added");
  filter.save(&fs);

  FilterRules restored;
  restored.begin(&fs);
  ASSERT_EQ(restored.getNumRules(), 1);
  FilterRule* r = restored.getRule(0);
  EXPECT_EQ(r->path.count, 2);
  EXPECT_EQ(r->path.pos, FILTER_PATH_FIRST | FILTER_PATH_LAST);
  EXPECT_EQ(r->path.len[0], 2);
  EXPECT_EQ(r->path.bytes[0][0], 0xA1);
  EXPECT_EQ(r->path.bytes[0][1], 0xB2);
  EXPECT_EQ(r->path.len[1], 1);
  EXPECT_EQ(r->path.bytes[1][0], 0xCC);
  EXPECT_EQ(r->hash_size_mask, 0x03);
  EXPECT_EQ(r->len.lo, 10);
  EXPECT_EQ(r->len.hi, 20);
  EXPECT_EQ(r->chan_flags, FILTER_CHANFLG_HASH_SET);
  EXPECT_EQ(r->chan_hash, 0xAA);
  EXPECT_EQ(r->snr.lo, (uint16_t)(int16_t)-12);
  EXPECT_EQ(r->snr.hi, 2);

  // behaviourally: the restored rule still matches the same packet shape
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 15, 2, 2);
  pkt.path[0] = 0xA1; pkt.path[1] = 0xB2;   // ^A1B2 whole-path anchor
  pkt.path[2] = 0xCC;                       // >CC$ last entry (1-byte prefix vs hsz=2)
  pkt.payload[0] = 0xAA;
  EXPECT_EQ(restored.checkPacket(&pkt, 0, nullptr), FILTER_ACT_DROP);
}

TEST_F(FilterTest, StatsAreNeverPersisted) {
  filter.addRule();
  filter.getRule(0)->hits = 777;
  filter.save(&fs);

  FilterRules restored;
  restored.begin(&fs);
  ASSERT_EQ(restored.getNumRules(), 1);
  EXPECT_EQ(restored.getRule(0)->hits, 0u);   // hits live in RAM only
}

TEST_F(FilterTest, DisabledStatePersists) {
  filter.setEnabled(false);
  filter.save(&fs);
  FilterRules restored;
  restored.begin(&fs);
  EXPECT_FALSE(restored.isEnabled());
}

TEST_F(FilterTest, BeginProvisionsPublicChannelOnFreshNode) {
  NativeFS fresh;
  FilterRules f;
  f.begin(&fresh);
  ASSERT_EQ(f.getNumChannels(), 1);
  FilterChannel* pub = f.getChannel(0);
  EXPECT_STREQ(pub->name, "Public");
  EXPECT_EQ(pub->secret_len, 16);
  // hash is sha256(secret)[0] (native build uses the deterministic SHA256 mock)
  uint8_t expect_hash;
  mesh::Utils::sha256(&expect_hash, sizeof(expect_hash), pub->secret, pub->secret_len);
  EXPECT_EQ(pub->hash, expect_hash);

  // second begin() on the same store must not duplicate
  f.begin(&fresh);
  EXPECT_EQ(f.getNumChannels(), 1);
}

// ---------------------------------------------------------------- version upgrades

TEST_F(FilterTest, V3ConfigUpgradesToV4) {
  // build a v3 store: one rule with hops=[2,4], one channel (Public)
  filter.setAdvertRatelimit(7);
  ASSERT_EQ(cli(filter, "add hops=[2,4]"), "OK - rule 0 added");
  filter.save(&fs);

  // transmute the v4 blob into a v3 blob: version byte 3, rule records stop
  // before `regions`
  auto& blob = fs.files[CFG_FILE];
  ASSERT_GE(blob.size(), (size_t)(7 + V4_RULE_BYTES + sizeof(FilterChannel)));
  std::vector<uint8_t> v3(7 + V3_RULE_BYTES + sizeof(FilterChannel));
  memcpy(&v3[0], blob.data(), 5);
  v3[0] = 3;                                        // version
  memcpy(&v3[5], blob.data() + 5, 2);               // ratelimit
  memcpy(&v3[7], blob.data() + 7, V3_RULE_BYTES);   // rule record, pre-regions
  memcpy(&v3[7 + V3_RULE_BYTES], blob.data() + 7 + V4_RULE_BYTES, sizeof(FilterChannel));
  blob = v3;

  FilterRules upgraded;
  upgraded.begin(&fs);
  ASSERT_EQ(upgraded.getNumRules(), 1);
  FilterRule* r = upgraded.getRule(0);
  EXPECT_EQ(r->hops.flags, FILTER_IV_LO_INC | FILTER_IV_HI_INC);
  EXPECT_EQ(r->hops.lo, 2);
  EXPECT_EQ(r->hops.hi, 4);
  EXPECT_EQ(r->regions[0], 0);              // v4-only field reset, not garbage
  ASSERT_EQ(upgraded.getNumChannels(), 1);
  EXPECT_STREQ(upgraded.getChannel(0)->name, "Public");
  EXPECT_EQ(upgraded.getAdvertRatelimit(), 7);
}

TEST_F(FilterTest, UnknownConfigVersionDiscarded) {
  filter.addRule();
  filter.setEnabled(false);
  filter.save(&fs);
  fs.files[CFG_FILE][0] = 9;   // bogus version

  FilterRules restored;
  restored.begin(&fs);
  EXPECT_EQ(restored.getNumRules(), 0);
  EXPECT_TRUE(restored.isEnabled());   // defaults
}

TEST_F(FilterTest, TruncatedConfigPartiallyIgnored) {
  filter.setAdvertRatelimit(3);
  filter.addRule();
  filter.addRule();
  filter.save(&fs);

  // header promises 2 rules but the file stops after the first
  auto& blob = fs.files[CFG_FILE];
  blob[2] = 2;   // num_rules = 2
  blob.resize(7 + V4_RULE_BYTES);   // cut after rule 0

  FilterRules restored;
  restored.begin(&fs);
  EXPECT_EQ(restored.getNumRules(), 0);   // nothing half-loaded
}

TEST_F(FilterTest, TruncatedConfigAllOrNothingAtEveryLength) {
  // fuzz-lite: cut the saved blob at every byte offset; the loader must never
  // half-load (rules/channels appear only when the whole file reads cleanly)
  filter.setAdvertRatelimit(5);
  ASSERT_EQ(cli(filter, "add type=advert hops=[1,2] path=10>20"), "OK - rule 0 added");
  ASSERT_EQ(cli(filter, "chan add #x aabbccddeeff00112233445566778899").substr(0, 3), "OK ");
  filter.save(&fs);
  auto full = fs.files[CFG_FILE];
  ASSERT_GT(full.size(), (size_t)8);

  for (size_t len = 0; len < full.size(); len++) {
    NativeFS cut;
    cut.files[CFG_FILE] = std::vector<uint8_t>(full.begin(), full.begin() + len);
    FilterRules restored;
    restored.begin(&cut);   // must not crash or half-load
    EXPECT_EQ(restored.getNumRules(), 0) << "truncated at " << len;
    EXPECT_EQ(restored.getNumChannels(), 0) << "truncated at " << len;
  }

  // the full blob still loads intact
  FilterRules restored;
  restored.begin(&fs);
  EXPECT_EQ(restored.getNumRules(), 1);
  EXPECT_EQ(restored.getNumChannels(), 2);   // Public + #x
  EXPECT_EQ(restored.getAdvertRatelimit(), 5);
}

// ---------------------------------------------------------------- lazy save

TEST_F(FilterTest, LazySaveAfterDelay) {
  filter.addRule();          // marks dirty at t=0
  filter.loop(&fs);          // t=0: delay not elapsed
  EXPECT_FALSE(fs.exists(CFG_FILE));

  g_mock_millis = CFG_SAVE_DELAY_MS;
  filter.loop(&fs);
  EXPECT_TRUE(fs.exists(CFG_FILE));
}

TEST_F(FilterTest, LoopSavesOnceWhenClean) {
  filter.addRule();
  g_mock_millis = CFG_SAVE_DELAY_MS;
  filter.loop(&fs);
  auto saved = fs.files[CFG_FILE];

  // mutate the saved copy so a second write would be detectable
  filter.getRule(0)->hits = 99;
  g_mock_millis = CFG_SAVE_DELAY_MS * 2;
  filter.loop(&fs);          // not dirty: no save
  EXPECT_EQ(fs.files[CFG_FILE], saved);
}

// ============================================================
// UNIT TESTS: filter CLI surface
// ============================================================

// Native tests for the `filter` CLI surface: add parsing, channel store
// commands, rule bookkeeping, ratelimit commands, status/stats output.

#include <gtest/gtest.h>

#include "FilterTestHelpers.h"

// ---------------------------------------------------------------- status / on / off

TEST_F(FilterTest, StatusLineFormat) {
  EXPECT_EQ(cli(filter, ""), "on; rules 0/16; chans 1/16; ratelimit advert 0h; cache 0/256; limiter 0; aborted 0");
}

TEST_F(FilterTest, OnOffTogglesAndKeepsConfig) {
  ASSERT_EQ(cli(filter, "add type=advert"), "OK - rule 0 added");
  EXPECT_EQ(cli(filter, "off"), "OK - filter off");
  EXPECT_FALSE(filter.isEnabled());
  EXPECT_EQ(cli(filter, "on"), "OK - filter on");
  EXPECT_TRUE(filter.isEnabled());
  EXPECT_EQ(filter.getNumRules(), 1);   // config kept
}

TEST_F(FilterTest, UnknownSubcommandPrintsUsage) {
  EXPECT_EQ(cli(filter, "bogus"),
            "Err - usage: on|off|add|list|get|enable|disable|move|del|clear|chan|ratelimit|stats");
}

// ---------------------------------------------------------------- filter add: valid parsing

TEST_F(FilterTest, AddTypePredicate) {
  ASSERT_EQ(cli(filter, "add type=advert,txt,data"), "OK - rule 0 added");
  EXPECT_EQ(filter.getRule(0)->type_mask,
            FILTER_TYPE_ADVERT | FILTER_TYPE_GRP_TXT | FILTER_TYPE_GRP_DATA);
}

TEST_F(FilterTest, AddRoutePredicate) {
  expectOk(filter, "add route=flood");
  EXPECT_EQ(filter.getRule(0)->route_mask, FILTER_ROUTE_FLOOD);
  expectOk(filter, "add route=direct");
  EXPECT_EQ(filter.getRule(1)->route_mask, FILTER_ROUTE_DIRECT);
}

TEST_F(FilterTest, AddIntervalPredicates) {
  expectOk(filter, "add hops=[2,*] len=(10,100] snr=[-3.25,0.5]");
  FilterRule* r = filter.getRule(0);
  // '[' + ']' set both INC bits; HI_INC is redundant beside HI_ANY but harmless
  EXPECT_EQ(r->hops.flags, FILTER_IV_LO_INC | FILTER_IV_HI_INC | FILTER_IV_HI_ANY);
  EXPECT_EQ(r->hops.lo, 2);
  EXPECT_EQ(r->len.flags, FILTER_IV_HI_INC);   // lo exclusive, hi inclusive
  EXPECT_EQ(r->len.lo, 10);
  EXPECT_EQ(r->len.hi, 100);
  // snr in quarter-dB: -3.25 dB -> -13, 0.5 dB -> 2
  EXPECT_EQ(r->snr.flags, FILTER_IV_LO_INC | FILTER_IV_HI_INC);
  EXPECT_EQ(r->snr.lo, (uint16_t)(int16_t)-13);
  EXPECT_EQ(r->snr.hi, 2);
}

TEST_F(FilterTest, AddBareValueIsExactInterval) {
  expectOk(filter, "add hops=3");
  FilterRule* r = filter.getRule(0);
  EXPECT_EQ(r->hops.flags, FILTER_IV_LO_INC | FILTER_IV_HI_INC);
  EXPECT_EQ(r->hops.lo, 3);
  EXPECT_EQ(r->hops.hi, 3);
}

TEST_F(FilterTest, AddAsteriskAsteriskLeavesPredicateUnset) {
  expectOk(filter, "add hops=(*,*)");
  EXPECT_EQ(filter.getRule(0)->hops.flags, 0);   // wildcard = no predicate
}

TEST_F(FilterTest, AddPathPredicate) {
  expectOk(filter, "add path=^A1B2>CC$");
  FilterRule* r = filter.getRule(0);
  EXPECT_EQ(r->path.count, 2);
  EXPECT_EQ(r->path.pos, FILTER_PATH_FIRST | FILTER_PATH_LAST);   // both anchors
  EXPECT_EQ(r->path.len[0], 2);
  EXPECT_EQ(r->path.bytes[0][0], 0xA1);
  EXPECT_EQ(r->path.bytes[0][1], 0xB2);
  EXPECT_EQ(r->path.len[1], 1);
  EXPECT_EQ(r->path.bytes[1][0], 0xCC);

  // single anchors stay as before
  expectOk(filter, "add path=BB$");
  EXPECT_EQ(filter.getRule(1)->path.pos, FILTER_PATH_LAST);
  expectOk(filter, "add path=^CC");
  EXPECT_EQ(filter.getRule(2)->path.pos, FILTER_PATH_FIRST);
}

TEST_F(FilterTest, AddHsizePredicate) {
  expectOk(filter, "add hsize=1,2,4");
  EXPECT_EQ(filter.getRule(0)->hash_size_mask, 0x0B);
}

TEST_F(FilterTest, AddChanAutoProvisionsHashChannels) {
  expectOk(filter, "add chan=#fresh,#other");
  // both names auto-added to the store with derived PSKs
  FilterChannel* a = filter.findChannel("#fresh");
  FilterChannel* b = filter.findChannel("#other");
  ASSERT_NE(a, nullptr);
  ASSERT_NE(b, nullptr);
  EXPECT_EQ(a->secret_len, 16);
  // secret is sha256(name)[0..15] per the companion protocol
  uint8_t digest[32];
  mesh::Utils::sha256(digest, sizeof(digest), (const uint8_t*)"#fresh", 6);
  EXPECT_EQ(memcmp(a->secret, digest, 16), 0);
  EXPECT_EQ(filter.getRule(0)->chan_flags, FILTER_CHANFLG_MASK_SET);
}

TEST_F(FilterTest, AddRegionPredicateCanonicalizesViaPrefix) {
  expectOk(filter, "add region=TestN");
  EXPECT_STREQ(filter.getRule(0)->regions, "TestNorth");   // prefix lookup
  expectOk(filter, "add region=*,TestS");
  EXPECT_STREQ(filter.getRule(1)->regions, "unscoped,TestSouth");
}

TEST_F(FilterTest, AddSenderAndTextPatterns) {
  expectOk(filter, "add sender=^SpamBot$ text=^BEACON");
  EXPECT_STREQ(filter.getRule(0)->sender, "^SpamBot$");
  EXPECT_STREQ(filter.getRule(0)->text, "^BEACON");
}

TEST_F(FilterTest, AddQuotedTextWithSpaces) {
  expectOk(filter, "add text=\"^RX in place\"");
  EXPECT_STREQ(filter.getRule(0)->text, "^RX in place");
  auto payload = makeGroupText("Alice", "RX in place now");
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, payload.len);
  auto chan = channelFromStore(filter, 0);
  EXPECT_EQ(filter.checkContent(&pkt, PAYLOAD_TYPE_GRP_TXT, chan, payload.data,
                                payload.len, nullptr),
            FILTER_ACT_DROP);
}

TEST_F(FilterTest, SenderAndTextPredicatesCombine) {
  expectOk(filter, "add sender=^SpamBot text=\"^RX in place\"");
  auto chan = channelFromStore(filter, 0);
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 20);
  auto hit = makeGroupText("SpamBot", "RX in place");
  EXPECT_EQ(filter.checkContent(&pkt, PAYLOAD_TYPE_GRP_TXT, chan, hit.data,
                                hit.len, nullptr),
            FILTER_ACT_DROP);
  auto wrongSender = makeGroupText("OtherGuy", "RX in place");
  EXPECT_EQ(filter.checkContent(&pkt, PAYLOAD_TYPE_GRP_TXT, chan, wrongSender.data,
                                wrongSender.len, nullptr),
            FILTER_ACT_ALLOW);
}

TEST_F(FilterTest, GetQuotesValuesContainingSpaces) {
  expectOk(filter, "add sender=^SpamBot text=\"^RX in place\"");
  EXPECT_NE(cli(filter, "get 0").find("sender=^SpamBot text=\"^RX in place\""),
            std::string::npos);
}

TEST_F(FilterTest, AddForwardAction) {
  expectOk(filter, "add sender=x action=forward");
  EXPECT_EQ(filter.getRule(0)->action, FILTER_ACT_FORWARD);
  EXPECT_EQ(filter.getRule(0)->action, 2);   // byte value is the old logonly: configs load identically
}

// ---------------------------------------------------------------- filter add: rejection + rollback

TEST_F(FilterTest, AddRejectsBadValues) {
  struct { const char* cmd; const char* err_fragment; } cases[] = {
    { "add foo=bar", "Err - unknown param" },
    { "add nokeyvalue", "Err - expected key=value" },
    { "add type=wat", "Err - unknown type" },
    { "add route=sideways", "Err - route must be flood|direct" },
    { "add hops=[2", "Err - bad hops interval" },
    { "add hops=[a,b]", "Err - bad hops interval" },
    { "add hops=70000", "Err - bad hops interval" },   // int16 endpoint guard
    { "add len=[2,*", "Err - bad len interval" },
    { "add snr=abc", "Err - bad snr interval" },
    { "add path=zz", "Err - bad path spec" },          // not hex
    { "add path=A", "Err - bad path spec" },           // odd nibble
    { "add hsize=5", "Err - hsize values are 1..4" },
    { "add chanhash=ABCD", "Err - chanhash must be 2 hex chars" },
    { "add chan=nosuchchan", "Err - unknown chan" },   // non-# names must exist
    { "add region=Nowhere", "Err - unknown region" },
    { "add action=ban", "Err - action must be drop|forward" },
    { "add action=logonly", "Err - action must be drop|forward" },   // renamed keyword
    { "add sender=[a", "Err - bad/long sender regex" },  // compile check rejects
    { "add text=[a", "Err - bad/long text regex" },      // compile check rejects
    { "add text=", "Err - empty regex" },                // empty regex matches everything
    { "add sender=", "Err - empty regex" },
    { "add chan=#", "Err - empty chan name" },           // bare '#' = empty hashtag
    { "add text=\"unterminated", "Err - unbalanced quotes" },
  };
  for (auto& c : cases) {
    EXPECT_EQ(cli(filter, c.cmd).find(c.err_fragment), 0) << c.cmd;
    EXPECT_EQ(filter.getNumRules(), 0) << c.cmd;   // rolled back, no half-rule
  }
}

TEST_F(FilterTest, AddRejectsOverlongRegexWithoutTruncating) {
  std::string cmd = "add sender=";
  cmd += std::string(40, 'a');
  EXPECT_EQ(cli(filter, cmd.c_str()), "Err - bad/long sender regex");
  EXPECT_EQ(filter.getNumRules(), 0);
}

TEST_F(FilterTest, AddPathRejectsEmptySegments) {
  // parseHexHash needs >= 2 hex chars, so empty chain segments must fail
  EXPECT_EQ(cli(filter, "add path=10>"), "Err - bad path spec");
  EXPECT_EQ(cli(filter, "add path=>10"), "Err - bad path spec");
  EXPECT_EQ(cli(filter, "add path=10>>20"), "Err - bad path spec");
  EXPECT_EQ(filter.getNumRules(), 0);
}

TEST_F(FilterTest, LongCommandIsTruncatedSafely) {
  // the remote-CLI layer caps commands at 160 B; filterCLI() itself must also
  // be safe with longer input (bounded copy -> clean regex-length rejection)
  std::string cmd = "add text=" + std::string(300, 'a');
  std::string reply = cli(filter, cmd.c_str());
  EXPECT_EQ(reply.substr(0, 5), "Err -");
  EXPECT_LE(reply.size(), (size_t)MAX_PACKET_PAYLOAD);
  EXPECT_EQ(filter.getNumRules(), 0);
}

TEST_F(FilterTest, AddRejectsWhenFull) {
  for (int i = 0; i < FILTER_MAX_RULES; i++) filter.addRule();
  EXPECT_EQ(cli(filter, "add type=advert"), "Err - rule list full");
}

TEST_F(FilterTest, RuleIdxRejectsNonNumeric) {
  expectOk(filter, "add type=advert");
  EXPECT_EQ(cli(filter, "get abc").substr(0, 5), "Err -");   // atoi() would silently pick rule 0
  EXPECT_EQ(cli(filter, "disable abc").substr(0, 5), "Err -");
  EXPECT_EQ(cli(filter, "enable 1x").substr(0, 5), "Err -");
  EXPECT_EQ(filter.getRule(0)->enabled, true);   // untouched by the rejects
}

TEST_F(FilterTest, RuleIdxRejectsNegativeHexAndHuge) {
  expectOk(filter, "add type=advert");
  EXPECT_EQ(cli(filter, "get -1").substr(0, 5), "Err -");
  EXPECT_EQ(cli(filter, "del -1").substr(0, 5), "Err -");
  EXPECT_EQ(cli(filter, "get 0x10").substr(0, 5), "Err -");   // must not atoi() down to rule 0
  EXPECT_EQ(cli(filter, "get 99999").substr(0, 5), "Err -");
  EXPECT_EQ(cli(filter, "disable 99999999999999").substr(0, 5), "Err -");   // overflows int
  EXPECT_EQ(cli(filter, "get 0").find("r0 "), 0);   // genuine rule 0 still reachable
}

TEST_F(FilterTest, ChanAddRejectsBareHash) {
  EXPECT_EQ(cli(filter, "chan add #"), "Err - empty chan name");
  EXPECT_EQ(cli(filter, "chan add"), "Err - usage: filter chan add <name> [<psk-hex>]");
}

// ---------------------------------------------------------------- rule bookkeeping

TEST_F(FilterTest, GetEnableDisableDelClear) {
  expectOk(filter, "add type=advert");
  EXPECT_EQ(cli(filter, "get 0").find("r0 en drop type=advert"), 0);

  EXPECT_EQ(cli(filter, "disable 0"), "OK - rule 0 disabled");
  EXPECT_FALSE(filter.getRule(0)->enabled);
  EXPECT_EQ(cli(filter, "enable 0"), "OK - rule 0 enabled");
  EXPECT_TRUE(filter.getRule(0)->enabled);

  EXPECT_EQ(cli(filter, "get 5"), "Err - no such rule");
  EXPECT_EQ(cli(filter, "del 5"), "Err - no such rule");
  EXPECT_EQ(cli(filter, "del"), "Err - rule index required");

  expectOk(filter, "add type=txt");
  EXPECT_EQ(cli(filter, "del 0"), "OK - rule 0 deleted");
  EXPECT_EQ(filter.getNumRules(), 1);

  EXPECT_EQ(cli(filter, "clear"), "OK - rules cleared (chans kept)");
  EXPECT_EQ(filter.getNumRules(), 0);
}

TEST_F(FilterTest, ListAndStatsOutput) {
  expectOk(filter, "add type=advert");
  filter.getRule(0)->hits = 3;
  std::string list = cli(filter, "list");
  EXPECT_EQ(list.find("on 1/16: 0eD"), 0);   // enabled + Drop digest line

  std::string stats = cli(filter, "stats");
  EXPECT_EQ(stats.find("hits: 0:3"), 0);
  EXPECT_NE(stats.find("; limiter:0 aborted:0"), std::string::npos);
}

// ---------------------------------------------------------------- channel store

TEST_F(FilterTest, ChanAddNamedRequiresPsk) {
  EXPECT_EQ(cli(filter, "chan add nodata"), "Err - psk required for non-# names");
  // 16-byte psk, named channel
  std::string reply = cli(filter, "chan add backup 00112233445566778899aabbccddeeff");
  EXPECT_EQ(reply.find("OK - chan backup h="), 0);
  FilterChannel* ch = filter.findChannel("backup");
  ASSERT_NE(ch, nullptr);
  EXPECT_EQ(ch->secret_len, 16);
}

TEST_F(FilterTest, ChanAddErrors) {
  EXPECT_EQ(cli(filter, "chan"), cli(filter, "chan list"));   // bare = list
  EXPECT_EQ(cli(filter, "chan add"), "Err - usage: filter chan add <name> [<psk-hex>]");
  EXPECT_EQ(cli(filter, "chan add Public"), "Err - channel exists");
  EXPECT_EQ(cli(filter, "chan add #x deadbeef"), "Err - bad psk or store full");  // odd length
  std::string long_name(FILTER_CHAN_NAME_LEN, 'n');
  EXPECT_EQ(cli(filter, ("chan add " + long_name).c_str()), "Err - name too long");
  EXPECT_EQ(cli(filter, "chan del nosuch"), "Err - unknown channel");
}

TEST_F(FilterTest, ChanAddRejectsBadPskLength) {
  // psk must decode to exactly 16 or 32 bytes
  EXPECT_EQ(cli(filter, "chan add bad1 0011"), "Err - bad psk or store full");
  EXPECT_EQ(cli(filter, "chan add bad2 001122334455667788990011223344556677"), "Err - bad psk or store full");
}

TEST_F(FilterTest, ChanStoreFull) {
  // Public + 15 more fills the 16-slot store
  for (int i = 0; i < FILTER_MAX_CHANNELS - 1; i++) {
    ASSERT_EQ(cli(filter, ("chan add #full" + std::to_string(i)).c_str()).substr(0, 3), "OK ");
  }
  EXPECT_EQ(cli(filter, "chan add #onemore"), "Err - bad psk or store full");
}

TEST_F(FilterTest, AddChanAutoProvisionStoreFull) {
  // Public + 15 more fills the 16-slot store; auto-provision must report the
  // real reason, not "unknown chan"
  for (int i = 0; i < FILTER_MAX_CHANNELS - 1; i++) {
    ASSERT_EQ(cli(filter, ("chan add #full" + std::to_string(i)).c_str()).substr(0, 3), "OK ");
  }
  EXPECT_EQ(cli(filter, "add chan=#overflow"), "Err - chan store full");
}

TEST_F(FilterTest, ChanDelRemapsRuleMasks) {
  expectOk(filter, "chan add #a");
  expectOk(filter, "chan add #b");
  expectOk(filter, "chan add #c");
  // store: 0=Public, 1=#a, 2=#b, 3=#c
  ASSERT_EQ(cli(filter, "add chan=#a,#b,#c"), "OK - rule 0 added");
  EXPECT_EQ(filter.getRule(0)->chan_mask, 0x0E);

  // deleting #a (idx 1) shifts #b/#c down one slot and drops the bit
  ASSERT_EQ(cli(filter, "chan del #a"), "OK - chan #a deleted");
  EXPECT_EQ(filter.getNumChannels(), 3);
  FilterRule* r = filter.getRule(0);
  EXPECT_EQ(r->chan_mask, 0x06);   // now #b (idx1), #c (idx2)

  // behaviourally: the rule still matches the delivered #b channel
  int b_idx = -1;
  for (int i = 0; i < filter.getNumChannels(); i++) {
    if (strcmp(filter.getChannel(i)->name, "#b") == 0) b_idx = i;
  }
  ASSERT_GE(b_idx, 0);
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 10);
  EXPECT_EQ(filter.checkContent(&pkt, PAYLOAD_TYPE_GRP_TXT, channelFromStore(filter, b_idx),
                                nullptr, 0, nullptr),
            FILTER_ACT_DROP);
}

TEST_F(FilterTest, ChanListShowsDerivedFlag) {
  std::string reply = cli(filter, "chan list");
  // "<idx>:<name> k<len> h<hash>" — Public's hash is sha256(its psk)[0]
  char expect_head[40];
  auto pub = filter.getChannel(0);
  snprintf(expect_head, sizeof(expect_head), "0:Public k%u h%02X", pub->secret_len, pub->hash);
  EXPECT_NE(reply.find(expect_head), std::string::npos);
  EXPECT_EQ(reply.find(" d"), std::string::npos);   // provisioned Public has an explicit psk

  // a '#' channel with a companion-derived psk gets the " d" marker
  ASSERT_EQ(cli(filter, "chan add #derived").substr(0, 3), "OK ");
  reply = cli(filter, "chan list");
  EXPECT_NE(reply.find(" d"), std::string::npos);
}

// ---------------------------------------------------------------- ratelimit commands

TEST_F(FilterTest, RatelimitCommands) {
  EXPECT_EQ(cli(filter, "ratelimit"), "ratelimit advert 0h; cache 0/256");
  EXPECT_EQ(cli(filter, "ratelimit advert 48"), "OK - advert ratelimit 48h");
  EXPECT_EQ(filter.getAdvertRatelimit(), 48);
  EXPECT_EQ(cli(filter, "ratelimit advert 721"), "Err - hours must be 0..720 (0=off)");
  EXPECT_EQ(cli(filter, "ratelimit advert -1"), "Err - hours must be 0..720 (0=off)");
  EXPECT_EQ(cli(filter, "ratelimit advert 0"), "OK - advert ratelimit 0h");
  EXPECT_EQ(filter.getAdvertRatelimit(), 0);
  EXPECT_EQ(cli(filter, "ratelimit bogus"), "Err - usage: ratelimit advert <hours>|clear");
}

// ---------------------------------------------------------------- resetStats

TEST_F(FilterTest, ResetStatsClearsCounters) {
  expectOk(filter, "add type=advert");
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_ADVERT);
  filter.checkPacket(&pkt, 0, nullptr);
  ASSERT_GT(filter.getRule(0)->hits, 0u);

  filter.resetStats();
  EXPECT_EQ(filter.getRule(0)->hits, 0u);
  EXPECT_EQ(filter.getLimiterDrops(), 0u);
}

// ============================================================
// UNIT TESTS: forward verdict + single-pass evaluation + filter move
// ============================================================

// Native tests for the terminal `forward` action (allowlist-within-a-
// denylist), the single-pass whole-rule evaluation it relies on, the
// checkContent() -> checkPacket() verdict stash, and `filter move`.

#include <gtest/gtest.h>

#include "FilterTestHelpers.h"

// index of a named channel in the store (-1 if absent)
static int storeIdx(FilterRules& filter, const char* name) {
  for (int i = 0; i < filter.getNumChannels(); i++) {
    if (strcmp(filter.getChannel(i)->name, name) == 0) return i;
  }
  return -1;
}

// run checkContent() on a decrypted "<sender>: <text>" group-text packet
// delivered on channel `chan_name` (packet written into `pkt` so its address
// stays stable for the verdict-stash tests)
static uint8_t contentCheck(FilterRules& filter, mesh::Packet& pkt, const char* chan_name,
                            const char* sender, const char* text) {
  auto payload = makeGroupText(sender, text);
  pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, payload.len);
  int ci = storeIdx(filter, chan_name);
  EXPECT_GE(ci, 0) << chan_name;
  auto chan = channelFromStore(filter, ci);
  return filter.checkContent(&pkt, PAYLOAD_TYPE_GRP_TXT, chan, payload.data, payload.len, nullptr);
}

// ---------------------------------------------------------------- single-pass semantics

TEST_F(FilterTest, ForwardTerminalBeforeDrop) {
  // the allowlist idiom: Alice on #foo passes, everyone else is dropped
  expectOk(filter, "add chan=#foo sender=^Alice$ action=forward");
  expectOk(filter, "add chan=#foo");
  mesh::Packet pkt;
  EXPECT_EQ(contentCheck(filter, pkt, "#foo", "Alice", "hi"), FILTER_ACT_FORWARD);
  EXPECT_EQ(filter.getRule(0)->hits, 1u);
  EXPECT_EQ(filter.getRule(1)->hits, 0u);   // terminal: later rules not consulted

  EXPECT_EQ(contentCheck(filter, pkt, "#foo", "Bob", "hi"), FILTER_ACT_DROP);   // rule 0 fails
  EXPECT_EQ(filter.getRule(1)->hits, 1u);
}

TEST_F(FilterTest, DropBeforeForwardDropsEvenAlice) {
  // reversed order: the drop matches first and the forward is never reached
  expectOk(filter, "add chan=#foo");
  expectOk(filter, "add chan=#foo sender=^Alice$ action=forward");
  mesh::Packet pkt;
  EXPECT_EQ(contentCheck(filter, pkt, "#foo", "Alice", "hi"), FILTER_ACT_DROP);
  EXPECT_EQ(filter.getRule(0)->hits, 1u);
  EXPECT_EQ(filter.getRule(1)->hits, 0u);
}

TEST_F(FilterTest, ForwardShieldsPacketLevelRules) {
  // a forward verdict is terminal — the old two-pass model would have let the
  // packet-time chanhash scan drop the very packet rule 0 forwarded
  expectOk(filter, "add chan=#foo sender=^Alice$ action=forward");
  expectOk(filter, "add type=txt");   // packet-level drop that also matches
  mesh::Packet pkt;
  ASSERT_EQ(contentCheck(filter, pkt, "#foo", "Alice", "hi"), FILTER_ACT_FORWARD);
  // (payload left byte-identical, as in the real relay flow: the stash is
  // keyed by pointer + content hash, so mutated bytes would forfeit it)
  EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr), FILTER_ACT_FORWARD);   // stash consumed
  EXPECT_EQ(filter.getRule(0)->hits, 1u);
  EXPECT_EQ(filter.getRule(1)->hits, 0u);
}

TEST_F(FilterTest, PacketRuleBeforeContentRulePreempts) {
  // a packet-level rule listed first is terminal at content time for a
  // decrypted group packet — the content rule behind it is not evaluated
  expectOk(filter, "add type=txt");                        // packet-level drop
  expectOk(filter, "add sender=^Alice action=forward");    // never reached
  mesh::Packet pkt;
  EXPECT_EQ(contentCheck(filter, pkt, "Public", "Alice", "hi"), FILTER_ACT_DROP);
  EXPECT_EQ(filter.getRule(0)->hits, 1u);
  EXPECT_EQ(filter.getRule(1)->hits, 0u);
}

TEST_F(FilterTest, FirstMatchWinsMixedList) {
  // interleaved packet-level and content rules: exactly one hit per packet
  // across both scan entry points
  expectOk(filter, "add type=advert");                  // packet-level, no match
  expectOk(filter, "add sender=^Alice action=forward"); // content
  expectOk(filter, "add type=txt");                     // packet-level drop, shadowed
  mesh::Packet pkt;
  EXPECT_EQ(contentCheck(filter, pkt, "Public", "Alice", "hi"), FILTER_ACT_FORWARD);
  EXPECT_EQ(filter.getRule(0)->hits, 0u);
  EXPECT_EQ(filter.getRule(1)->hits, 1u);
  EXPECT_EQ(filter.getRule(2)->hits, 0u);
  EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr), FILTER_ACT_FORWARD);   // no rescan
}

TEST_F(FilterTest, ProbeBeforeDropCountsWhatDropWouldCatch) {
  // shadow-mode idiom: a forward probe immediately before the drop counts
  // exactly what the drop would catch, while the packet still passes
  expectOk(filter, "add chan=#foo action=forward");   // probe
  expectOk(filter, "add chan=#foo");                  // the drop it shadows
  mesh::Packet pkt;
  EXPECT_EQ(contentCheck(filter, pkt, "#foo", "Bob", "hi"), FILTER_ACT_FORWARD);
  EXPECT_EQ(filter.getRule(0)->hits, 1u);
  EXPECT_EQ(filter.getRule(1)->hits, 0u);

  // flip the probe to an enforcing rule: same traffic now drops
  ASSERT_EQ(cli(filter, "del 0"), "OK - rule 0 deleted");
  EXPECT_EQ(contentCheck(filter, pkt, "#foo", "Bob", "hi"), FILTER_ACT_DROP);
}

TEST_F(FilterTest, TrailingCatchAllProbeCountsOnlyUnmatched) {
  // a catch-all forward rule at the END of the list only sees packets no
  // earlier rule matched, so it tallies surviving traffic
  expectOk(filter, "add chan=#memes");                 // drop
  expectOk(filter, "add type=txt action=forward");     // catch-all probe, last
  mesh::Packet pkt;
  EXPECT_EQ(contentCheck(filter, pkt, "#memes", "Bob", "hi"), FILTER_ACT_DROP);
  EXPECT_EQ(filter.getRule(0)->hits, 1u);
  EXPECT_EQ(filter.getRule(1)->hits, 0u);   // drop short-circuits the probe

  EXPECT_EQ(contentCheck(filter, pkt, "Public", "Alice", "hi"), FILTER_ACT_FORWARD);
  EXPECT_EQ(filter.getRule(0)->hits, 1u);   // untouched
  EXPECT_EQ(filter.getRule(1)->hits, 1u);   // probe tallies the survivor
}

TEST_F(FilterTest, DisabledForwardRuleSkipped) {
  // disabling the forward exception restores the channel drop for everyone
  expectOk(filter, "add chan=#foo sender=^Alice$ action=forward");
  expectOk(filter, "add chan=#foo");
  ASSERT_EQ(cli(filter, "disable 0"), "OK - rule 0 disabled");
  mesh::Packet pkt;
  EXPECT_EQ(contentCheck(filter, pkt, "#foo", "Alice", "hi"), FILTER_ACT_DROP);
}

TEST_F(FilterTest, DisabledFilterForwardMoot) {
  // filter off: everything is allowed and no verdict is stashed — once the
  // filter is back on, the same packet is scanned normally
  ASSERT_EQ(cli(filter, "off"), "OK - filter off");
  expectOk(filter, "add chan=#foo sender=^Alice$ action=forward");
  expectOk(filter, "add type=txt");
  mesh::Packet pkt;
  EXPECT_EQ(contentCheck(filter, pkt, "#foo", "Alice", "hi"), FILTER_ACT_ALLOW);
  ASSERT_EQ(cli(filter, "on"), "OK - filter on");
  EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr), FILTER_ACT_DROP);   // rule 1 scanned
  EXPECT_EQ(filter.getRule(1)->hits, 1u);
}

// ---------------------------------------------------------------- packet classes

TEST_F(FilterTest, PacketOnlyForwardOnAdvertAndDirect) {
  // packet-level forward rules decide on traffic checkContent never sees
  expectOk(filter, "add type=advert route=flood action=forward");
  expectOk(filter, "add route=direct action=forward");
  auto adv = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_ADVERT, 64, 1, 2);
  EXPECT_EQ(filter.checkPacket(&adv, 1000, nullptr), FILTER_ACT_FORWARD);
  EXPECT_EQ(filter.getRule(0)->hits, 1u);
  auto direct = makePacket(ROUTE_TYPE_DIRECT, PAYLOAD_TYPE_TXT_MSG, 10);
  EXPECT_EQ(filter.checkPacket(&direct, 1000, nullptr), FILTER_ACT_FORWARD);
  EXPECT_EQ(filter.getRule(1)->hits, 1u);
}

TEST_F(FilterTest, UndecryptedGroupPacketSkipsContentRules) {
  // group traffic that never decrypts only ever sees the packet-level scan
  expectOk(filter, "add chan=#neverseen");            // content rule (deferred)
  expectOk(filter, "add type=txt action=forward");    // packet-level
  expectOk(filter, "add type=advert");                // packet-level drop
  auto txt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 20);   // no checkContent ran
  EXPECT_EQ(filter.checkPacket(&txt, 0, nullptr), FILTER_ACT_FORWARD);
  EXPECT_EQ(filter.getRule(0)->hits, 0u);   // content rule untouched
  EXPECT_EQ(filter.getRule(1)->hits, 1u);
  auto adv = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_ADVERT, 64, 1, 2);
  EXPECT_EQ(filter.checkPacket(&adv, 0, nullptr), FILTER_ACT_DROP);
  EXPECT_EQ(filter.getRule(2)->hits, 1u);
  EXPECT_EQ(filter.getRule(0)->hits, 0u);
}

TEST_F(FilterTest, GrpDataForwardRuleHonoursChanIgnoresSenderText) {
  // GRP_DATA has no parseable sender: the sender predicate can't match, so
  // the rule is skipped even though the channel matches
  expectOk(filter, "add chan=#foo sender=^Alice$ action=forward");
  mesh::Packet pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_DATA, 20);
  uint8_t raw[] = { 0, 0, 0, 0, 'x' };   // not even parsed for data
  EXPECT_EQ(filter.checkContent(&pkt, PAYLOAD_TYPE_GRP_DATA,
                                channelFromStore(filter, storeIdx(filter, "#foo")),
                                raw, sizeof(raw), nullptr),
            FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.getRule(0)->hits, 0u);
}

TEST_F(FilterTest, LimiterStillAppliesAfterForward) {
  // forward short-circuits the rule list only — never the advert rate limiter
  expectOk(filter, "add type=advert action=forward");
  filter.setAdvertRatelimit(48);
  uint8_t key[4] = { 0x21, 0x22, 0x23, 0x24 };
  auto pkt = makeAdvert(key);
  EXPECT_EQ(filter.checkPacket(&pkt, 1000, nullptr), FILTER_ACT_FORWARD);
  EXPECT_EQ(filter.getRule(0)->hits, 1u);
  pkt.transport_codes[0] = 7;
  EXPECT_EQ(filter.checkPacket(&pkt, 2000, nullptr), FILTER_ACT_DROP);   // limiter wins
  EXPECT_EQ(filter.getLimiterDrops(), 1u);
  EXPECT_EQ(filter.getRule(0)->hits, 2u);   // one scan per received advert (single pass)
}

TEST_F(FilterTest, LimiterUnaffectedByForwardOnGroupPackets) {
  // group traffic never touches the advert rate limiter (payload-type guard)
  expectOk(filter, "add type=txt action=forward");
  filter.setAdvertRatelimit(48);
  mesh::Packet pkt;
  EXPECT_EQ(contentCheck(filter, pkt, "Public", "Alice", "hi"), FILTER_ACT_FORWARD);
  EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr), FILTER_ACT_FORWARD);   // stash consumed
  EXPECT_EQ(filter.getAdvertCacheCount(), 0);
}

// ---------------------------------------------------------------- verdict stash handoff

TEST_F(FilterTest, StashConsumedOnceNoDoubleHits) {
  // a packet-level rule matching a decrypted group packet is counted once at
  // content time; checkPacket() serves the stashed verdict without rescanning
  expectOk(filter, "add type=txt");
  mesh::Packet pkt;
  EXPECT_EQ(contentCheck(filter, pkt, "Public", "Alice", "hi"), FILTER_ACT_DROP);
  EXPECT_EQ(filter.getRule(0)->hits, 1u);
  EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr), FILTER_ACT_DROP);
  EXPECT_EQ(filter.getRule(0)->hits, 1u);
}

TEST_F(FilterTest, StashCarriesAllowVerdict) {
  // even an allow verdict is stashed: no surprise packet-level drop later
  expectOk(filter, "add type=advert");   // matches nothing here
  mesh::Packet pkt;
  EXPECT_EQ(contentCheck(filter, pkt, "Public", "Alice", "hi"), FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.checkPacket(&pkt, 0, nullptr), FILTER_ACT_ALLOW);
  EXPECT_EQ(filter.getRule(0)->hits, 0u);   // not rescanned
}

TEST_F(FilterTest, DropVerdictStashLingers) {
  // a content drop makes core mark the packet DoNotRetransmit, so checkPacket
  // is never called for it; the next, different packet must clear the stale
  // stash by pointer mismatch and be scanned normally
  expectOk(filter, "add sender=^Alice");   // content drop
  expectOk(filter, "add type=advert");     // packet-level drop for the next pkt
  mesh::Packet a;
  EXPECT_EQ(contentCheck(filter, a, "Public", "Alice", "hi"), FILTER_ACT_DROP);
  auto adv = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_ADVERT, 64, 1, 2);
  EXPECT_EQ(filter.checkPacket(&adv, 0, nullptr), FILTER_ACT_DROP);   // scanned
  EXPECT_EQ(filter.getRule(1)->hits, 1u);
}

TEST_F(FilterTest, StashStaleByPointerAndHash) {
  expectOk(filter, "add chan=#foo sender=^Alice$ action=forward");   // content rule
  expectOk(filter, "add chanhash=AA");                               // packet-level drop
  mesh::Packet a, b;

  // different pointer: stash cleared, normal scan (rule 1 matches)
  ASSERT_EQ(contentCheck(filter, a, "#foo", "Alice", "hi"), FILTER_ACT_FORWARD);
  EXPECT_EQ(filter.getRule(0)->hits, 1u);
  b = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 20);
  b.payload[0] = 0xAA;
  EXPECT_EQ(filter.checkPacket(&b, 0, nullptr), FILTER_ACT_DROP);
  EXPECT_EQ(filter.getRule(1)->hits, 1u);

  // same buffer, same bytes (the real relay flow): stash consumed at any age
  ASSERT_EQ(contentCheck(filter, a, "#foo", "Alice", "hi"), FILTER_ACT_FORWARD);
  EXPECT_EQ(filter.checkPacket(&a, 0, nullptr), FILTER_ACT_FORWARD);
  EXPECT_EQ(filter.getRule(1)->hits, 1u);

  // same buffer re-used by the pool with different content: stale, scan
  ASSERT_EQ(contentCheck(filter, a, "#foo", "Alice", "hi"), FILTER_ACT_FORWARD);
  a.payload[0] = 0xAA;
  EXPECT_EQ(filter.checkPacket(&a, 0, nullptr), FILTER_ACT_DROP);
  EXPECT_EQ(filter.getRule(1)->hits, 2u);
}

TEST_F(FilterTest, StashClearedOnNewSequence) {
  expectOk(filter, "add chan=#foo sender=^Alice$ action=forward");   // rule 0
  expectOk(filter, "add chan=#foo");                                 // rule 1 (drop)
  mesh::Packet a, b;

  // forward-then-drop: b's drop stash replaces a's forward stash; a is never
  // handed to checkPacket (dropped packets aren't relayed), and b's verdict
  // is the one served
  ASSERT_EQ(contentCheck(filter, a, "#foo", "Alice", "hi"), FILTER_ACT_FORWARD);
  ASSERT_EQ(contentCheck(filter, b, "#foo", "Bob", "hi"), FILTER_ACT_DROP);
  EXPECT_EQ(filter.checkPacket(&b, 0, nullptr), FILTER_ACT_DROP);

  // drop-then-forward: the lingering drop stash is overwritten before it can
  // mis-serve the next packet
  ASSERT_EQ(contentCheck(filter, a, "#foo", "Bob", "hi"), FILTER_ACT_DROP);
  ASSERT_EQ(contentCheck(filter, b, "#foo", "Alice", "hi"), FILTER_ACT_FORWARD);
  EXPECT_EQ(filter.checkPacket(&b, 0, nullptr), FILTER_ACT_FORWARD);
  EXPECT_EQ(filter.getRule(0)->hits, 2u);   // Alice matched twice
  EXPECT_EQ(filter.getRule(1)->hits, 2u);   // Bob matched twice
}

// ---------------------------------------------------------------- rename (logonly -> forward)

TEST_F(FilterTest, LogonlyKeywordRejected) {
  EXPECT_EQ(cli(filter, "add sender=x action=logonly"), "Err - action must be drop|forward");
  EXPECT_EQ(filter.getNumRules(), 0);   // rolled back, no half rule
}

TEST_F(FilterTest, LegacyByteLoadsAsForward) {
  // action byte 2 on disk (written by pre-rename firmware as logonly) loads,
  // evaluates, and displays as forward — the value was reused, not remapped
  ASSERT_EQ(cli(filter, "add sender=^Alice action=forward"), "OK - rule 0 added");
  filter.save(&fs);
  ASSERT_EQ(fs.files[CFG_FILE][8], 2);   // the record's action byte carries value 2

  FilterRules restored;
  restored.begin(&fs);
  ASSERT_EQ(restored.getNumRules(), 1);
  EXPECT_EQ(restored.getRule(0)->action, 2);
  auto payload = makeGroupText("Alice", "hi");
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, payload.len);
  EXPECT_EQ(restored.checkContent(&pkt, PAYLOAD_TYPE_GRP_TXT, channelFromStore(restored, 0),
                                  payload.data, payload.len, nullptr),
            FILTER_ACT_FORWARD);
  EXPECT_EQ(cli(restored, "get 0").find("r0 en forward"), 0);

  // digest identical to the same rule added via the new CLI
  NativeFS fs2;
  FilterRules fresh;
  fresh.begin(&fs2);
  expectOk(fresh, "add sender=^Alice action=forward");
  fresh.save(&fs2);
  FilterRules freshRestored;
  freshRestored.begin(&fs2);
  EXPECT_EQ(cli(restored, "list"), cli(freshRestored, "list"));
}

TEST_F(FilterTest, ListAndGetShowForward) {
  expectOk(filter, "add sender=^Alice action=forward");
  std::string list = cli(filter, "list");
  EXPECT_EQ(list.find("on 1/16: 0eF"), 0);   // F = forward, no L anymore
  EXPECT_EQ(cli(filter, "get 0").find("r0 en forward"), 0);
}

// ---------------------------------------------------------------- filter move

TEST_F(FilterTest, MoveForwardAndBackward) {
  expectOk(filter, "add hops=11");
  expectOk(filter, "add hops=22");
  expectOk(filter, "add hops=33");
  ASSERT_EQ(cli(filter, "move 0 2"), "OK - rule 0 moved to 2");   // rule ends up AT index 2
  EXPECT_LE(strlen("OK - rule 15 moved to 15"), (size_t)160);   // remote-CLI reply discipline
  EXPECT_EQ(filter.getRule(0)->hops.lo, 22);
  EXPECT_EQ(filter.getRule(1)->hops.lo, 33);
  EXPECT_EQ(filter.getRule(2)->hops.lo, 11);
  ASSERT_EQ(cli(filter, "move 2 0"), "OK - rule 2 moved to 0");
  EXPECT_EQ(filter.getRule(0)->hops.lo, 11);
  EXPECT_EQ(filter.getRule(1)->hops.lo, 22);
  EXPECT_EQ(filter.getRule(2)->hops.lo, 33);
}

TEST_F(FilterTest, MoveRejects) {
  EXPECT_EQ(cli(filter, "move 0 1"), "Err - no such rule");   // empty list
  expectOk(filter, "add hops=1");
  EXPECT_EQ(cli(filter, "move 0 0"), "Err - move: source and target are the same rule");   // no-op rejected
  EXPECT_EQ(cli(filter, "move 0 1"), "Err - no such rule");
  EXPECT_EQ(cli(filter, "move 1 0"), "Err - no such rule");
  EXPECT_EQ(cli(filter, "move -1 0").substr(0, 5), "Err -");
  EXPECT_EQ(cli(filter, "move 0 -1").substr(0, 5), "Err -");
  EXPECT_EQ(cli(filter, "move 0x1 0").substr(0, 5), "Err -");
  EXPECT_EQ(cli(filter, "move 0 99999").substr(0, 5), "Err -");
  EXPECT_EQ(cli(filter, "move 99999999999999 0").substr(0, 5), "Err -");
  EXPECT_EQ(cli(filter, "move").substr(0, 5), "Err -");       // missing args
  EXPECT_EQ(cli(filter, "move 0").substr(0, 5), "Err -");     // missing target
  EXPECT_EQ(filter.getRule(0)->hops.lo, 1);   // untouched by the rejects
  EXPECT_EQ(filter.getNumRules(), 1);
}

TEST_F(FilterTest, MoveCarriesHits) {
  expectOk(filter, "add type=advert");
  expectOk(filter, "add type=txt");
  auto adv = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_ADVERT);
  filter.checkPacket(&adv, 0, nullptr);
  ASSERT_EQ(filter.getRule(0)->hits, 1u);
  ASSERT_EQ(cli(filter, "move 0 1"), "OK - rule 0 moved to 1");
  EXPECT_EQ(filter.getRule(1)->hits, 1u);   // hits travel with the rule, not the slot
  EXPECT_EQ(filter.getRule(0)->hits, 0u);
  filter.resetStats();
  EXPECT_EQ(filter.getRule(0)->hits, 0u);
  EXPECT_EQ(filter.getRule(1)->hits, 0u);
}

TEST_F(FilterTest, MovePersistsOrder) {
  expectOk(filter, "add hops=11");
  expectOk(filter, "add hops=22");
  filter.getRule(1)->hits = 42;   // hits are RAM-only
  ASSERT_EQ(cli(filter, "move 1 0"), "OK - rule 1 moved to 0");
  filter.save(&fs);
  EXPECT_EQ(fs.files[CFG_FILE][0], 4);   // config version byte unchanged

  FilterRules restored;
  restored.begin(&fs);
  ASSERT_EQ(restored.getNumRules(), 2);
  EXPECT_EQ(restored.getRule(0)->hops.lo, 22);
  EXPECT_EQ(restored.getRule(1)->hops.lo, 11);
  EXPECT_EQ(restored.getRule(0)->hits, 0u);   // still never persisted
}

TEST_F(FilterTest, MoveLeavesChanMasksAlone) {
  // masks index the channel store, not rule slots: remap + move compose
  expectOk(filter, "chan add #a");
  expectOk(filter, "chan add #b");
  expectOk(filter, "chan add #c");
  ASSERT_EQ(cli(filter, "add chan=#a,#b,#c"), "OK - rule 0 added");   // mask 0x0E
  ASSERT_EQ(cli(filter, "add chan=#b"), "OK - rule 1 added");         // mask 0x04
  ASSERT_EQ(cli(filter, "chan del #a"), "OK - chan #a deleted");      // remap: #b=1, #c=2
  ASSERT_EQ(cli(filter, "move 0 1"), "OK - rule 0 moved to 1");
  EXPECT_EQ(filter.getRule(0)->chan_mask, 0x02);   // #b (was rule 1)
  EXPECT_EQ(filter.getRule(1)->chan_mask, 0x06);   // #b,#c (was rule 0)
  int b_idx = storeIdx(filter, "#b");
  ASSERT_GE(b_idx, 0);
  auto pkt = makePacket(ROUTE_TYPE_FLOOD, PAYLOAD_TYPE_GRP_TXT, 20);
  EXPECT_EQ(filter.checkContent(&pkt, PAYLOAD_TYPE_GRP_TXT, channelFromStore(filter, b_idx),
                                nullptr, 0, nullptr),
            FILTER_ACT_DROP);   // still matches the same named channel
}

TEST_F(FilterTest, MoveChangesVerdicts) {
  // moving the forward exception behind the channel drop flips a previously
  // protected packet to dropped (end-to-end semantic check)
  expectOk(filter, "add chan=#foo sender=^Alice$ action=forward");
  expectOk(filter, "add chan=#foo");
  mesh::Packet pkt;
  EXPECT_EQ(contentCheck(filter, pkt, "#foo", "Alice", "hi"), FILTER_ACT_FORWARD);
  ASSERT_EQ(cli(filter, "move 0 1"), "OK - rule 0 moved to 1");   // drop now first
  EXPECT_EQ(contentCheck(filter, pkt, "#foo", "Alice", "hi"), FILTER_ACT_DROP);
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
