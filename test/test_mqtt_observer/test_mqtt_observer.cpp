#include <gtest/gtest.h>

#include <string>
#include "MQTTObserverCli.h"
#include "MQTTObserverConfig.h"
#include "MQTTObserverFormat.h"
#include "MQTTObserverPolicy.h"
#include "MQTTObserverRecord.h"
#include "PersistUtil.h"

// The CLI surface: the observer's commands and their replies, driven the way
// the firmware drives them (MQTT_OBSERVER.md is the spec for these strings).
struct ObserverCliTest : public ::testing::Test {
  MQTTObserverConfig config;
  MQTTObserverCliFacts facts;
  std::string reply;
  bool changed = false;

  void SetUp() override {
    MQTTObserverConfigCodec::setDefaults(config);
    facts.wifi_up = true;
    facts.clock_valid = true;
    facts.wifi_ip[0] = 192; facts.wifi_ip[1] = 168; facts.wifi_ip[2] = 1; facts.wifi_ip[3] = 42;
    facts.wifi_rssi = -55;
    facts.free_heap = 118032;
    facts.max_heap = 98304;
    facts.queue_capacity = 8;
    facts.slot_state[0] = STATE_CONNECTED;
    facts.slot_connected[0] = true;
  }

  // run one command; returns true when the command was the observer's
  bool run(const std::string& command) {
    reply.assign(200, '\0');
    changed = false;
    return mqttObserverCliCommand(config, "Roof Repeater", facts, command.c_str(),
                                  &reply[0], &changed);
  }
  std::string said() const { return std::string(reply.c_str()); }
};

TEST_F(ObserverCliTest, SharedSettersAnswerAndStore) {
  EXPECT_TRUE(run("set mqtt.iata sea"));
  EXPECT_EQ(said(), "OK - MQTT setting saved");
  EXPECT_STREQ(config.iata, "SEA");   // auto-uppercased
  EXPECT_TRUE(changed);

  EXPECT_TRUE(run("set mqtt.origin Shed"));
  EXPECT_STREQ(config.origin, "Shed");
  EXPECT_TRUE(run("set mqtt.interval 12"));
  EXPECT_EQ(config.status_interval_minutes, 12);
  EXPECT_TRUE(run("set mqtt.rx on"));
  EXPECT_EQ(config.rx_enabled, 1);
  EXPECT_TRUE(run("set mqtt.tx advert"));
  EXPECT_EQ(config.tx_enabled, 2);
  EXPECT_TRUE(run("set mqtt.ntp none"));
  EXPECT_STREQ(config.ntp_server, "");   // none restores the default
}

TEST_F(ObserverCliTest, SharedSettersRefuseBadValues) {
  EXPECT_TRUE(run("set mqtt.iata x"));
  EXPECT_EQ(said(), "Err - IATA must be 3 letters/digits; XXX is reserved");
  EXPECT_TRUE(run("set mqtt.iata XXXX"));
  EXPECT_EQ(said(), "Err - IATA must be 3 letters/digits; XXX is reserved");
  EXPECT_TRUE(run("set mqtt.interval 0"));
  EXPECT_EQ(said(), "Err - interval must be 1-60 minutes");
  EXPECT_TRUE(run("set mqtt.interval 61"));
  EXPECT_EQ(said(), "Err - interval must be 1-60 minutes");
  EXPECT_TRUE(run("set mqtt.tx sometimes"));
  EXPECT_EQ(said(), "Err - tx must be off, on, or advert");
  EXPECT_TRUE(run("set mqtt.rx maybe"));
  EXPECT_EQ(said(), "Err - expected on or off");
  EXPECT_TRUE(run("set mqtt.ntp not a host"));
  EXPECT_EQ(said(), "Err - invalid NTP server");
  EXPECT_FALSE(changed);
}

TEST_F(ObserverCliTest, PerBrokerSettersAnswerAndStore) {
  EXPECT_TRUE(run("set mqtt1.enabled on"));
  EXPECT_EQ(said(), "OK - MQTT1 enabled");
  EXPECT_EQ(config.brokers[0].enabled, 1);

  EXPECT_TRUE(run("set mqtt1.server wss://broker.example:443/mqtt"));
  EXPECT_EQ(said(), "OK - MQTT1 setting saved");
  EXPECT_STREQ(config.brokers[0].server, "wss://broker.example:443/mqtt");

  EXPECT_TRUE(run("set mqtt1.port 8883"));
  EXPECT_EQ(said(), "OK - MQTT1 port saved");
  EXPECT_EQ(config.brokers[0].port, 8883);

  EXPECT_TRUE(run("set mqtt1.password hunter2"));
  EXPECT_EQ(said(), "OK - credential saved");
  EXPECT_STREQ(config.brokers[0].password, "hunter2");

  EXPECT_TRUE(run("set mqtt1.audience mqtt.example"));
  EXPECT_STREQ(config.brokers[0].audience, "mqtt.example");
}

TEST_F(ObserverCliTest, PerBrokerSettersRefuseBadValues) {
  EXPECT_TRUE(run("set mqtt1.port 0"));
  EXPECT_EQ(said(), "Err - port must be 1-65535");
  EXPECT_TRUE(run("set mqtt1.port 70000"));
  EXPECT_EQ(said(), "Err - port must be 1-65535");
  EXPECT_TRUE(run("set mqtt1.server not a url"));
  EXPECT_EQ(said(), "Err - invalid or oversized MQTT setting");
  EXPECT_TRUE(run("set mqtt1.enabled maybe"));
  EXPECT_EQ(said(), "Err - expected on or off");
  EXPECT_FALSE(changed);
}

TEST_F(ObserverCliTest, TopicTemplateRefusesUnknownPlaceholders) {
  EXPECT_TRUE(run("set mqtt1.topic meshcore/{iata}/{device}/{type}"));
  EXPECT_EQ(said(), "OK - MQTT1 setting saved");
  EXPECT_STREQ(config.brokers[0].topic, "meshcore/{iata}/{device}/{type}");

  // a typo would publish the whole feed to a topic nobody reads
  EXPECT_TRUE(run("set mqtt1.topic meshcore/{iataa}/{device}"));
  EXPECT_EQ(said(), "Err - topic placeholder must be {iata}, {device}, {token} or {type}");
  EXPECT_STREQ(config.brokers[0].topic, "meshcore/{iata}/{device}/{type}");   // unchanged
  EXPECT_FALSE(changed);
}

TEST_F(ObserverCliTest, WifiSettersAndGetters) {
  EXPECT_TRUE(run("set wifi.ssid mesh-net"));
  EXPECT_EQ(said(), "OK - WiFi SSID saved");
  EXPECT_STREQ(config.wifi_ssid, "mesh-net");
  EXPECT_TRUE(run("set wifi.pwd secret"));
  EXPECT_EQ(said(), "OK - credential saved");
  EXPECT_STREQ(config.wifi_password, "secret");
  EXPECT_TRUE(run("set wifi.powersave max"));
  EXPECT_EQ(said(), "OK - WiFi power save saved");
  EXPECT_EQ(config.wifi_power_save, 2);
  EXPECT_TRUE(run("set wifi.powersave sometimes"));
  EXPECT_EQ(said(), "Err - expected none, min, or max");

  EXPECT_TRUE(run("get wifi.ssid"));
  EXPECT_EQ(said(), "mesh-net");
  EXPECT_TRUE(run("get wifi.pwd"));
  EXPECT_EQ(said(), "secret");
  EXPECT_TRUE(run("get wifi.powersave"));
  EXPECT_EQ(said(), "max");
  EXPECT_TRUE(run("get wifi.status"));
  EXPECT_EQ(said(), "wifi connected ip:192.168.1.42 rssi:-55");
}

TEST_F(ObserverCliTest, SharedGettersReportTheConfig) {
  EXPECT_TRUE(run("set mqtt.iata SEA"));
  EXPECT_TRUE(run("set mqtt.origin Shed"));
  EXPECT_TRUE(run("set mqtt.interval 7"));
  EXPECT_TRUE(run("set mqtt.rx on"));
  EXPECT_TRUE(run("set mqtt.tx advert"));

  EXPECT_TRUE(run("get mqtt.iata"));
  EXPECT_EQ(said(), "SEA");
  EXPECT_TRUE(run("get mqtt.origin"));
  EXPECT_EQ(said(), "Shed");
  EXPECT_TRUE(run("get mqtt.ntp"));
  EXPECT_EQ(said(), "pool.ntp.org");
  EXPECT_TRUE(run("get mqtt.interval"));
  EXPECT_EQ(said(), "7");
  EXPECT_TRUE(run("get mqtt.rx"));
  EXPECT_EQ(said(), "on");
  EXPECT_TRUE(run("get mqtt.status.enabled"));
  EXPECT_EQ(said(), "on");
  EXPECT_TRUE(run("get mqtt.tx"));
  EXPECT_EQ(said(), "advert");
  // the origin falls back to the node name
  EXPECT_TRUE(run("set mqtt.origin "));
  EXPECT_TRUE(run("get mqtt.origin"));
  EXPECT_EQ(said(), "Roof Repeater");
}

TEST_F(ObserverCliTest, PerBrokerGettersReportTheConfig) {
  EXPECT_TRUE(run("set mqtt1.server wss://broker.example:443/mqtt"));
  EXPECT_TRUE(run("set mqtt1.port 8883"));
  EXPECT_TRUE(run("set mqtt1.username user"));
  EXPECT_TRUE(run("set mqtt1.enabled on"));

  EXPECT_TRUE(run("get mqtt1.server"));
  EXPECT_EQ(said(), "broker.example:443");
  EXPECT_TRUE(run("get mqtt1.port"));
  EXPECT_EQ(said(), "443");   // the effective port: the server URL carries it
  EXPECT_TRUE(run("get mqtt1.username"));
  EXPECT_EQ(said(), "user");
  EXPECT_TRUE(run("get mqtt1.password"));
  EXPECT_EQ(said(), "not set");
  EXPECT_TRUE(run("get mqtt1.audience"));
  EXPECT_EQ(said(), "not configured");
  EXPECT_TRUE(run("get mqtt1.topic"));
  EXPECT_EQ(said(), "meshcore/{iata}/{device}/{type}");
  EXPECT_TRUE(run("get mqtt1.enabled"));
  EXPECT_EQ(said(), "on");
}

TEST_F(ObserverCliTest, SlotStatusAndDashboard) {
  EXPECT_TRUE(run("get mqtt1.status"));
  EXPECT_EQ(said(), "mqtt1 connected wifi:up q:0");

  facts.slot_state[1] = STATE_ERROR;
  facts.queue_size = 3;
  EXPECT_TRUE(run("get mqtt2.status"));
  EXPECT_EQ(said(), "mqtt2 error wifi:up q:3");

  // the dashboard counts the configured slots, not the build-time maximum
  EXPECT_TRUE(run("set mqtt1.enabled on"));
  facts.published = 12;
  facts.received = 34;
  facts.publish_drops = 2;
  facts.queue_size = 0;
  EXPECT_TRUE(run("get mqtt.status"));
  EXPECT_EQ(said(), "mqtt wifi:up time:ok slots:1/1 rx:34 pub:12 drop:2 q:0/8 heap:118032/98304");
}

TEST_F(ObserverCliTest, SlotDiagReportsTheLastBrokerError) {
  // A healthy slot that has never had an error still answers with a stable
  // field layout (parsers should not have to special-case the first run).
  EXPECT_TRUE(run("get mqtt1.diag"));
  EXPECT_EQ(said(),
            "mqtt1 diag connected err:none tls:0x0 stk:0x0 cert:0x0 sock:0 "
            "att:0 fail:0 age:0s up:0s retry:0s");

  facts.slot_state[1] = STATE_ERROR;
  facts.slot_connected[1] = false;
  MQTTObserverSlotDiag& diag = facts.slot_diag[1];
  diag.error_type = DIAG_ERR_TRANSPORT;
  diag.tls_stack = -29184;
  diag.tls_flags = 0x8000;
  diag.sock_errno = 111;
  diag.attempts = 7;
  diag.failures = 3;
  diag.error_age_s = 42;
  diag.retry_in_s = 18;
  EXPECT_TRUE(run("get mqtt2.diag"));
  EXPECT_EQ(said(),
            "mqtt2 diag error err:transport tls:0x0 stk:0xFFFF8E00 cert:0x8000 "
            "sock:111 att:7 fail:3 age:42s up:0s retry:18s");
  EXPECT_LT(said().size(), static_cast<size_t>(CLI_REPLY_MAX));

  // The observer's own outcomes have no transport detail of their own.
  diag.error_type = DIAG_ERR_TIMEOUT;
  diag.tls_stack = 0;
  diag.tls_flags = 0;
  diag.sock_errno = 0;
  EXPECT_TRUE(run("get mqtt2.diag"));
  EXPECT_EQ(said(),
            "mqtt2 diag error err:timeout tls:0x0 stk:0x0 cert:0x0 sock:0 "
            "att:7 fail:3 age:42s up:0s retry:18s");

  diag.error_type = DIAG_ERR_CONFIG;
  EXPECT_TRUE(run("get mqtt2.diag"));
  EXPECT_EQ(said(),
            "mqtt2 diag error err:config tls:0x0 stk:0x0 cert:0x0 sock:0 "
            "att:7 fail:3 age:42s up:0s retry:18s");

  diag.error_type = DIAG_ERR_LIVENESS;
  EXPECT_TRUE(run("get mqtt2.diag"));
  EXPECT_EQ(said(),
            "mqtt2 diag error err:liveness tls:0x0 stk:0x0 cert:0x0 sock:0 "
            "att:7 fail:3 age:42s up:0s retry:18s");

  diag.error_type = DIAG_ERR_CLOSED;
  EXPECT_TRUE(run("get mqtt2.diag"));
  EXPECT_EQ(said(),
            "mqtt2 diag error err:closed tls:0x0 stk:0x0 cert:0x0 sock:0 "
            "att:7 fail:3 age:42s up:0s retry:18s");
}

TEST_F(ObserverCliTest, ForeignCommandsAreNotOurs) {
  EXPECT_FALSE(run("filter stats"));
  EXPECT_FALSE(run("battery"));
  EXPECT_FALSE(run("get mqtt9.status"));   // there is no mqtt3
  EXPECT_FALSE(run("get mqtt1.nosuchthing"));
  // an unsupported set is still the observer's to answer
  EXPECT_TRUE(run("set mqtt1.nosuchthing x"));
  EXPECT_EQ(said(), "Err - unsupported MQTT/WiFi setting");
}

TEST_F(ObserverCliTest, StateNames) {
  EXPECT_STREQ(mqttObserverStateName(STATE_OFF), "off");
  EXPECT_STREQ(mqttObserverStateName(STATE_CONFIG), "config");
  EXPECT_STREQ(mqttObserverStateName(STATE_WIFI), "wifi");
  EXPECT_STREQ(mqttObserverStateName(STATE_TIME), "time");
  EXPECT_STREQ(mqttObserverStateName(STATE_CONNECTING), "connecting");
  EXPECT_STREQ(mqttObserverStateName(STATE_CONNECTED), "connected");
  EXPECT_STREQ(mqttObserverStateName(STATE_RETRY), "retry");
  EXPECT_STREQ(mqttObserverStateName(STATE_ERROR), "error");
}

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
  config.tx_enabled = 1;
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

  EXPECT_EQ(MQTT_OBSERVER_CONFIG_BROKER_CAPACITY, 2u);
  EXPECT_EQ(MQTTObserverConfigCodec::kVersion, 5);
  EXPECT_EQ(MQTTObserverConfigCodec::kPayloadSize, 1509u);
  EXPECT_EQ(MQTTObserverRecord::kRecordSize, 1518u);
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
  EXPECT_EQ(decoded.tx_enabled, 1);
  EXPECT_EQ(decoded.brokers[0].enabled, 1);
  EXPECT_STREQ(decoded.brokers[0].server, "wss://mqtt.example:443/mqtt");
  EXPECT_STREQ(decoded.brokers[0].password, "broker-secret");
  EXPECT_STREQ(decoded.brokers[0].audience, "mqtt.example");
  EXPECT_STREQ(decoded.brokers[0].owner, config.brokers[0].owner);
  EXPECT_STREQ(decoded.brokers[0].email, "operator@example.org");
  EXPECT_EQ(decoded.brokers[0].port, 8883);
  EXPECT_STREQ(decoded.brokers[1].server, "mqtt://second.example");
  EXPECT_STREQ(decoded.brokers[1].username, "second-user");
}

TEST(MQTTObserverConfig, RejectsInvalidValuesAndIntegrityChanges) {
  MQTTObserverConfig config;
  uint8_t payload[MQTTObserverConfigCodec::kPayloadSize];
  MQTTObserverConfigCodec::setDefaults(config);
  strcpy(config.brokers[0].server, "https://mqtt.example");
  EXPECT_EQ(MQTTObserverConfigCodec::encode(config, payload, sizeof(payload)), 0u);

  strcpy(config.brokers[0].server, "mqtts://mqtt.example");
  strcpy(config.iata, "Sea");
  EXPECT_EQ(MQTTObserverConfigCodec::encode(config, payload, sizeof(payload)), 0u);

  strcpy(config.iata, "SEA");
  ASSERT_EQ(MQTTObserverConfigCodec::encode(config, payload, sizeof(payload)), sizeof(payload));
  uint16_t checksum = crc16_ccitt(payload, sizeof(payload));
  payload[0] = 2;
  EXPECT_NE(crc16_ccitt(payload, sizeof(payload)), checksum);
  EXPECT_FALSE(MQTTObserverConfigCodec::decode(payload, sizeof(payload), config));
  EXPECT_EQ(config.status_enabled, 1);
  EXPECT_FALSE(MQTTObserverConfigCodec::decode(payload, sizeof(payload) - 1, config));
}

TEST(MQTTObserverConfig, TxDefaultsToAdvertAndRejectsUnknownModes) {
  MQTTObserverConfig config;
  MQTTObserverConfigCodec::setDefaults(config);
  EXPECT_EQ(config.tx_enabled, 2);   // own adverts by default
  uint8_t payload[MQTTObserverConfigCodec::kPayloadSize];
  ASSERT_EQ(MQTTObserverConfigCodec::encode(config, payload, sizeof(payload)), sizeof(payload));
  config.tx_enabled = 3;
  EXPECT_EQ(MQTTObserverConfigCodec::encode(config, payload, sizeof(payload)), 0u);
  // decoding an out-of-range mode falls back to defaults rather than trusting it
  config.tx_enabled = 2;
  ASSERT_EQ(MQTTObserverConfigCodec::encode(config, payload, sizeof(payload)), sizeof(payload));
  payload[4] = 9;   // tx_enabled sits after status/packets/raw/rx
  EXPECT_FALSE(MQTTObserverConfigCodec::decode(payload, sizeof(payload), config));
  EXPECT_EQ(config.tx_enabled, 2);   // defaults restored
}

TEST(MQTTObserverRecord, ChecksVersionLengthAndChecksum) {
  MQTTObserverConfig original;
  MQTTObserverConfigCodec::setDefaults(original);
  original.brokers[0].enabled = 1;
  strcpy(original.brokers[0].server, "mqtts://broker.example");
  original.brokers[1].enabled = 1;
  strcpy(original.brokers[1].server, "mqtt://second.example");
  strcpy(original.iata, "SEA");
  uint8_t record[MQTTObserverRecord::kRecordSize];
  ASSERT_EQ(MQTTObserverRecord::encode(original, record, sizeof(record)), sizeof(record));

  MQTTObserverConfig decoded;
  ASSERT_TRUE(MQTTObserverRecord::decode(record, sizeof(record), decoded));
  EXPECT_STREQ(decoded.brokers[0].server, original.brokers[0].server);
  EXPECT_STREQ(decoded.brokers[1].server, original.brokers[1].server);
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

  strcpy(config.brokers[0].server, "mqtts://[2001:db8::1]/mqtt");
  ASSERT_TRUE(MQTTObserverConfigCodec::buildServerUri(config.brokers[0], uri, sizeof(uri)));
  EXPECT_STREQ(uri, "mqtts://[2001:db8::1]:8883/mqtt");
  EXPECT_EQ(MQTTObserverConfigCodec::configuredPort(config.brokers[0]), 8883);

  strcpy(config.brokers[0].server, "ws://[::1]:8080/mqtt");
  ASSERT_TRUE(MQTTObserverConfigCodec::buildServerUri(config.brokers[0], uri, sizeof(uri)));
  EXPECT_STREQ(uri, "ws://[::1]:8080/mqtt");
  EXPECT_EQ(MQTTObserverConfigCodec::configuredPort(config.brokers[0]), 8080);
}

TEST(MQTTObserverConfig, RejectsMalformedAuthoritiesAndPorts) {
  const char* invalid[] = {
    "mqtt://host:", "mqtt://host:0", "mqtt://host:65536", "mqtt://host:abc",
    "mqtt://host:12345678901234567890", "mqtt://host:12:34", "mqtt://[::1",
    "mqtt://[]", "mqtt://[::1]extra", "mqtt://host\\other", "host:1883",
    "mqtt://user:password@host", "mqtt://?query"
  };
  for (const char* server : invalid)
    EXPECT_FALSE(MQTTObserverConfigCodec::validServer(server)) << server;
  EXPECT_TRUE(MQTTObserverConfigCodec::validServer("mqtt://[::1]:1883"));
  EXPECT_TRUE(MQTTObserverConfigCodec::validServer("[2001:db8::1]"));
  EXPECT_TRUE(MQTTObserverConfigCodec::validServer("wss://host:443/mqtt"));
}

TEST(MQTTObserverConfig, BrokerSlotsHaveDistinctClientIds) {
  char first[67], second[67];
  const char* device = "0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF0123456789ABCDEF";
  ASSERT_TRUE(MQTTObserverConfigCodec::buildClientId(device, 0, first, sizeof(first)));
  ASSERT_TRUE(MQTTObserverConfigCodec::buildClientId(device, 1, second, sizeof(second)));
  EXPECT_EQ(std::string(first), std::string(device) + "-1");
  EXPECT_EQ(std::string(second), std::string(device) + "-2");
  EXPECT_FALSE(MQTTObserverConfigCodec::buildClientId(device, 2, first, sizeof(first)));
  EXPECT_FALSE(MQTTObserverConfigCodec::buildClientId(device, 0, first, sizeof(first) - 1));
}

TEST(MQTTObserverConnectionPolicy, RenewsBeforeReconnectAsWellAsWhileConnected) {
  EXPECT_FALSE(MQTTObserverConnectionPolicy::jwtNeedsRenewal(1000, 0));
  EXPECT_FALSE(MQTTObserverConnectionPolicy::jwtNeedsRenewal(1000, 1301));
  EXPECT_TRUE(MQTTObserverConnectionPolicy::jwtNeedsRenewal(1000, 1300));
  EXPECT_TRUE(MQTTObserverConnectionPolicy::jwtNeedsRenewal(2000, 1300));
}

TEST(MQTTObserverConnectionPolicy, RequiresHeadroomInThePoolThatServesTls) {
  using namespace MQTTObserverConnectionPolicy;
  // Without PSRAM the secure thresholds apply to the internal heap.
  EXPECT_TRUE(hasConnectHeadroom(true, false, kSecureFreeBytes, kSecureLargestBytes, 0, 0));
  EXPECT_FALSE(hasConnectHeadroom(true, false, kSecureFreeBytes - 1, kSecureLargestBytes, 0, 0));
  EXPECT_FALSE(hasConnectHeadroom(true, false, kSecureFreeBytes, kSecureLargestBytes - 1, 0, 0));
  // With PSRAM they apply to PSRAM; internal only needs the plaintext floor.
  EXPECT_TRUE(hasConnectHeadroom(true, true, kPlainFreeInternalBytes, kPlainLargestInternalBytes,
                                 kSecureFreeBytes, kSecureLargestBytes));
  EXPECT_FALSE(hasConnectHeadroom(true, true, kPlainFreeInternalBytes - 1, kPlainLargestInternalBytes,
                                  kSecureFreeBytes, kSecureLargestBytes));
  EXPECT_FALSE(hasConnectHeadroom(true, true, kPlainFreeInternalBytes, kPlainLargestInternalBytes - 1,
                                  kSecureFreeBytes, kSecureLargestBytes));
  EXPECT_FALSE(hasConnectHeadroom(true, true, kPlainFreeInternalBytes, kPlainLargestInternalBytes,
                                  kSecureFreeBytes - 1, kSecureLargestBytes));
  EXPECT_FALSE(hasConnectHeadroom(true, true, kPlainFreeInternalBytes, kPlainLargestInternalBytes,
                                  kSecureFreeBytes, kSecureLargestBytes - 1));
  // Plaintext brokers always use the internal heap, even on a PSRAM board.
  EXPECT_TRUE(hasConnectHeadroom(false, false, kSecureFreeBytes, kSecureLargestBytes, 0, 0));
  EXPECT_TRUE(hasConnectHeadroom(false, true, kPlainFreeInternalBytes, kPlainLargestInternalBytes, 0, 0));
  EXPECT_FALSE(hasConnectHeadroom(false, true, kPlainFreeInternalBytes - 1, kPlainLargestInternalBytes, 0, 0));
  EXPECT_FALSE(hasConnectHeadroom(false, true, kPlainFreeInternalBytes, kPlainLargestInternalBytes - 1, 0, 0));
  // An empty heap never admits a client.
  EXPECT_FALSE(hasConnectHeadroom(true, false, 0, 0, 0, 0));
  EXPECT_FALSE(hasConnectHeadroom(true, true, 0, 0, 0, 0));
  EXPECT_FALSE(hasConnectHeadroom(true, true, kPlainFreeInternalBytes, kPlainLargestInternalBytes, 0, 0));
  EXPECT_FALSE(hasConnectHeadroom(false, false, 0, 0, 0, 0));
}

TEST(MQTTObserverConnectionPolicy, PromotesAtMostOnePacketProbePerInterval) {
  using namespace MQTTObserverConnectionPolicy;
  const uint32_t kIntervalMs = 5u * 60000u;
  // Status is the probe while it is enabled: never promote packets.
  EXPECT_FALSE(livenessProbeDue(true, 0, 1000, 0, kIntervalMs));
  // One probe at a time; the next packet is QoS 0 while one is in flight.
  EXPECT_FALSE(livenessProbeDue(false, 42, 1000, 0, kIntervalMs));
  // A fresh connect probes on its first publish.
  EXPECT_TRUE(livenessProbeDue(false, 0, 1000, 0, kIntervalMs));
  // Afterwards the cadence is mqtt.interval, wrap-safe.
  EXPECT_FALSE(livenessProbeDue(false, 0, kIntervalMs, 1000, kIntervalMs));
  EXPECT_TRUE(livenessProbeDue(false, 0, kIntervalMs + 1000, 1000, kIntervalMs));
  EXPECT_TRUE(livenessProbeDue(false, 0, 100, 200, kIntervalMs));
}

TEST(MQTTObserverConnectionPolicy, TimesOutAnUnacknowledgedPacketProbe) {
  using namespace MQTTObserverConnectionPolicy;
  const uint32_t kTimeoutMs = kLivenessProbeTimeoutMs;
  EXPECT_FALSE(livenessProbeTimedOut(0, 1000, 1000));
  EXPECT_FALSE(livenessProbeTimedOut(42, 1000, 0));
  EXPECT_FALSE(livenessProbeTimedOut(42, 1000 + kTimeoutMs - 1, 1000));
  EXPECT_TRUE(livenessProbeTimedOut(42, 1000 + kTimeoutMs, 1000));
  EXPECT_TRUE(livenessProbeTimedOut(42, 100, 200));
}

TEST(MQTTObserverConnectionPolicy, AConnectAttemptThatNeverCameUpIsAFailure) {
  using namespace MQTTObserverConnectionPolicy;
  // no attempt in flight: nothing to time out
  EXPECT_FALSE(connectAttemptTimedOut(false, 1000 + kConnectAttemptTimeoutMs, 1000));
  // the attempt is still young
  EXPECT_FALSE(connectAttemptTimedOut(true, 1000 + kConnectAttemptTimeoutMs - 1, 1000));
  // it never came up
  EXPECT_TRUE(connectAttemptTimedOut(true, 1000 + kConnectAttemptTimeoutMs, 1000));
  // wrap-safe: the attempt started just before the rollover
  EXPECT_TRUE(connectAttemptTimedOut(true, 100, 200));
}

TEST(MQTTObserverConnectionPolicy, ClassifiesConnectFailureSources) {
  using namespace MQTTObserverConnectionPolicy;
  EXPECT_EQ(attemptFailureSource(1000, 0, 0), AttemptFailureSource::Timeout);
  EXPECT_EQ(attemptFailureSource(1000, 1500, 0), AttemptFailureSource::Reported);
  EXPECT_EQ(attemptFailureSource(1000, 0, 1500), AttemptFailureSource::Closed);
  // a transport error outranks the disconnect it caused
  EXPECT_EQ(attemptFailureSource(1000, 1500, 1600), AttemptFailureSource::Reported);
  // timestamps older than the attempt belong to its predecessor
  EXPECT_EQ(attemptFailureSource(1000, 900, 0), AttemptFailureSource::Timeout);
  EXPECT_EQ(attemptFailureSource(1000, 0, 900), AttemptFailureSource::Timeout);
  // wrap-safe: both stamps are after the attempt in millis() terms
  EXPECT_EQ(attemptFailureSource(0xFFFFFF00u, 0x00000100u, 0),
            AttemptFailureSource::Reported);
}

TEST(MQTTObserverConnectionPolicy, DisconnectBelongsToARecentTransportError) {
  using namespace MQTTObserverConnectionPolicy;
  EXPECT_FALSE(disconnectBelongsToError(10000, 0));
  EXPECT_TRUE(disconnectBelongsToError(10000, 10000));
  EXPECT_TRUE(disconnectBelongsToError(10000, 10000 - kDisconnectAfterErrorMs));
  EXPECT_FALSE(disconnectBelongsToError(10000,
                                        10000 - kDisconnectAfterErrorMs - 1));
}

TEST(MQTTObserverProbeTracker, ClearsTheProbeOnItsPuback) {
  MQTTObserverProbeTracker tracker;
  tracker.noteProbe(42, 1000);
  EXPECT_EQ(tracker.outstanding(), 42);
  EXPECT_EQ(tracker.sentMs(), 1000u);
  EXPECT_EQ(tracker.lastProbeMs(), 1000u);
  tracker.notePuback(42);
  EXPECT_EQ(tracker.outstanding(), 0);
  EXPECT_EQ(tracker.sentMs(), 0u);
}

TEST(MQTTObserverProbeTracker, AProbeAcknowledgedBeforeItWasRecordedIsNotOutstanding) {
  // The broker can answer while publish() is still returning: the PUBACK
  // arrives before the id is recorded. Recording it anyway would leave a probe
  // outstanding that nothing clears, and the liveness timeout would force a
  // reconnect every 60 s.
  MQTTObserverProbeTracker tracker;
  tracker.notePuback(7);        // the ack landed first
  tracker.noteProbe(7, 1000);
  EXPECT_EQ(tracker.outstanding(), 0);
  EXPECT_EQ(tracker.sentMs(), 0u);
  EXPECT_FALSE(MQTTObserverConnectionPolicy::livenessProbeTimedOut(
      tracker.outstanding(), 1000 + MQTTObserverConnectionPolicy::kLivenessProbeTimeoutMs,
      tracker.sentMs()));
}

TEST(MQTTObserverProbeTracker, OnlyOneProbeIsOutstandingAtATime) {
  MQTTObserverProbeTracker tracker;
  tracker.noteProbe(1, 1000);
  tracker.noteProbe(2, 1100);   // a second probe while one is in flight
  EXPECT_EQ(tracker.outstanding(), 1);
  EXPECT_EQ(tracker.sentMs(), 1000u);
  // a PUBACK for another id does not clear the outstanding probe
  tracker.notePuback(99);
  EXPECT_EQ(tracker.outstanding(), 1);
  EXPECT_EQ(tracker.sentMs(), 1000u);
}

TEST(MQTTObserverProbeTracker, ANewConnectionForgetsTheOldPacketIds) {
  MQTTObserverProbeTracker tracker;
  tracker.noteProbe(5, 1000);
  tracker.reset();
  EXPECT_EQ(tracker.outstanding(), 0);
  EXPECT_EQ(tracker.sentMs(), 0u);
  // the ids restart on a new connection: an id from the old session must not
  // be mistaken for an acknowledgement
  tracker.noteProbe(5, 2000);
  EXPECT_EQ(tracker.outstanding(), 5);
  EXPECT_EQ(tracker.sentMs(), 2000u);
}

TEST(MQTTObserverQueuePolicy, RestartsStaleWindowAfterFlushAndHandlesWrap) {
  uint32_t since = 0;
  EXPECT_FALSE(MQTTObserverQueuePolicy::trackDisconnected(100, false, since));
  EXPECT_EQ(since, 100u);
  EXPECT_TRUE(MQTTObserverQueuePolicy::trackDisconnected(60100, false, since));
  EXPECT_EQ(since, 0u);
  EXPECT_FALSE(MQTTObserverQueuePolicy::trackDisconnected(60101, false, since));
  EXPECT_FALSE(MQTTObserverQueuePolicy::trackDisconnected(60102, false, since));
  EXPECT_FALSE(MQTTObserverQueuePolicy::trackDisconnected(60103, true, since));
  EXPECT_EQ(since, 0u);
  EXPECT_FALSE(MQTTObserverQueuePolicy::trackDisconnected(0xfffffff0, false, since));
  EXPECT_TRUE(MQTTObserverQueuePolicy::trackDisconnected(59984, false, since));
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
  uint8_t copied[MQTTObserverRawStager::kCapacity];
  uint16_t copied_len = 0;
  float copied_snr = 0, copied_rssi = 0;

  stager.stage(first, sizeof(first), -4.5f, -91.0f);
  ASSERT_TRUE(stager.consume(copied, sizeof(copied), copied_len, copied_snr, copied_rssi));
  EXPECT_EQ(copied_len, sizeof(first));
  EXPECT_EQ(copied[2], 3);
  EXPECT_FLOAT_EQ(copied_snr, -4.5f);
  EXPECT_FLOAT_EQ(copied_rssi, -91.0f);
  EXPECT_FALSE(stager.consume(copied, sizeof(copied), copied_len, copied_snr, copied_rssi));

  uint8_t second[] = {9};
  stager.stage(second, sizeof(second), 2.0f, -40.0f);
  stager.stage(nullptr, 0, 0.0f, 0.0f);
  EXPECT_FALSE(stager.consume(copied, sizeof(copied), copied_len, copied_snr, copied_rssi));

  stager.stage(first, sizeof(first), 1.5f, -80.0f);
  ASSERT_TRUE(stager.consume(copied, sizeof(copied), copied_len, copied_snr, copied_rssi));
  EXPECT_EQ(copied[0], 1);
  EXPECT_FLOAT_EQ(copied_snr, 1.5f);
  EXPECT_FLOAT_EQ(copied_rssi, -80.0f);

  stager.stage(first, sizeof(first), 1.5f, -80.0f);
  uint8_t too_small[sizeof(first) - 1];
  EXPECT_FALSE(stager.consume(too_small, sizeof(too_small), copied_len, copied_snr, copied_rssi));
  EXPECT_EQ(copied_len, 0);
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
    "D", 3, -4.5f, -91.0f, 1.234f, hash, sizeof(hash), path, 2, 1, false
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

TEST(MQTTObserverFormat, TxPacketOmitsReceiveMeasurements) {
  const uint8_t wire[] = {0xa5, 0x00, 0xff};
  const uint8_t hash[] = {0xab, 0xcd};
  MQTTObserverFormat::PacketView packet = {
    "node", "001122", 0, 0, wire, sizeof(wire), 4,
    "F", 3, 0.0f, 0.0f, 0.0f, hash, sizeof(hash), nullptr, 0, 0, true
  };
  char json[1024];
  size_t written = 0;
  ASSERT_TRUE(MQTTObserverFormat::buildPacket(packet, json, sizeof(json), written));
  std::string payload(json, written);
  EXPECT_NE(payload.find("\"direction\":\"tx\""), std::string::npos);
  // A transmission has no reception measurements to report.
  EXPECT_EQ(payload.find("\"SNR\""), std::string::npos);
  EXPECT_EQ(payload.find("\"RSSI\""), std::string::npos);
  EXPECT_EQ(payload.find("\"score\""), std::string::npos);
  EXPECT_EQ(payload.find("\"path\""), std::string::npos);
  // The wire frame and identity are still present.
  EXPECT_NE(payload.find("\"raw\":\"A500FF\""), std::string::npos);
  EXPECT_NE(payload.find("\"origin_id\":\"001122\""), std::string::npos);
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
  MQTTObserverFormat::StatusStats stats{3900, 1234, -110, true};
  ASSERT_TRUE(MQTTObserverFormat::buildStatus("node", "AABB", "MeshCore", "1.0", "radio",
                                               "timestamp", stats, json, sizeof(json), written));
  EXPECT_NE(std::string(json, written).find("\"status\":\"online\""), std::string::npos);
  EXPECT_NE(std::string(json, written).find("\"origin_id\":\"AABB\""), std::string::npos);
  EXPECT_NE(std::string(json, written).find("\"model\":\"MeshCore\""), std::string::npos);
  EXPECT_NE(std::string(json, written).find("\"client_version\":\"meshcore-jhuebert/1.0\""), std::string::npos);
  char short_json[8];
  EXPECT_FALSE(MQTTObserverFormat::buildRaw("node", "AABB", "timestamp", raw, sizeof(raw),
                                            short_json, sizeof(short_json), written));
  EXPECT_EQ(short_json[0], 0);
}

TEST(MQTTObserverFormat, StatusReportsRelayAndDeviceStats) {
  char json[512];
  size_t written = 0;
  MQTTObserverFormat::StatusStats stats{0, 0, -95, false};
  ASSERT_TRUE(MQTTObserverFormat::buildStatus("node", "AABB", "Station G2", "1.2",
                                               "910.525,62.5,7,5", "timestamp", stats,
                                               json, sizeof(json), written));
  std::string payload(json, written);
  EXPECT_NE(payload.find("\"model\":\"Station G2\""), std::string::npos);
  EXPECT_NE(payload.find("\"repeat\":false"), std::string::npos);
  EXPECT_NE(payload.find("\"stats\":{\"battery_mv\":0,\"uptime_secs\":0,\"noise_floor\":-95}"),
            std::string::npos);
}

TEST(MQTTObserverFormat, ClientVersionIsSharedByStatusAndClaims) {
  char version[96];
  ASSERT_TRUE(MQTTObserverFormat::buildClientVersion("1.2.3", version, sizeof(version)));
  EXPECT_STREQ(version, "meshcore-jhuebert/1.2.3");
  char tiny[8];
  EXPECT_FALSE(MQTTObserverFormat::buildClientVersion("1.2.3-abcdef", tiny, sizeof(tiny)));
  EXPECT_FALSE(MQTTObserverFormat::buildClientVersion(nullptr, version, sizeof(version)));
}

TEST(MQTTObserverConfig, DefaultsAreEnabledAndSafeForAFreshInstall) {
  MQTTObserverConfig config;
  MQTTObserverConfigCodec::setDefaults(config);
  EXPECT_EQ(config.status_enabled, 1);
  EXPECT_EQ(config.packets_enabled, 1);
  EXPECT_EQ(config.raw_enabled, 1);
  EXPECT_EQ(config.rx_enabled, 1);
  EXPECT_EQ(config.tx_enabled, 2);            // own adverts
  EXPECT_EQ(config.status_interval_minutes, 5);
  EXPECT_EQ(config.wifi_power_save, 1);
  for (size_t i = 0; i < MQTT_OBSERVER_CONFIG_BROKER_CAPACITY; i++) {
    EXPECT_EQ(config.brokers[i].port, 1883);
    EXPECT_EQ(config.brokers[i].enabled, 0);
    EXPECT_EQ(config.brokers[i].server[0], 0);
  }
  EXPECT_TRUE(MQTTObserverConfigCodec::valid(config));
}

TEST(MQTTObserverConfig, TopicTemplateTakesOnlyKnownPlaceholders) {
  // An unknown placeholder would be published verbatim, so the whole feed
  // would go to a topic nobody reads.
  EXPECT_TRUE(MQTTObserverConfigCodec::validTopicTemplate("meshcore/{iata}/{device}/{type}"));
  EXPECT_TRUE(MQTTObserverConfigCodec::validTopicTemplate("meshcore/{token}"));
  EXPECT_TRUE(MQTTObserverConfigCodec::validTopicTemplate("meshcore/plain"));
  EXPECT_FALSE(MQTTObserverConfigCodec::validTopicTemplate("meshcore/{iataa}/{device}"));
  EXPECT_FALSE(MQTTObserverConfigCodec::validTopicTemplate("meshcore/{IATA}"));
  EXPECT_FALSE(MQTTObserverConfigCodec::validTopicTemplate("meshcore/{}"));
  EXPECT_FALSE(MQTTObserverConfigCodec::validTopicTemplate("meshcore/{iata"));
  // the plain topic validator is unchanged: a token is a value, not a template
  EXPECT_TRUE(MQTTObserverConfigCodec::validTopic("token-with-{braces}"));
}

TEST(MQTTObserverConfig, ValidatorsRejectHostileStrings) {
  EXPECT_FALSE(MQTTObserverConfigCodec::validTopic("bad+topic"));
  EXPECT_FALSE(MQTTObserverConfigCodec::validTopic("bad#topic"));
  EXPECT_FALSE(MQTTObserverConfigCodec::validTopic("has space"));
  EXPECT_TRUE(MQTTObserverConfigCodec::validTopic("meshcore/{iata}/{device}/{type}"));

  EXPECT_TRUE(MQTTObserverConfigCodec::validOwner(""));
  EXPECT_TRUE(MQTTObserverConfigCodec::validOwner(
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"));
  EXPECT_FALSE(MQTTObserverConfigCodec::validOwner("0123"));
  EXPECT_FALSE(MQTTObserverConfigCodec::validOwner(
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdeg"));

  EXPECT_TRUE(MQTTObserverConfigCodec::validNtpServer(""));
  EXPECT_TRUE(MQTTObserverConfigCodec::validNtpServer("time.example.com"));
  EXPECT_FALSE(MQTTObserverConfigCodec::validNtpServer("time example"));
  EXPECT_FALSE(MQTTObserverConfigCodec::validNtpServer("time:123"));

  EXPECT_TRUE(MQTTObserverConfigCodec::validAudience("broker.example"));
  EXPECT_FALSE(MQTTObserverConfigCodec::validAudience("bad audience"));

  EXPECT_TRUE(MQTTObserverConfigCodec::validEmail(""));
  EXPECT_FALSE(MQTTObserverConfigCodec::validEmail("line\nbreak"));
}

TEST(MQTTObserverConfig, IataAcceptsEmptyAndRejectsWrongShape) {
  EXPECT_TRUE(MQTTObserverConfigCodec::validIata(""));
  EXPECT_TRUE(MQTTObserverConfigCodec::validIata("AB1"));
  EXPECT_FALSE(MQTTObserverConfigCodec::validIata("abc"));    // lowercase
  EXPECT_FALSE(MQTTObserverConfigCodec::validIata("AB"));     // too short
  EXPECT_FALSE(MQTTObserverConfigCodec::validIata("ABCD"));   // too long
}

TEST(MQTTObserverConfig, TopicTemplateKeepsLiteralTextAndDefaultsWhenEmpty) {
  MQTTObserverConfig config;
  MQTTObserverConfigCodec::setDefaults(config);
  strcpy(config.iata, "SEA");
  char topic[160];
  // No {type}: the template is emitted verbatim.
  strcpy(config.brokers[0].topic, "static/feed");
  ASSERT_TRUE(MQTTObserverConfigCodec::buildTopic(config, config.brokers[0], "AABB",
                                                  "packets", topic, sizeof(topic)));
  EXPECT_STREQ(topic, "static/feed");
  // Empty template falls back to the MeshCore convention.
  config.brokers[0].topic[0] = 0;
  ASSERT_TRUE(MQTTObserverConfigCodec::buildTopic(config, config.brokers[0], "AABB",
                                                  "status", topic, sizeof(topic)));
  EXPECT_STREQ(topic, "meshcore/SEA/AABB/status");
}

TEST(MQTTObserverConfig, SchemeLessHostInfersSchemeFromPort) {
  MQTTObserverConfig config;
  MQTTObserverConfigCodec::setDefaults(config);
  MQTTObserverBrokerConfig& broker = config.brokers[0];
  char uri[128];
  strcpy(broker.server, "broker.example");
  broker.port = 443;
  ASSERT_TRUE(MQTTObserverConfigCodec::buildServerUri(broker, uri, sizeof(uri)));
  EXPECT_STREQ(uri, "wss://broker.example:443");
  broker.port = 1883;
  ASSERT_TRUE(MQTTObserverConfigCodec::buildServerUri(broker, uri, sizeof(uri)));
  EXPECT_STREQ(uri, "mqtt://broker.example:1883");
}

TEST(MQTTObserverCommandPolicy, ParseSlotRejectsZeroAndTrailingGarbage) {
  size_t slot = 99;
  const char* property = nullptr;
  EXPECT_FALSE(MQTTObserverCommandPolicy::parseSlot("set mqtt0.server x", "set ", 2, slot, property));
  EXPECT_FALSE(MQTTObserverCommandPolicy::parseSlot("set mqtt1server x", "set ", 2, slot, property));
  EXPECT_FALSE(MQTTObserverCommandPolicy::parseSlot("get mqtt1", "get ", 2, slot, property));
  EXPECT_FALSE(MQTTObserverCommandPolicy::parseSlot("get xmqtt1.p", "get ", 2, slot, property));
}

TEST(MQTTObserverQueuePolicy, RetryScheduleStaysWithinJitterWindow) {
  using namespace MQTTObserverQueuePolicy;
  RetryDecision first = retryDecision(false, 0, 1000);
  ASSERT_EQ(first.action, RetryAction::Schedule);
  EXPECT_EQ(first.retry_attempts, 1);
  EXPECT_GE(first.delay_ms, kRetryDelayBaseMs);
  EXPECT_LT(first.delay_ms, kRetryDelayBaseMs + kRetryDelayJitterMs);
  EXPECT_EQ(first.next_retry_ms, 1000u + first.delay_ms);
  EXPECT_EQ(retryDecision(false, kMaxQos0RetryAttempts, 1000).action, RetryAction::Drop);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
