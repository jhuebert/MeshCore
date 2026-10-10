#include <gtest/gtest.h>

#include <string>
#include "MQTTObserverConfig.h"
#include "MQTTObserverFormat.h"
#include "MQTTObserverPolicy.h"
#include "MQTTObserverRecord.h"
#include "PersistUtil.h"

TEST(MQTTObserverConfig, RoundTripsFixedPayload) {
  MQTTObserverConfig config;
  MQTTObserverConfigCodec::setDefaults(config);
  config.enabled = 1;
  strcpy(config.wifi_ssid, "mesh-net");
  strcpy(config.wifi_password, "wifi-secret");
  strcpy(config.server, "wss://mqtt.example:443/mqtt");
  strcpy(config.username, "v1_public-key");
  strcpy(config.password, "broker-secret");
  strcpy(config.audience, "mqtt.example");
  strcpy(config.iata, "SEA");
  config.rx_enabled = 1;
  config.status_interval_minutes = 12;
  config.port = 8883;
  strcpy(config.origin, "Roof Repeater");
  strcpy(config.topic, "meshcore/{iata}/{device}/{type}");
  strcpy(config.token, "site-token");
  strcpy(config.ntp_server, "192.168.1.1");
  strcpy(config.owner, "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
  strcpy(config.email, "operator@example.org");
  config.wifi_power_save = 2;

  uint8_t payload[MQTTObserverConfigCodec::kPayloadSize];
  ASSERT_EQ(MQTTObserverConfigCodec::encode(config, payload, sizeof(payload)), sizeof(payload));
  MQTTObserverConfig decoded;
  ASSERT_TRUE(MQTTObserverConfigCodec::decode(payload, sizeof(payload), decoded));
  EXPECT_EQ(decoded.enabled, 1);
  EXPECT_STREQ(decoded.wifi_ssid, "mesh-net");
  EXPECT_STREQ(decoded.wifi_password, "wifi-secret");
  EXPECT_STREQ(decoded.server, "wss://mqtt.example:443/mqtt");
  EXPECT_STREQ(decoded.password, "broker-secret");
  EXPECT_STREQ(decoded.audience, "mqtt.example");
  EXPECT_STREQ(decoded.iata, "SEA");
  EXPECT_EQ(decoded.rx_enabled, 1);
  EXPECT_EQ(decoded.status_interval_minutes, 12);
  EXPECT_EQ(decoded.port, 8883);
  EXPECT_STREQ(decoded.origin, "Roof Repeater");
  EXPECT_STREQ(decoded.topic, "meshcore/{iata}/{device}/{type}");
  EXPECT_STREQ(decoded.token, "site-token");
  EXPECT_STREQ(decoded.ntp_server, "192.168.1.1");
  EXPECT_STREQ(decoded.owner, config.owner);
  EXPECT_STREQ(decoded.email, "operator@example.org");
  EXPECT_EQ(decoded.wifi_power_save, 2);
}

TEST(MQTTObserverConfig, RejectsInvalidValuesAndIntegrityChanges) {
  MQTTObserverConfig config;
  MQTTObserverConfigCodec::setDefaults(config);
  strcpy(config.server, "https://mqtt.example");
  EXPECT_EQ(MQTTObserverConfigCodec::encode(config, nullptr, 0), 0u);

  strcpy(config.server, "mqtts://mqtt.example");
  strcpy(config.iata, "Sea");
  EXPECT_EQ(MQTTObserverConfigCodec::encode(config, nullptr, 0), 0u);

  strcpy(config.iata, "SEA");
  uint8_t payload[MQTTObserverConfigCodec::kPayloadSize];
  ASSERT_EQ(MQTTObserverConfigCodec::encode(config, payload, sizeof(payload)), sizeof(payload));
  uint16_t checksum = crc16_ccitt(payload, sizeof(payload));
  payload[0] = 2;
  EXPECT_NE(crc16_ccitt(payload, sizeof(payload)), checksum);
  EXPECT_FALSE(MQTTObserverConfigCodec::decode(payload, sizeof(payload), config));
  EXPECT_FALSE(MQTTObserverConfigCodec::decode(payload, sizeof(payload) - 1, config));
}

TEST(MQTTObserverRecord, ChecksVersionLengthAndChecksum) {
  MQTTObserverConfig original;
  MQTTObserverConfigCodec::setDefaults(original);
  original.enabled = 1;
  strcpy(original.server, "mqtts://broker.example");
  strcpy(original.iata, "SEA");
  uint8_t record[MQTTObserverRecord::kRecordSize];
  ASSERT_EQ(MQTTObserverRecord::encode(original, record, sizeof(record)), sizeof(record));

  MQTTObserverConfig decoded;
  ASSERT_TRUE(MQTTObserverRecord::decode(record, sizeof(record), decoded));
  EXPECT_STREQ(decoded.server, original.server);
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

TEST(MQTTObserverRecord, MigratesV1SettingsWithNewDefaults) {
  uint8_t record[MQTTObserverRecord::kLegacyV1RecordSize] = {};
  memcpy(record, MQTTObserverRecord::kMagic, sizeof(MQTTObserverRecord::kMagic));
  record[4] = 1;
  record[5] = MQTTObserverConfigCodec::kLegacyPayloadSize & 0xff;
  record[6] = MQTTObserverConfigCodec::kLegacyPayloadSize >> 8;
  uint8_t* payload = record + MQTTObserverRecord::kHeaderSize;
  size_t pos = 0;
  payload[pos++] = 1;
  payload[pos++] = 1;
  payload[pos++] = 1;
  payload[pos++] = 1;
#define COPY_OLD_FIELD(name) \
  memcpy(payload + pos, legacy.name, sizeof(legacy.name)); \
  pos += sizeof(legacy.name)
  MQTTObserverConfig legacy;
  MQTTObserverConfigCodec::setDefaults(legacy);
  strcpy(legacy.wifi_ssid, "old-wifi");
  strcpy(legacy.wifi_password, "old-password");
  strcpy(legacy.server, "mqtts://old-broker.example");
  strcpy(legacy.username, "old-user");
  strcpy(legacy.password, "old-broker-password");
  strcpy(legacy.audience, "old-audience");
  strcpy(legacy.iata, "SEA");
  COPY_OLD_FIELD(wifi_ssid);
  COPY_OLD_FIELD(wifi_password);
  COPY_OLD_FIELD(server);
  COPY_OLD_FIELD(username);
  COPY_OLD_FIELD(password);
  COPY_OLD_FIELD(audience);
  COPY_OLD_FIELD(iata);
#undef COPY_OLD_FIELD
  uint16_t crc = crc16_ccitt(record, sizeof(record) - 2);
  record[sizeof(record) - 2] = crc & 0xff;
  record[sizeof(record) - 1] = crc >> 8;

  MQTTObserverConfig migrated;
  ASSERT_TRUE(MQTTObserverRecord::decode(record, sizeof(record), migrated));
  EXPECT_STREQ(migrated.wifi_ssid, "old-wifi");
  EXPECT_STREQ(migrated.server, "mqtts://old-broker.example");
  EXPECT_STREQ(migrated.iata, "SEA");
  EXPECT_EQ(migrated.rx_enabled, 1);
  EXPECT_EQ(migrated.status_interval_minutes, 5);
  EXPECT_EQ(migrated.port, 1883);
  EXPECT_EQ(migrated.wifi_power_save, 1);
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
  strcpy(config.server, "broker.example");
  config.port = 8883;
  ASSERT_TRUE(MQTTObserverConfigCodec::buildServerUri(config, uri, sizeof(uri)));
  EXPECT_STREQ(uri, "mqtts://broker.example:8883");

  strcpy(config.server, "wss://broker.example/mqtt");
  config.port = 1883;
  ASSERT_TRUE(MQTTObserverConfigCodec::buildServerUri(config, uri, sizeof(uri)));
  EXPECT_STREQ(uri, "wss://broker.example:443/mqtt");
  EXPECT_EQ(MQTTObserverConfigCodec::configuredPort(config), 443);

  strcpy(config.server, "mqtts://broker.example:9000/mqtt");
  ASSERT_TRUE(MQTTObserverConfigCodec::buildServerUri(config, uri, sizeof(uri)));
  EXPECT_STREQ(uri, "mqtts://broker.example:9000/mqtt");
  EXPECT_EQ(MQTTObserverConfigCodec::configuredPort(config), 9000);
}

TEST(MQTTObserverConfig, ExpandsReferenceTopicTemplate) {
  MQTTObserverConfig config;
  MQTTObserverConfigCodec::setDefaults(config);
  strcpy(config.iata, "A1B");
  strcpy(config.token, "region-token");
  char topic[160];
  ASSERT_TRUE(MQTTObserverConfigCodec::buildTopic(config, "AABB", "packets", topic, sizeof(topic)));
  EXPECT_STREQ(topic, "meshcore/A1B/AABB/packets");
  strcpy(config.topic, "local/{token}/{device}/{type}");
  ASSERT_TRUE(MQTTObserverConfigCodec::buildTopic(config, "AABB", "raw", topic, sizeof(topic)));
  EXPECT_STREQ(topic, "local/region-token/AABB/raw");
  char short_topic[8];
  EXPECT_FALSE(MQTTObserverConfigCodec::buildTopic(config, "AABB", "packets", short_topic,
                                                    sizeof(short_topic)));
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

TEST(MQTTObserverQueuePolicy, ExpiresStaleEventsWrapSafely) {
  EXPECT_FALSE(MQTTObserverQueuePolicy::eventExpired(10000, 1000));
  EXPECT_TRUE(MQTTObserverQueuePolicy::eventExpired(11001, 1000));
  EXPECT_FALSE(MQTTObserverQueuePolicy::eventExpired(5, 0xfffffff0));
}

TEST(MQTTObserverLiveQueue, DropsDisconnectedEventsAndKeepsNewestOnOverflow) {
  struct Event { int id; };
  typedef MQTTObserverLiveQueue<Event, 2> EventQueue;
  EventQueue queue;
  Event one = {1}, two = {2}, three = {3};
  EXPECT_EQ(queue.offer(false, one), EventQueue::DROPPED_DISCONNECTED);
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_EQ(queue.offer(true, one), EventQueue::QUEUED);
  EXPECT_EQ(queue.offer(true, two), EventQueue::QUEUED);
  EXPECT_EQ(queue.offer(true, three), EventQueue::DROPPED_OLDEST);
  Event event;
  ASSERT_TRUE(queue.pop(event));
  EXPECT_EQ(event.id, 2);
  ASSERT_TRUE(queue.pop(event));
  EXPECT_EQ(event.id, 3);
  EXPECT_FALSE(queue.pop(event));
  Event four = {4};
  queue.offer(true, four);
  queue.clear();
  EXPECT_EQ(queue.size(), 0u);
  EXPECT_FALSE(queue.pop(event));
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
