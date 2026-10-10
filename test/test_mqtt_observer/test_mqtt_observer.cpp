#include <gtest/gtest.h>

#include <string>
#include "MQTTObserverConfig.h"
#include "MQTTObserverFormat.h"
#include "MQTTObserverPolicy.h"
#include "MQTTObserverRecord.h"
#include "PersistUtil.h"

TEST(MQTTObserverConfig, RoundTripsTwoBrokerSettings) {
  MQTTObserverConfig config;
  MQTTObserverConfigCodec::setDefaults(config);
  strcpy(config.wifi_ssid, "mesh-net");
  strcpy(config.wifi_password, "wifi-secret");
  strcpy(config.iata, "SEA");
  strcpy(config.origin, "Roof Repeater");
  strcpy(config.ntp_server, "192.168.1.1");
  config.status_interval_minutes = 12;
  config.wifi_power_save = 2;
  config.brokers[0].enabled = 1;
  strcpy(config.brokers[0].server, "wss://mqtt.example:443/mqtt");
  strcpy(config.brokers[0].username, "v1_public-key");
  strcpy(config.brokers[0].password, "broker-secret");
  strcpy(config.brokers[0].audience, "mqtt.example");
  strcpy(config.brokers[0].topic, "meshcore/{iata}/{device}/{type}");
  strcpy(config.brokers[0].token, "site-token");
  strcpy(config.brokers[0].owner,
         "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  strcpy(config.brokers[0].email, "operator@example.org");
  config.brokers[0].port = 8883;
  config.brokers[1].enabled = 1;
  strcpy(config.brokers[1].server, "mqtt://second.example");
  strcpy(config.brokers[1].username, "second-user");
  config.brokers[5].enabled = 1;
  strcpy(config.brokers[5].server, "mqtt://reserved-slot.example");

  uint8_t payload[MQTTObserverConfigCodec::kPayloadSize];
  ASSERT_EQ(MQTTObserverConfigCodec::encode(config, payload, sizeof(payload)), sizeof(payload));
  MQTTObserverConfig decoded;
  ASSERT_TRUE(MQTTObserverConfigCodec::decode(payload, sizeof(payload), decoded));
  EXPECT_STREQ(decoded.wifi_ssid, "mesh-net");
  EXPECT_STREQ(decoded.wifi_password, "wifi-secret");
  EXPECT_STREQ(decoded.iata, "SEA");
  EXPECT_STREQ(decoded.origin, "Roof Repeater");
  EXPECT_STREQ(decoded.ntp_server, "192.168.1.1");
  EXPECT_EQ(decoded.status_interval_minutes, 12);
  EXPECT_EQ(decoded.wifi_power_save, 2);
  EXPECT_EQ(decoded.brokers[0].enabled, 1);
  EXPECT_STREQ(decoded.brokers[0].server, "wss://mqtt.example:443/mqtt");
  EXPECT_STREQ(decoded.brokers[0].password, "broker-secret");
  EXPECT_STREQ(decoded.brokers[0].audience, "mqtt.example");
  EXPECT_STREQ(decoded.brokers[0].owner, config.brokers[0].owner);
  EXPECT_STREQ(decoded.brokers[0].email, "operator@example.org");
  EXPECT_EQ(decoded.brokers[0].port, 8883);
  EXPECT_STREQ(decoded.brokers[1].server, "mqtt://second.example");
  EXPECT_STREQ(decoded.brokers[1].username, "second-user");
  EXPECT_EQ(decoded.brokers[5].enabled, 1);
  EXPECT_STREQ(decoded.brokers[5].server, "mqtt://reserved-slot.example");
}

TEST(MQTTObserverConfig, RejectsInvalidValuesAndIntegrityChanges) {
  MQTTObserverConfig config;
  MQTTObserverConfigCodec::setDefaults(config);
  strcpy(config.brokers[0].server, "https://mqtt.example");
  EXPECT_EQ(MQTTObserverConfigCodec::encode(config, nullptr, 0), 0u);

  strcpy(config.brokers[0].server, "mqtts://mqtt.example");
  strcpy(config.iata, "Sea");
  EXPECT_EQ(MQTTObserverConfigCodec::encode(config, nullptr, 0), 0u);

  strcpy(config.iata, "SEA");
  uint8_t payload[MQTTObserverConfigCodec::kPayloadSize];
  ASSERT_EQ(MQTTObserverConfigCodec::encode(config, payload, sizeof(payload)), sizeof(payload));
  uint16_t checksum = crc16_ccitt(payload, sizeof(payload));
  payload[0] = 2;
  EXPECT_NE(crc16_ccitt(payload, sizeof(payload)), checksum);
  EXPECT_FALSE(MQTTObserverConfigCodec::decode(payload, sizeof(payload), config));
  EXPECT_EQ(config.status_enabled, 1);
  EXPECT_FALSE(MQTTObserverConfigCodec::decode(payload, sizeof(payload) - 1, config));
}

TEST(MQTTObserverRecord, ChecksVersionLengthAndChecksum) {
  MQTTObserverConfig original;
  MQTTObserverConfigCodec::setDefaults(original);
  original.brokers[0].enabled = 1;
  strcpy(original.brokers[0].server, "mqtts://broker.example");
  original.brokers[1].enabled = 1;
  strcpy(original.brokers[1].server, "mqtt://second.example");
  original.brokers[5].enabled = 1;
  strcpy(original.brokers[5].server, "mqtt://reserved.example");
  strcpy(original.iata, "SEA");
  uint8_t record[MQTTObserverRecord::kRecordSize];
  ASSERT_EQ(MQTTObserverRecord::encode(original, record, sizeof(record)), sizeof(record));

  MQTTObserverConfig decoded;
  ASSERT_TRUE(MQTTObserverRecord::decode(record, sizeof(record), decoded));
  EXPECT_STREQ(decoded.brokers[0].server, original.brokers[0].server);
  EXPECT_STREQ(decoded.brokers[1].server, original.brokers[1].server);
  EXPECT_STREQ(decoded.brokers[5].server, original.brokers[5].server);
  EXPECT_STREQ(decoded.iata, original.iata);
  EXPECT_FALSE(MQTTObserverRecord::decode(record, sizeof(record) - 1, decoded));

  uint8_t damaged[MQTTObserverRecord::kRecordSize];
  memcpy(damaged, record, sizeof(record));
  damaged[MQTTObserverRecord::kHeaderSize] ^= 1;
  EXPECT_FALSE(MQTTObserverRecord::decode(damaged, sizeof(damaged), decoded));

  memcpy(damaged, record, sizeof(record));
  damaged[4]++;
  uint16_t crc = crc16_ccitt(damaged, sizeof(damaged) - 2);
  damaged[sizeof(damaged) - 2] = crc & 0xff;
  damaged[sizeof(damaged) - 1] = crc >> 8;
  EXPECT_FALSE(MQTTObserverRecord::decode(damaged, sizeof(damaged), decoded));
}


TEST(MQTTObserverConfig, IataSupportsAlphanumericAndRejectsPlaceholder) {
  EXPECT_TRUE(MQTTObserverConfigCodec::validIata("A1B"));
  EXPECT_FALSE(MQTTObserverConfigCodec::validIata("XXX"));
}

TEST(MQTTObserverConfig, EndpointLabelOmitsSchemeAndPath) {
  char host[64];
  MQTTObserverConfigCodec::hostLabel("wss://broker.example:443/mqtt", host, sizeof(host));
  EXPECT_STREQ(host, "broker.example:443");
  MQTTObserverConfigCodec::hostLabel("mqtt://localhost", host, sizeof(host));
  EXPECT_STREQ(host, "localhost");
}

TEST(MQTTObserverConfig, BuildsBrokerUrisAndHonorsExplicitPorts) {
  MQTTObserverConfig config;
  MQTTObserverConfigCodec::setDefaults(config);
  char uri[192];
  strcpy(config.brokers[0].server, "broker.example");
  config.brokers[0].port = 8883;
  ASSERT_TRUE(MQTTObserverConfigCodec::buildServerUri(config.brokers[0], uri, sizeof(uri)));
  EXPECT_STREQ(uri, "mqtts://broker.example:8883");

  strcpy(config.brokers[0].server, "wss://broker.example/mqtt");
  config.brokers[0].port = 1883;
  ASSERT_TRUE(MQTTObserverConfigCodec::buildServerUri(config.brokers[0], uri, sizeof(uri)));
  EXPECT_STREQ(uri, "wss://broker.example:443/mqtt");
  EXPECT_EQ(MQTTObserverConfigCodec::configuredPort(config.brokers[0]), 443);

  strcpy(config.brokers[0].server, "mqtts://broker.example:9000/mqtt");
  ASSERT_TRUE(MQTTObserverConfigCodec::buildServerUri(config.brokers[0], uri, sizeof(uri)));
  EXPECT_STREQ(uri, "mqtts://broker.example:9000/mqtt");
  EXPECT_EQ(MQTTObserverConfigCodec::configuredPort(config.brokers[0]), 9000);
}

TEST(MQTTObserverConfig, ExpandsReferenceTopicTemplate) {
  MQTTObserverConfig config;
  MQTTObserverConfigCodec::setDefaults(config);
  strcpy(config.iata, "A1B");
  strcpy(config.brokers[0].token, "region-token");
  char topic[160];
  ASSERT_TRUE(MQTTObserverConfigCodec::buildTopic(config, config.brokers[0], "AABB", "packets", topic, sizeof(topic)));
  EXPECT_STREQ(topic, "meshcore/A1B/AABB/packets");
  strcpy(config.brokers[0].topic, "local/{token}/{device}/{type}");
  ASSERT_TRUE(MQTTObserverConfigCodec::buildTopic(config, config.brokers[0], "AABB", "raw", topic, sizeof(topic)));
  EXPECT_STREQ(topic, "local/region-token/AABB/raw");
  char short_topic[8];
  EXPECT_FALSE(MQTTObserverConfigCodec::buildTopic(config, config.brokers[0], "AABB", "packets",
                                                    short_topic, sizeof(short_topic)));
}

TEST(MQTTObserverRawStager, ConsumesOnlyTheImmediatelyStagedFrame) {
  MQTTObserverRawStager stager;
  uint8_t first[] = {1, 2, 3};
  MQTTObserverRawStager::Snapshot snapshot;
  stager.stage(first, sizeof(first), -4.5f, -91.0f);
  ASSERT_TRUE(stager.consume(snapshot));
  EXPECT_EQ(snapshot.length, sizeof(first));
  EXPECT_EQ(snapshot.bytes[2], 3);
  EXPECT_FLOAT_EQ(snapshot.snr, -4.5f);
  EXPECT_FLOAT_EQ(snapshot.rssi, -91.0f);
  EXPECT_FALSE(stager.consume(snapshot));

  uint8_t second[] = {9};
  stager.stage(second, sizeof(second), 2.0f, -40.0f);
  stager.stage(nullptr, 0, 0.0f, 0.0f);
  EXPECT_FALSE(stager.consume(snapshot));

  stager.stage(first, sizeof(first), 1.5f, -80.0f);
  uint8_t copied[MQTTObserverRawStager::kCapacity];
  uint16_t copied_len = 0;
  float copied_snr = 0, copied_rssi = 0;
  ASSERT_TRUE(stager.consume(copied, sizeof(copied), copied_len, copied_snr, copied_rssi));
  EXPECT_EQ(copied_len, sizeof(first));
  EXPECT_EQ(copied[0], 1);
  EXPECT_FLOAT_EQ(copied_snr, 1.5f);
  EXPECT_FLOAT_EQ(copied_rssi, -80.0f);
  EXPECT_FALSE(stager.consume(copied, sizeof(copied), copied_len, copied_snr, copied_rssi));
}

TEST(MQTTObserverCommandPolicy, ParsesNumberedSlotsAndRejectsUnnumberedBrokerSettings) {
  size_t slot = 99;
  const char* property = nullptr;
  EXPECT_FALSE(MQTTObserverCommandPolicy::parseSlot("set mqtt.server broker", "set ", 2,
                                                     slot, property));
  ASSERT_TRUE(MQTTObserverCommandPolicy::parseSlot("set mqtt1.server broker", "set ", 2,
                                                    slot, property));
  EXPECT_EQ(slot, 0u);
  EXPECT_STREQ(property, "server broker");
  ASSERT_TRUE(MQTTObserverCommandPolicy::parseSlot("get mqtt1.password", "get ", 2,
                                                    slot, property));
  EXPECT_EQ(slot, 0u);
  EXPECT_STREQ(property, "password");
  ASSERT_TRUE(MQTTObserverCommandPolicy::parseSlot("get mqtt2.status", "get ", 2,
                                                    slot, property));
  EXPECT_EQ(slot, 1u);
  EXPECT_STREQ(property, "status");
  EXPECT_FALSE(MQTTObserverCommandPolicy::parseSlot("get mqtt3.server", "get ", 2,
                                                     slot, property));
  EXPECT_FALSE(MQTTObserverCommandPolicy::parseSlot("get mqtt2x.server", "get ", 2,
                                                     slot, property));
}

TEST(MQTTObserverQueuePolicy, FlushesOnlyAfterLongDisconnectAndRetriesBoundedly) {
  EXPECT_FALSE(MQTTObserverQueuePolicy::shouldFlushDisconnected(60099, 100));
  EXPECT_TRUE(MQTTObserverQueuePolicy::shouldFlushDisconnected(60100, 100));
  EXPECT_FALSE(MQTTObserverQueuePolicy::shouldFlushDisconnected(4, 0xfffffff0));
  EXPECT_TRUE(MQTTObserverQueuePolicy::retryReady(1200, 1100, 1));
  EXPECT_FALSE(MQTTObserverQueuePolicy::retryReady(1099, 1100, 1));
  EXPECT_FALSE(MQTTObserverQueuePolicy::retryReady(0xfffffff8, 4, 1));
  EXPECT_TRUE(MQTTObserverQueuePolicy::retryReady(4, 4, 1));
  EXPECT_EQ(MQTTObserverQueuePolicy::retryDecision(false, 0, 10).action,
            MQTTObserverQueuePolicy::RetryAction::Schedule);
  EXPECT_EQ(MQTTObserverQueuePolicy::retryDecision(false, 3, 10).action,
            MQTTObserverQueuePolicy::RetryAction::Drop);
  EXPECT_EQ(MQTTObserverQueuePolicy::retryDecision(true, 0, 10).action,
            MQTTObserverQueuePolicy::RetryAction::Complete);
}

TEST(MQTTObserverEventQueue, BoundedRetriesAndProtectsInFlightHead) {
  struct Event { int id; };
  typedef MQTTObserverEventQueue<Event, 2> EventQueue;
  EventQueue queue;
  EventQueue::Entry entry;
  Event one = {1}, two = {2}, three = {3}, event;
  EXPECT_EQ(queue.offer(one), EventQueue::QUEUED);
  EXPECT_TRUE(queue.beginAttempt(0, entry));
  EXPECT_EQ(entry.event.id, 1);
  EXPECT_EQ(queue.offer(two), EventQueue::QUEUED);
  EXPECT_EQ(queue.offer(three), EventQueue::DROPPED_OLDEST);
  EXPECT_FALSE(queue.beginAttempt(0, entry));
  queue.retryAttempt(1, 300);
  EXPECT_FALSE(queue.beginAttempt(299, entry));
  EXPECT_TRUE(queue.beginAttempt(300, entry));
  EXPECT_EQ(entry.event.id, 1);
  queue.completeAttempt();
  EXPECT_EQ(queue.size(), 1u);
  EXPECT_TRUE(queue.beginAttempt(301, entry));
  EXPECT_EQ(entry.event.id, 3);
  queue.completeAttempt();
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_FALSE(queue.beginAttempt(400, entry));
}

TEST(MQTTObserverEventQueue, OverflowReplacesOldestPendingEntry) {
  struct Event { int id; };
  typedef MQTTObserverEventQueue<Event, 2> EventQueue;
  EventQueue queue;
  EventQueue::Entry entry;
  EXPECT_EQ(queue.offer(Event{1}), EventQueue::QUEUED);
  EXPECT_EQ(queue.offer(Event{2}), EventQueue::QUEUED);
  EXPECT_EQ(queue.offer(Event{3}), EventQueue::DROPPED_OLDEST);
  ASSERT_TRUE(queue.beginAttempt(0, entry));
  EXPECT_EQ(entry.event.id, 2);
  queue.completeAttempt();
  ASSERT_TRUE(queue.beginAttempt(0, entry));
  EXPECT_EQ(entry.event.id, 3);
}

TEST(MQTTObserverFormat, PacketMessageMatchesObserverSchema) {
  const uint8_t wire[] = {0xa5, 0x00, 0xff};
  const uint8_t hash[] = {0xab, 0xcd};
  const uint8_t path[] = {0xab, 0x01};
  MQTTObserverFormat::PacketView packet = {
    "Roof \"Repeater\"", "001122", 0, 123456, wire, sizeof(wire), 4,
    "D", 3, -4.5f, -91.0f, 1.234f, hash, sizeof(hash), path, 2, 1
  };
  char json[1024];
  size_t written = 0;
  ASSERT_TRUE(MQTTObserverFormat::buildPacket(packet, json, sizeof(json), written));
  std::string payload(json, written);
  EXPECT_NE(payload.find("\"timestamp\":\"1970-01-01T00:00:00.123456+00:00\""), std::string::npos);
  EXPECT_NE(payload.find("\"hash\":\"ABCD\""), std::string::npos);
  EXPECT_NE(payload.find("\"origin\":\"Roof \\\"Repeater\\\"\""), std::string::npos);
  EXPECT_NE(payload.find("\"type\":\"PACKET\""), std::string::npos);
  EXPECT_NE(payload.find("\"direction\":\"rx\""), std::string::npos);
  EXPECT_NE(payload.find("\"packet_type\":\"4\""), std::string::npos);
  EXPECT_NE(payload.find("\"route\":\"D\""), std::string::npos);
  EXPECT_NE(payload.find("\"raw\":\"A500FF\""), std::string::npos);
  EXPECT_NE(payload.find("\"SNR\":\"-4.5\""), std::string::npos);
  EXPECT_NE(payload.find("\"RSSI\":\"-91\""), std::string::npos);
  EXPECT_NE(payload.find("\"score\":\"1234\""), std::string::npos);
  EXPECT_NE(payload.find("\"path\":[\"ab\",\"01\"]"), std::string::npos);
}

TEST(MQTTObserverFormat, RawStatusAndTruncationAreBounded) {
  const uint8_t raw[] = {0, 0x12, 0xab};
  char json[512];
  size_t written = 0;
  ASSERT_TRUE(MQTTObserverFormat::buildRaw("node", "AABB", "timestamp", raw, sizeof(raw),
                                           json, sizeof(json), written));
  EXPECT_EQ(std::string(json, written),
            "{\"origin\":\"node\",\"origin_id\":\"AABB\","
            "\"timestamp\":\"timestamp\",\"type\":\"RAW\",\"data\":\"0012AB\"}");
  ASSERT_TRUE(MQTTObserverFormat::buildStatus("node", "AABB", "MeshCore", "1.0", "radio",
                                               "timestamp", json, sizeof(json), written));
  EXPECT_NE(std::string(json, written).find("\"status\":\"online\""), std::string::npos);
  EXPECT_NE(std::string(json, written).find("\"origin_id\":\"AABB\""), std::string::npos);
  EXPECT_NE(std::string(json, written).find("\"client_version\":\"meshcore-jhuebert/1.0\""), std::string::npos);
  char short_json[8];
  EXPECT_FALSE(MQTTObserverFormat::buildRaw("node", "AABB", "timestamp", raw, sizeof(raw),
                                            short_json, sizeof(short_json), written));
  EXPECT_EQ(short_json[0], 0);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
