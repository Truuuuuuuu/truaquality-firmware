#include "Provisioning.h"

#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <cctype>
#include <cstring>

namespace
{
  // BOOT on most ESP32 dev boards (nodemcu-32s included). Wire an external waterproof button from this pin to
  // GND for a sealed enclosure; no code change needed. NOTE: only press it after boot - held down during
  // power-on it drops the chip into the ROM download mode instead.
  constexpr int BUTTON_PIN = 0;
  // Built-in blue LED on most nodemcu-32s boards.
  constexpr int LED_PIN = 2;
  constexpr unsigned long BUTTON_HOLD_MS = 5000;
  constexpr unsigned long OUTAGE_TRIGGER_MS = 10UL * 60 * 1000;
  constexpr unsigned long TIMED_PORTAL_SECONDS = 5UL * 60;
  constexpr unsigned long RESTART_DELAY_MS = 2000;
  constexpr unsigned long LED_BLINK_MS = 150;

  constexpr size_t DEVICE_ID_LEN = 36;     // UUID, no null
  constexpr size_t DEVICE_SECRET_LEN = 43; // base64url HMAC-SHA256, no null

  WiFiManager wm;
  Preferences prefs;
  provisioning::Config config;

  char deviceIdBuf[DEVICE_ID_LEN + 1] = {0};
  char deviceSecretBuf[DEVICE_SECRET_LEN + 1] = {0};
  bool identityPresent = false;

  char apName[24] = {0};
  char macInfoHtml[64] = {0};

  WiFiManagerParameter *macInfoParam = nullptr;
  WiFiManagerParameter *deviceIdParam = nullptr;
  WiFiManagerParameter *deviceSecretParam = nullptr;

  bool buttonWasDown = false;
  unsigned long buttonPressStartMs = 0;
  unsigned long disconnectedSinceMs = 0;
  bool restartPending = false;
  unsigned long restartAtMs = 0;
  unsigned long lastBlinkMs = 0;
  bool ledState = false;

  bool isValidDeviceId(const char *value)
  {
    if (strlen(value) != DEVICE_ID_LEN)
    {
      return false;
    }
    for (size_t i = 0; i < DEVICE_ID_LEN; i++)
    {
      char c = value[i];
      if (i == 8 || i == 13 || i == 18 || i == 23)
      {
        if (c != '-')
        {
          return false;
        }
      }
      else if (!isxdigit(static_cast<unsigned char>(c)))
      {
        return false;
      }
    }
    return true;
  }

  bool isValidDeviceSecret(const char *value)
  {
    size_t len = strlen(value);
    if (len != DEVICE_SECRET_LEN)
    {
      return false;
    }
    for (size_t i = 0; i < len; i++)
    {
      char c = value[i];
      if (!isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
      {
        return false;
      }
    }
    return true;
  }

  void loadIdentity()
  {
    prefs.begin("unit", true);
    String id = prefs.getString("device_id", "");
    String secret = prefs.getString("device_secret", "");
    prefs.end();

    strncpy(deviceIdBuf, id.c_str(), sizeof(deviceIdBuf) - 1);
    deviceIdBuf[sizeof(deviceIdBuf) - 1] = '\0';
    strncpy(deviceSecretBuf, secret.c_str(), sizeof(deviceSecretBuf) - 1);
    deviceSecretBuf[sizeof(deviceSecretBuf) - 1] = '\0';

    identityPresent = isValidDeviceId(deviceIdBuf) && isValidDeviceSecret(deviceSecretBuf);
  }

  // Fires when the "Setup" page is saved. Runs the same validation the portal's HTML `pattern` attributes
  // already do client-side, since nothing stops a technician from bypassing them.
  void onSaveParams()
  {
    const char *idValue = deviceIdParam->getValue();
    const char *secretValue = deviceSecretParam->getValue();

    if (!isValidDeviceId(idValue))
    {
      Serial.println("[provisioning] rejected: device ID must be a 36-character UUID");
      return;
    }

    bool secretProvided = strlen(secretValue) > 0;
    if (secretProvided && !isValidDeviceSecret(secretValue))
    {
      Serial.println("[provisioning] rejected: device secret must be 43 base64url characters");
      return;
    }
    if (!secretProvided && !identityPresent)
    {
      Serial.println("[provisioning] rejected: device secret is required for a new unit");
      return;
    }

    prefs.begin("unit", false);
    prefs.putString("device_id", idValue);
    if (secretProvided)
    {
      prefs.putString("device_secret", secretValue);
    }
    prefs.end();

    strncpy(deviceIdBuf, idValue, sizeof(deviceIdBuf) - 1);
    deviceIdBuf[sizeof(deviceIdBuf) - 1] = '\0';
    if (secretProvided)
    {
      strncpy(deviceSecretBuf, secretValue, sizeof(deviceSecretBuf) - 1);
      deviceSecretBuf[sizeof(deviceSecretBuf) - 1] = '\0';
    }
    identityPresent = isValidDeviceId(deviceIdBuf) && isValidDeviceSecret(deviceSecretBuf);
    Serial.println("[provisioning] device identity saved");
  }

  void setupPortalParams()
  {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(macInfoHtml, sizeof(macInfoHtml), "<p>Unit MAC: %02X:%02X:%02X:%02X:%02X:%02X</p>",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    macInfoParam = new WiFiManagerParameter(macInfoHtml);

    // Prefilled with the current device ID (blank on a fresh unit) so a technician only has to retype it when
    // actually changing units, not on every secret rotation.
    deviceIdParam = new WiFiManagerParameter(
        "device_id", "Device ID (from the Devices page)", deviceIdBuf, DEVICE_ID_LEN,
        "pattern=\"[0-9a-fA-F-]{36}\" maxlength=\"36\" required");
    // Never prefilled - the backend never sends the secret back down, so there's nothing to prefill it with
    // safely, and a rotation should always require pasting the fresh one.
    deviceSecretParam = new WiFiManagerParameter(
        "device_secret", "Device secret (leave blank to keep the current one)", "", DEVICE_SECRET_LEN,
        "pattern=\"[A-Za-z0-9_-]{43}\" maxlength=\"43\" placeholder=\"leave blank to keep current\"");

    wm.addParameter(macInfoParam);
    wm.addParameter(deviceIdParam);
    wm.addParameter(deviceSecretParam);
  }

  void openPortal(unsigned long timeoutSeconds)
  {
    if (wm.getConfigPortalActive())
    {
      return;
    }
    wm.setConfigPortalTimeout(timeoutSeconds);
    Serial.printf("[provisioning] setup hotspot \"%s\" open%s\n", apName,
                   timeoutSeconds == 0 ? " (no timeout)" : "");
    wm.startConfigPortal(apName, config.setupApPassword);
  }

  void handleButton()
  {
    bool down = digitalRead(BUTTON_PIN) == LOW;
    if (down && !buttonWasDown)
    {
      buttonPressStartMs = millis();
    }
    if (down && !wm.getConfigPortalActive() && millis() - buttonPressStartMs >= BUTTON_HOLD_MS)
    {
      Serial.println("[provisioning] BOOT held, opening setup hotspot");
      openPortal(TIMED_PORTAL_SECONDS);
    }
    buttonWasDown = down;
  }

  void handleOutage(bool wifiConnected)
  {
    if (!provisioning::isProvisioned() || wm.getConfigPortalActive() || wifiConnected)
    {
      disconnectedSinceMs = 0;
      return;
    }
    if (disconnectedSinceMs == 0)
    {
      disconnectedSinceMs = millis();
      return;
    }
    if (millis() - disconnectedSinceMs >= OUTAGE_TRIGGER_MS)
    {
      Serial.println("[provisioning] WiFi unreachable for 10 minutes, opening setup hotspot");
      openPortal(TIMED_PORTAL_SECONDS);
      // Reset so a still-ongoing outage opens the hotspot again 10 minutes after this timed attempt closes,
      // rather than only once.
      disconnectedSinceMs = 0;
    }
  }

  void handleLed()
  {
    bool shouldBlink = !provisioning::isProvisioned() || wm.getConfigPortalActive();
    if (!shouldBlink)
    {
      digitalWrite(LED_PIN, LOW);
      ledState = false;
      return;
    }
    if (millis() - lastBlinkMs >= LED_BLINK_MS)
    {
      lastBlinkMs = millis();
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState ? HIGH : LOW);
    }
  }
}

namespace provisioning
{
  void begin(const Config &newConfig)
  {
    config = newConfig;

    pinMode(BUTTON_PIN, INPUT_PULLUP);
    pinMode(LED_PIN, OUTPUT);

    // Needed before WiFi.macAddress() returns a real value and before the portal can start.
    WiFi.mode(WIFI_STA);

    loadIdentity();

    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(apName, sizeof(apName), "TruAquality-%02X%02X", mac[4], mac[5]);

    wm.setConfigPortalBlocking(false);
    // Lets the save callbacks run (and the portal close) even when a WiFi connection attempt fails, so
    // main.cpp's loop-driven reopen logic below stays in control instead of WiFiManager looping on its own.
    wm.setBreakAfterConfig(true);
    wm.setSaveParamsCallback(onSaveParams);
    wm.setParamsPage(true);
    setupPortalParams();

    if (!isProvisioned())
    {
      openPortal(0);
    }
  }

  bool isProvisioned()
  {
    return identityPresent && wm.getWiFiIsSaved();
  }

  const char *deviceId()
  {
    return deviceIdBuf;
  }

  const char *deviceSecret()
  {
    return deviceSecretBuf;
  }

  bool portalActive()
  {
    return wm.getConfigPortalActive();
  }

  void loop(bool wifiConnected)
  {
    wm.process();

    handleButton();
    handleOutage(wifiConnected);
    handleLed();

    // Invariant: an unprovisioned unit always has its setup hotspot open. WiFiManager can close the portal on
    // its own (its timeout, or breakAfterConfig after either page is saved) without both pieces being present
    // yet - reopen it rather than leaving the unit stranded with no WiFi and no way to reach it.
    if (!isProvisioned() && !wm.getConfigPortalActive() && !restartPending)
    {
      openPortal(0);
    }

    if (wm.getConfigPortalActive() && isProvisioned() && !restartPending)
    {
      Serial.println("[provisioning] WiFi + device identity saved, restarting");
      restartPending = true;
      restartAtMs = millis() + RESTART_DELAY_MS;
    }
    if (restartPending && millis() >= restartAtMs)
    {
      ESP.restart();
    }
  }
}
