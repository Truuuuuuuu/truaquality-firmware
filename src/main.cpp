#include <Arduino.h>
#include <WiFi.h>

#if __has_include("unit_config.h")
#include "unit_config.h"
#else
#error "Missing include/unit_config.h - copy include/unit_config.example.h and fill in this unit's WiFi, MQTT and device credentials."
#endif

#include "Sensors.h"
#include "Uplink.h"

static const char *FIRMWARE_VERSION = "0.2.0";
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;

static unsigned long lastReadingMs = 0;
static bool readOnce = false;

static void ensureWiFi()
{
  if (WiFi.status() == WL_CONNECTED)
  {
    return;
  }
  Serial.printf("[wifi] connecting to %s\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long startedMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedMs < WIFI_CONNECT_TIMEOUT_MS)
  {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.printf("[wifi] connected, ip %s\n", WiFi.localIP().toString().c_str());
  }
  else
  {
    Serial.println("[wifi] not connected, will retry next cycle");
  }
}

void setup()
{
  Serial.begin(115200);
  sensors::begin();
  ensureWiFi();

  uplink::begin(uplink::Config{
      MQTT_HOST,
      MQTT_PORT,
      MQTT_USE_TLS != 0,
      MQTT_USERNAME,
      MQTT_PASSWORD,
      DEVICE_ID,
      DEVICE_SECRET,
      FIRMWARE_VERSION,
  });
}

void loop()
{
  if (!readOnce || millis() - lastReadingMs >= REPORT_INTERVAL_MS)
  {
    readOnce = true;
    lastReadingMs = millis();
    ensureWiFi();

    // A sample without a trustworthy timestamp can't be placed on the pond's timeline, so skip it until NTP syncs.
    if (uplink::timeSynced())
    {
      uplink::enqueue(sensors::readAll());
    }
    else
    {
      Serial.println("[time] waiting for NTP sync, sample skipped");
    }
  }

  uplink::loop();
  delay(10);
}
