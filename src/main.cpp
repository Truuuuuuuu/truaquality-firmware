#include <Arduino.h>
#include <WiFi.h>

#if __has_include("unit_config.h")
#include "unit_config.h"
#else
#error "Missing include/unit_config.h - copy include/unit_config.example.h and fill in this unit's MQTT and setup-hotspot settings."
#endif

#include "Provisioning.h"
#include "Sensors.h"
#include "Uplink.h"

static const char *FIRMWARE_VERSION = "0.3.0";
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;

static unsigned long lastReadingMs = 0;
static bool readOnce = false;
static bool uplinkStarted = false;

static void ensureWiFi()
{
  // The captive portal runs its own AP + WiFi.mode(); touching either here would tear it down.
  if (provisioning::portalActive())
  {
    return;
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    return;
  }
  Serial.println("[wifi] connecting");
  WiFi.mode(WIFI_STA);
  WiFi.begin(); // uses the credentials the setup portal saved
  unsigned long startedMs = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedMs < WIFI_CONNECT_TIMEOUT_MS)
  {
    delay(250);
  }
  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.printf("[wifi] connected to %s, ip %s\n", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
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
  provisioning::begin(provisioning::Config{
      SETUP_AP_PASSWORD,
  });
}

void loop()
{
  provisioning::loop(WiFi.status() == WL_CONNECTED);

  // Nothing else to do until a technician has provisioned WiFi + device identity through the setup hotspot.
  if (!provisioning::isProvisioned())
  {
    return;
  }

  if (!uplinkStarted)
  {
    uplinkStarted = true;
    uplink::begin(uplink::Config{
        MQTT_HOST,
        MQTT_PORT,
        MQTT_USE_TLS != 0,
        MQTT_USERNAME,
        MQTT_PASSWORD,
        provisioning::deviceId(),
        provisioning::deviceSecret(),
        FIRMWARE_VERSION,
    });
  }

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
