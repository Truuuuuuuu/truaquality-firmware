#include "Provisioning.h"

#include "CalibrationPage.h"
#include "TurbidityCalibration.h"
#include "TurbidityMath.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <cctype>
#include <cstring>
#include <ctime>
#include <vector>

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

  // The unit's clear-water reference in sensor-side millivolts, cached from NVS for the whole run. The
  // calibration lives here rather than in lib/Sensors because this module already owns the "unit" namespace;
  // keeping every Preferences call in one file is what lets Sensors stay a pure hardware reader.
  uint16_t turbidityClearMv = 0;
  // When turbidityClearMv was stored, Unix seconds; 0 = date unknown (D-08). Kept beside the value it dates.
  uint32_t turbidityCalAt = 0;

  char apName[24] = {0};
  char macInfoHtml[64] = {0};

  WiFiManagerParameter *macInfoParam = nullptr;
  WiFiManagerParameter *deviceIdParam = nullptr;
  WiFiManagerParameter *deviceSecretParam = nullptr;

  bool buttonWasDown = false;
  unsigned long buttonPressStartMs = 0;
  unsigned long disconnectedSinceMs = 0;
  bool restartPending = false;
  // The unit restarts to apply a freshly saved WiFi/identity pair, never merely because the portal is open. An
  // installed unit opened by BOOT-hold is already provisioned, so a rule keyed on "portal open + provisioned"
  // closed its portal about 2 s after it opened and left no time to recalibrate.
  bool savedThisSession = false;
  unsigned long restartAtMs = 0;
  unsigned long lastBlinkMs = 0;
  bool ledState = false;

  // D-03's 150 mV spread limit was derived from bench rows logged at 1 Hz, so the window it judges has to be
  // sampled at 1 Hz too - a faster or slower cadence would change what "steady over 20 samples" means.
  constexpr unsigned long SAMPLER_INTERVAL_MS = 1000;

  // Live window: the last 20 samples as a ring, feeding the on-screen stability indicator. Order inside the
  // ring does not matter because the rule is max minus min.
  float liveWindow[turbidity::cal::CAPTURE_WINDOW_SAMPLES] = {0};
  size_t liveFilled = 0;
  size_t liveNext = 0;
  float latestMv = NAN;
  float latestNtu = NAN;
  unsigned long lastSampleMs = 0;
  bool samplerPrimed = false;

  // Capture window: separate from the live ring so a capture only ever holds samples taken after the request.
  float captureWindow[turbidity::cal::CAPTURE_WINDOW_SAMPLES] = {0};
  size_t captureCount = 0;
  provisioning::CaptureState captureState = provisioning::CaptureState::Idle;
  bool captureStartedInPortal = false;
  uint16_t captureMedianMv = 0;
  float captureSpreadMv = NAN;
  const char *captureReason = nullptr;

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

  // Same shape as loadIdentity(): open the shared "unit" namespace read-only, pull the value, close it at
  // once, cache it for the run. 0 is the sentinel for "never calibrated" - it is what NVS hands back when the
  // key was never written, and it is the one value no real clear-water reading can be. The key name is
  // deliberately 13 characters: NVS caps key names at 15, and a longer, more readable name would be
  // truncated silently and stop matching the key Phase 5's portal writes.
  void loadTurbidityCalibration()
  {
    prefs.begin("unit", true);
    turbidityClearMv = prefs.getUShort("turb_clear_mv", 0);
    // 11 characters, under the same 15-character cap. Missing on a unit calibrated before Phase 5, which reads
    // back as 0 = "date unknown" - exactly what such a unit should show.
    turbidityCalAt = prefs.getUInt("turb_cal_at", 0);
    prefs.end();
  }

  // Fires when the "Setup" page is saved. Runs the same validation the portal's HTML `pattern` attributes
  // already do client-side, since nothing stops an admin from bypassing them.
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
    savedThisSession = true;
    Serial.println("[provisioning] device identity saved");
  }

  // Fires after the WiFi page is saved. With breakAfterConfig WiFiManager also calls it when the connection
  // attempt fails, which is fine: the credentials were still written and a restart retries them cleanly.
  void onSaveWifi()
  {
    savedThisSession = true;
    Serial.println("[provisioning] WiFi saved");
  }

  void setupPortalParams()
  {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(macInfoHtml, sizeof(macInfoHtml), "<p>Unit MAC: %02X:%02X:%02X:%02X:%02X:%02X</p>",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    macInfoParam = new WiFiManagerParameter(macInfoHtml);

    // Prefilled with the current device ID (blank on a fresh unit) so an admin only has to retype it when
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

  void finishCapture()
  {
    const turbidity::cal::CaptureResult result = turbidity::cal::evaluateCapture(captureWindow, captureCount);
    captureMedianMv = result.medianMv;
    captureSpreadMv = result.spreadMv;
    if (result.outcome == turbidity::cal::CaptureOutcome::Accepted)
    {
      if (provisioning::storeTurbidityClearWaterMv(result.medianMv))
      {
        captureState = provisioning::CaptureState::Accepted;
        captureReason = nullptr;
        Serial.printf("[provisioning] calibration capture accepted: median %u mV, spread %.1f mV\n",
                      static_cast<unsigned>(captureMedianMv), captureSpreadMv);
        return;
      }
      captureReason = "nvs";
    }
    else
    {
      captureReason = turbidity::cal::captureOutcomeToken(result.outcome);
    }
    captureState = provisioning::CaptureState::Refused;
    Serial.printf("[provisioning] calibration capture refused (%s): median %u mV, spread %.1f mV\n", captureReason,
                  static_cast<unsigned>(captureMedianMv), captureSpreadMv);
  }

  // One burst per second while the setup hotspot is open or a capture is running, and nothing otherwise: a
  // unit in normal field operation takes its readings only on the reporting schedule. Tick-driven, never a
  // wait, so the portal, MQTT keepalive and PUBACKs keep running through a 20 s capture.
  void tickSampler()
  {
    const bool portalOpen = wm.getConfigPortalActive();
    bool capturing = captureState == provisioning::CaptureState::Capturing;

    // Nobody can see the result of a portal capture once the hotspot is gone, and a calibration change nobody
    // saw is exactly the confidently-wrong risk this phase exists to prevent - so it is cancelled, not stored.
    if (capturing && captureStartedInPortal && !portalOpen)
    {
      captureState = provisioning::CaptureState::Refused;
      captureReason = "cancelled";
      capturing = false;
      Serial.println("[provisioning] calibration capture cancelled: setup hotspot closed");
    }

    if (!portalOpen && !capturing)
    {
      liveFilled = 0;
      liveNext = 0;
      latestMv = NAN;
      latestNtu = NAN;
      samplerPrimed = false;
      return;
    }

    if (samplerPrimed && millis() - lastSampleMs < SAMPLER_INTERVAL_MS)
    {
      return;
    }
    if (config.readTurbiditySensorMv == nullptr)
    {
      return;
    }
    samplerPrimed = true;
    lastSampleMs = millis();

    const float mv = config.readTurbiditySensorMv();
    latestMv = mv;
    latestNtu = config.readTurbidityNtu != nullptr ? config.readTurbidityNtu() : NAN;

    liveWindow[liveNext] = mv;
    liveNext = (liveNext + 1) % turbidity::cal::CAPTURE_WINDOW_SAMPLES;
    if (liveFilled < turbidity::cal::CAPTURE_WINDOW_SAMPLES)
    {
      liveFilled++;
    }

    if (capturing)
    {
      captureWindow[captureCount++] = mv;
      if (captureCount >= turbidity::cal::CAPTURE_WINDOW_SAMPLES)
      {
        finishCapture();
      }
    }
  }

  // Every /cal response is live state, so none may be cached: a captive browser that served a stale status or
  // page would show an admin a reading or a "Saved" line that is no longer true.
  void sendJson(int code, JsonDocument &doc)
  {
    String body;
    serializeJson(doc, body);
    wm.server->sendHeader("Cache-Control", "no-store");
    wm.server->send(code, "application/json", body);
  }

  void handleBlocked()
  {
    wm.server->send(404, "text/plain", "not available");
  }

  void handleCalPage()
  {
    wm.server->sendHeader("Cache-Control", "no-store");
    wm.server->send(200, "text/html; charset=utf-8", provisioning::calpage::CAL_PAGE_HTML);
  }

  const char *captureStateToken(provisioning::CaptureState state)
  {
    switch (state)
    {
    case provisioning::CaptureState::Idle:
      return "idle";
    case provisioning::CaptureState::Capturing:
      return "capturing";
    case provisioning::CaptureState::Accepted:
      return "accepted";
    case provisioning::CaptureState::Refused:
      return "refused";
    }
    return "refused";
  }

  // A fixed key set and nothing else (T-05-21): the hotspot password is shared across every unit, so this
  // response must never carry the device identity, its secret, the saved SSID or the MAC. Serves cached values
  // only - the sampler owns the sensor - so polling it costs no burst.
  void handleCalStatus()
  {
    const provisioning::LiveReadout live = provisioning::turbidityLiveReadout();
    const provisioning::CaptureStatus capture = provisioning::turbidityCaptureStatus();
    const bool finished = capture.state == provisioning::CaptureState::Accepted ||
                          capture.state == provisioning::CaptureState::Refused;

    JsonDocument doc;
    if (isnan(live.sensorMv))
    {
      doc["mv"] = nullptr;
    }
    else
    {
      doc["mv"] = live.sensorMv;
    }
    if (isnan(live.ntu))
    {
      doc["ntu"] = nullptr;
    }
    else
    {
      doc["ntu"] = live.ntu;
    }
    doc["state"] = live.state;
    doc["capture"] = captureStateToken(capture.state);
    doc["samples"] = static_cast<unsigned>(capture.samples);
    doc["needed"] = static_cast<unsigned>(turbidity::cal::CAPTURE_WINDOW_SAMPLES);
    if (finished)
    {
      doc["medianMv"] = static_cast<unsigned>(capture.medianMv);
      if (isnan(capture.spreadMv))
      {
        doc["spreadMv"] = nullptr;
      }
      else
      {
        doc["spreadMv"] = capture.spreadMv;
      }
    }
    else
    {
      doc["medianMv"] = nullptr;
      doc["spreadMv"] = nullptr;
    }
    if (capture.reason != nullptr)
    {
      doc["reason"] = capture.reason;
    }
    else
    {
      doc["reason"] = nullptr;
    }
    doc["storedMv"] = static_cast<unsigned>(provisioning::turbidityClearWaterMv());
    doc["calibratedAt"] = provisioning::turbidityCalibratedAt();
    sendJson(200, doc);
  }

  void handleCalCapture()
  {
    JsonDocument doc;
    if (provisioning::startTurbidityCapture())
    {
      doc["capture"] = "capturing";
      sendJson(202, doc);
      return;
    }
    doc["error"] = "capture already running";
    sendJson(409, doc);
  }

  // D-09: a blank field means "keep", never "write 0". Only the "mv" argument is read, its length is capped
  // before parsing, and the response carries a result token, never the raw input (T-05-19, T-05-20). A typed
  // value still goes through the single door, which re-checks the plausible window and stamps the date.
  void handleCalSet()
  {
    JsonDocument doc;
    const String arg = wm.server->hasArg("mv") ? wm.server->arg("mv") : String();
    if (arg.length() > 16)
    {
      Serial.println("[provisioning] manual calibration not_a_number");
      doc["result"] = "not_a_number";
      sendJson(400, doc);
      return;
    }

    const turbidity::cal::ManualResult parsed = turbidity::cal::parseManualMv(arg.c_str());
    switch (parsed.kind)
    {
    case turbidity::cal::ManualKind::Keep:
      Serial.println("[provisioning] manual calibration keep");
      doc["result"] = "keep";
      sendJson(200, doc);
      return;
    case turbidity::cal::ManualKind::NotANumber:
    case turbidity::cal::ManualKind::Implausible:
      Serial.printf("[provisioning] manual calibration %s\n", turbidity::cal::manualKindToken(parsed.kind));
      doc["result"] = turbidity::cal::manualKindToken(parsed.kind);
      sendJson(400, doc);
      return;
    case turbidity::cal::ManualKind::Set:
      break;
    }

    // A running capture would overwrite this value with its own median 20 s later, and the admin would see two
    // different "Saved" lines for one session - so a manual save waits for the capture to finish.
    if (provisioning::turbidityCaptureStatus().state == provisioning::CaptureState::Capturing)
    {
      Serial.println("[provisioning] manual calibration busy");
      doc["result"] = "busy";
      sendJson(409, doc);
      return;
    }
    if (!provisioning::storeTurbidityClearWaterMv(parsed.mv))
    {
      Serial.println("[provisioning] manual calibration nvs");
      doc["result"] = "nvs";
      sendJson(500, doc);
      return;
    }
    Serial.println("[provisioning] manual calibration set");
    doc["result"] = "set";
    doc["storedMv"] = static_cast<unsigned>(provisioning::turbidityClearWaterMv());
    doc["calibratedAt"] = provisioning::turbidityCalibratedAt();
    sendJson(200, doc);
  }

  // Runs on every portal start, because WiFiManager rebuilds its web server each time. It is called before
  // WiFiManager adds its own routes, and the server matches the first registered handler, so the routes added
  // here win over WiFiManager's for the same URI.
  void registerCalibrationRoutes()
  {
    // WiFiManager registers web OTA (/update, and /u as its upload target), /erase and /restart whether or not
    // they are in the menu. Anyone holding the shared hotspot password could otherwise flash arbitrary firmware
    // or wipe the unit's WiFi (T-05-16, T-05-17). /u is answered by a handler with no upload function, so an
    // uploaded image is discarded rather than written.
    wm.server->on("/update", HTTP_ANY, handleBlocked);
    wm.server->on("/u", HTTP_ANY, handleBlocked);
    wm.server->on("/erase", HTTP_ANY, handleBlocked);
    wm.server->on("/restart", HTTP_ANY, handleBlocked);

    // Changes are POST-only so a captive browser's prefetch or a followed link can never start a capture or
    // store a value (T-05-18).
    wm.server->on("/cal", HTTP_GET, handleCalPage);
    wm.server->on("/cal/status", HTTP_GET, handleCalStatus);
    wm.server->on("/cal/capture", HTTP_POST, handleCalCapture);
    wm.server->on("/cal/set", HTTP_POST, handleCalSet);
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
    loadTurbidityCalibration();

    // D-17: exactly one line, and only when the calibration is unusable. An uncalibrated unit and a unit with
    // a dead turbidity sensor look identical from the dashboard - both report temperature and no turbidity -
    // so the boot log is the only place an admin can tell the two apart, and it has to name which of the
    // two unusable cases this is.
    if (turbidityClearMv == 0)
    {
      Serial.println("[provisioning] turbidity uncalibrated: no clear-water value stored");
    }
    else if (!turbidity::isPlausibleClearWaterMv(turbidityClearMv))
    {
      Serial.printf("[provisioning] turbidity uncalibrated: stored clear-water value %u mV is outside the "
                    "plausible window\n",
                    static_cast<unsigned>(turbidityClearMv));
    }

    uint8_t mac[6];
    WiFi.macAddress(mac);
    snprintf(apName, sizeof(apName), "TruAquality-%02X%02X", mac[4], mac[5]);

    wm.setConfigPortalBlocking(false);
    // Lets the save callbacks run (and the portal close) even when a WiFi connection attempt fails, so
    // main.cpp's loop-driven reopen logic below stays in control instead of WiFiManager looping on its own.
    wm.setBreakAfterConfig(true);
    wm.setSaveParamsCallback(onSaveParams);
    wm.setSaveConfigCallback(onSaveWifi);
    // The timed BOOT-hold/outage portal otherwise closes at 5 min even while an admin is watching a page,
    // because only WiFiManager's own handlers count as "accessed". With this it stays open while any station is
    // joined to the hotspot - trust is the same as the Setup page: whoever holds SETUP_AP_PASSWORD.
    wm.setAPClientCheck(true);
    // An explicit menu instead of the params-page shortcut, which calls setMenu itself and would drop the
    // "custom" Calibrate button. Listing "param" still keeps the Setup page separate from the WiFi page. There
    // is deliberately no "update", "erase" or "restart": update is web OTA, and all three are also blocked at
    // the route level in registerCalibrationRoutes(). Calibrating never sets savedThisSession, so it never
    // restarts the unit (CAL-07).
    std::vector<const char *> menu = {"wifi", "param", "custom", "info", "exit"};
    wm.setMenu(menu);
    wm.setCustomMenuHTML(provisioning::calpage::CAL_MENU_HTML);
    wm.setWebServerCallback(registerCalibrationRoutes);
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

  uint16_t turbidityClearWaterMv()
  {
    return turbidityClearMv;
  }

  uint32_t turbidityCalibratedAt()
  {
    return turbidityCalAt;
  }

  bool storeTurbidityClearWaterMv(uint16_t clearWaterMv)
  {
    // Validated before the write, not after the read alone: this function is the only door into the key, so
    // refusing here is what stops a mistyped bench command or a Phase 5 portal submission from persisting a
    // reference that would silently rescale every later reading. The loader re-validates anyway, because
    // flash written by an older build is not this function's output.
    if (!turbidity::isPlausibleClearWaterMv(clearWaterMv))
    {
      Serial.printf("[provisioning] rejected: clear-water calibration %u mV is implausible for clear water\n",
                    static_cast<unsigned>(clearWaterMv));
      return false;
    }

    // Both results are checked because a "saved" line the admin trusts must mean it survives a reboot:
    // putUShort returns 0 on a full or failing NVS, and an unchecked write would leave the unit calibrated in
    // RAM only, silently reverting to "no turbidity" at the next power cycle.
    if (!prefs.begin("unit", false))
    {
      Serial.println("[provisioning] calibration NOT saved: could not open NVS");
      return false;
    }
    const size_t written = prefs.putUShort("turb_clear_mv", clearWaterMv);
    if (written != sizeof(uint16_t))
    {
      prefs.end();
      Serial.println("[provisioning] calibration NOT saved: NVS write failed");
      return false;
    }
    // Value first, then the date it was stored. A unit that has not synced NTP (an unprovisioned one never
    // does) gets 0 = "date unknown" from calibrationStamp rather than a 1970 date. If only this second write
    // fails the value is kept - it is valid and already on flash - but this run reports "date unknown" so the
    // screen never pairs a newer value with an older date.
    const uint32_t stamp = turbidity::cal::calibrationStamp(static_cast<long long>(time(nullptr)));
    const size_t stampWritten = prefs.putUInt("turb_cal_at", stamp);
    prefs.end();

    turbidityClearMv = clearWaterMv;
    if (stampWritten != sizeof(uint32_t))
    {
      turbidityCalAt = 0;
      Serial.println("[provisioning] calibration date NOT saved: NVS write failed");
    }
    else
    {
      turbidityCalAt = stamp;
      if (stamp == 0)
      {
        Serial.println("[provisioning] calibration date unknown: clock not set");
      }
    }
    Serial.printf("[provisioning] turbidity calibration saved: %u mV at %lu\n", static_cast<unsigned>(clearWaterMv),
                  static_cast<unsigned long>(turbidityCalAt));

    // Applied here, inside the door, so no caller can store a value and forget to make it live.
    if (config.applyTurbidityCalibration != nullptr)
    {
      config.applyTurbidityCalibration(clearWaterMv);
    }
    return true;
  }

  bool startTurbidityCapture()
  {
    if (captureState == CaptureState::Capturing)
    {
      return false;
    }
    captureCount = 0;
    for (size_t i = 0; i < turbidity::cal::CAPTURE_WINDOW_SAMPLES; i++)
    {
      captureWindow[i] = NAN;
    }
    captureMedianMv = 0;
    captureSpreadMv = NAN;
    captureReason = nullptr;
    captureStartedInPortal = wm.getConfigPortalActive();
    captureState = CaptureState::Capturing;
    // D-01: the window starts at the request. Un-priming makes the next tick take the first sample at once
    // and restarts the 1 s cadence from there, so no reading from before the click is ever reused.
    samplerPrimed = false;
    Serial.println("[provisioning] calibration capture started (20 s)");
    return true;
  }

  CaptureStatus turbidityCaptureStatus()
  {
    return CaptureStatus{captureState, static_cast<uint8_t>(captureCount), captureMedianMv, captureSpreadMv,
                         captureReason};
  }

  LiveReadout turbidityLiveReadout()
  {
    return LiveReadout{latestMv, latestNtu,
                       turbidity::cal::liveStateToken(turbidity::cal::liveState(liveWindow, liveFilled))};
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
    tickSampler();

    // Invariant: an unprovisioned unit always has its setup hotspot open. WiFiManager can close the portal on
    // its own (its timeout, or breakAfterConfig after either page is saved) without both pieces being present
    // yet - reopen it rather than leaving the unit stranded with no WiFi and no way to reach it.
    if (!isProvisioned() && !wm.getConfigPortalActive() && !restartPending)
    {
      openPortal(0);
    }

    if (savedThisSession && isProvisioned() && !restartPending)
    {
      Serial.println("[provisioning] WiFi or device identity saved, restarting");
      restartPending = true;
      restartAtMs = millis() + RESTART_DELAY_MS;
    }
    if (restartPending && millis() >= restartAtMs)
    {
      ESP.restart();
    }
  }
}
