#ifdef ESP_PLATFORM

#include "ESP32Board.h"
#include <target.h>

#if (defined(ADMIN_PASSWORD) || defined(ENABLE_WIFI_OTA)) && !defined(DISABLE_WIFI_OTA)   // Repeater, Room Server or companion WiFi OTA
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <AsyncElegantOTA.h>

#include <SPIFFS.h>

#if defined(ENABLE_WIFI_OTA)
  #define OTA_DEVICE_TYPE "Companion"
#else
  #define OTA_DEVICE_TYPE "Repeater"
#endif

bool ESP32Board::startOTAUpdate(const char* id, char reply[]) {
  inhibit_sleep = true;   // prevent sleep during OTA

  IPAddress ip;
  if (WiFi.status() == WL_CONNECTED) {
    ip = WiFi.localIP();   // already on Wi-Fi: serve the OTA page on that network
  } else {
    WiFi.softAP("MeshCore-OTA", NULL);
    ip = WiFi.softAPIP();
  }
  sprintf(reply, "Started: http://%s/update", ip.toString().c_str());
  MESH_DEBUG_PRINTLN("startOTAUpdate: %s", reply);

  static char id_buf[60];
  sprintf(id_buf, "%s (%s)", id, getManufacturerName());
  static char home_buf[90];
  sprintf(home_buf, "<H2>Hi! I am a MeshCore %s. ID: %s</H2>", OTA_DEVICE_TYPE, id);

  // One server per boot: a second AsyncWebServer cannot bind port 80 again, so
  // repeated `start ota` would leak a server and leave the page dead.
  static AsyncWebServer* server = nullptr;
  if (server != nullptr) {
    return true;
  }

  server = new AsyncWebServer(80);

  server->on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(200, "text/html", home_buf);
  });
  server->on("/log", HTTP_GET, [](AsyncWebServerRequest *request) {
    request->send(SPIFFS, "/packet_log", "text/plain");
  });

  AsyncElegantOTA.setID(id_buf);
  AsyncElegantOTA.begin(server);    // Start ElegantOTA
  server->begin();

  return true;
}

#else
bool ESP32Board::startOTAUpdate(const char* id, char reply[]) {
  return false; // not supported
}
#endif

void ESP32Board::shutdownPeripherals() {
  // Power off the display if any
#ifdef DISPLAY_CLASS
  display.turnOff();
#endif

  // Power off LoRa
  radio_driver.powerOff();

  // Keep LoRa inactive during deepsleep
  digitalWrite(P_LORA_NSS, HIGH);
#if defined(CONFIG_IDF_TARGET_ESP32C3) || defined(CONFIG_IDF_TARGET_ESP32C6)
  gpio_hold_en((gpio_num_t)P_LORA_NSS);
#else
  rtc_gpio_hold_en((gpio_num_t)P_LORA_NSS);
#endif

  // Power off GPS if any
  if (sensors.getLocationProvider() != NULL) {
    sensors.getLocationProvider()->stop();
  }

  // Flush serial buffers
  Serial.flush();
  delay(100);
}

void ESP32Board::powerOff() {
  enterDeepSleep(0); // Do not wakeup
}

void ESP32Board::enterDeepSleep(uint32_t secs) {
  shutdownPeripherals();

  // Clear stale wakeup sources to avoid ghost wakeup
  // This is required when Power Management and automatic lightsleep are enabled
  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);

  if (secs > 0) {
    esp_sleep_enable_timer_wakeup(secs * 1000000ULL);
  }

  // Finally set ESP32 into deepsleep
  esp_deep_sleep_start(); // CPU halts here and never returns!
}
#endif
