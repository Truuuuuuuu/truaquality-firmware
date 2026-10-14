#include "Provisioning.h"

#include "CalibrationPage.h"
#include "PhCalibrationPage.h"
#include "PhAcceptance.h"
#include "PhCalibration.h"
#include "PhMath.h"
#include "TurbidityCalibration.h"
#include "TurbidityMath.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <algorithm>
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

  // The unit's pH calibration, cached from NVS for the run. Zeroed = version 0 = uncalibrated (SENS-09).
  ph::Calibration phCal{};

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

  // pH portal sampler (Phase 11). Its own live ring and 1 Hz clock, separate from turbidity's, because the pH
  // drift rule is judged on chronological 1 Hz samples of the pH pin. Captured points are held in RAM only, so a
  // reboot, a closed hotspot or walking away discards them and the stored calibration stays in effect (D-14).
  float phLiveRing[ph::cal::CAPTURE_WINDOW_SAMPLES] = {0};
  size_t phLiveFilled = 0;
  size_t phLiveNext = 0;
  float phLatestMv = NAN;
  float phLatestPh = NAN;
  float phLatestTempC = NAN;
  unsigned long phLastSampleMs = 0;
  unsigned long phCaptureStartMs = 0;
  bool phSamplerPrimed = false;
  ph::accept::CaptureEngine phEngine{};
  ph::accept::Slot phCaptureSlot = ph::accept::Slot::None;
  bool phCaptureStartedInPortal = false;
  ph::accept::HeldPair phHeld{};
  ph::accept::PairResult phReview{};
  bool phReviewValid = false;

  // Both captures share the 1 Hz cadence and the admin's attention, so only one runs at a time, pH or turbidity.
  bool anyCaptureRunning()
  {
    return ph::accept::isCaptureRunning(phEngine) || captureState == provisioning::CaptureState::Capturing;
  }

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

  // D-17 layout, every key under NVS's 15-character cap. Everything defaults to 0, so a never-calibrated unit, or
  // one whose last store was cut before ph_ver (the commit marker) was written, reads back version 0 and
  // ph::isValidCalibration refuses it: honestly uncalibrated, never half-calibrated. No validation here — PhMath
  // re-validates on every use, because flash is not this build's output.
  void loadPhCalibration()
  {
    prefs.begin("unit", true);
    phCal.version = prefs.getUChar("ph_ver", 0);
    phCal.mvA = prefs.getUShort("ph_mv_a", 0);
    phCal.bufA = prefs.getUShort("ph_buf_a", 0);
    phCal.mvB = prefs.getUShort("ph_mv_b", 0);
    phCal.bufB = prefs.getUShort("ph_buf_b", 0);
    phCal.tcalCentiC = prefs.getShort("ph_tcal", 0);
    phCal.calAt = prefs.getUInt("ph_cal_at", 0);
    prefs.end();
  }

  void applyPhCalibrationLive()
  {
    if (config.applyPhCalibration != nullptr)
    {
      config.applyPhCalibration(phCal.mvA, phCal.bufA, phCal.mvB, phCal.bufB, phCal.tcalCentiC, phCal.calAt,
                                phCal.version);
    }
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

  void resetPhLive()
  {
    phLiveFilled = 0;
    phLiveNext = 0;
    phLatestMv = NAN;
    phLatestPh = NAN;
    phLatestTempC = NAN;
    phSamplerPrimed = false;
  }

  // One pH burst and one non-blocking temperature poll per second while the setup hotspot is open, nothing
  // otherwise. Tick-driven, never a wait, so the portal, MQTT keepalive and PUBACKs keep running through a
  // capture that may take up to SETTLE_TIMEOUT_S.
  void tickPhSampler()
  {
    const bool portalOpen = wm.getConfigPortalActive();

    // As with turbidity: a capture nobody can see the end of is cancelled, never kept.
    if (ph::accept::isCaptureRunning(phEngine) && phCaptureStartedInPortal && !portalOpen)
    {
      ph::accept::cancelCapture(phEngine);
      Serial.println("[provisioning] pH capture cancelled: setup hotspot closed");
    }

    if (!portalOpen)
    {
      // D-14: held points and terminal capture results never outlive their portal session.
      // Reset after cancellation above so an active close is still logged before its metadata is discarded.
      phEngine = ph::accept::CaptureEngine{};
      phCaptureSlot = ph::accept::Slot::None;
      phCaptureStartedInPortal = false;
      phCaptureStartMs = 0;
      phHeld = ph::accept::HeldPair{};
      phReview = ph::accept::PairResult{};
      phReviewValid = false;
      resetPhLive();
      return;
    }

    if (phSamplerPrimed && millis() - phLastSampleMs < SAMPLER_INTERVAL_MS)
    {
      return;
    }
    if (config.readPhSensorMv == nullptr)
    {
      return;
    }
    phSamplerPrimed = true;
    phLastSampleMs = millis();

    const float t = config.pollTemperatureC != nullptr ? config.pollTemperatureC() : NAN;
    const float mv = config.readPhSensorMv(t);
    phLatestPh = config.lastPhValue != nullptr ? config.lastPhValue() : NAN;
    phLatestMv = mv;
    phLatestTempC = t;

    phLiveRing[phLiveNext] = mv;
    phLiveNext = (phLiveNext + 1) % ph::cal::CAPTURE_WINDOW_SAMPLES;
    if (phLiveFilled < ph::cal::CAPTURE_WINDOW_SAMPLES)
    {
      phLiveFilled++;
    }

    if (!ph::accept::isCaptureRunning(phEngine))
    {
      return;
    }
    // Wall-clock seconds since the request (unsigned subtraction survives a millis() wrap), so the settle
    // timeout and its "N min" text are real time even when a tick runs late (D-15).
    const uint16_t elapsed =
        static_cast<uint16_t>(std::min<unsigned long>((millis() - phCaptureStartMs) / 1000UL, 65535UL));
    ph::accept::feedCapture(phEngine, mv, t, elapsed);

    const char *slotTok = ph::accept::slotToken(phCaptureSlot);
    if (phEngine.state == ph::accept::CaptureState::Accepted)
    {
      const ph::accept::PairResult r = ph::accept::acceptIntoSlot(phHeld, phCaptureSlot, phEngine.point);
      if (r.reason != ph::accept::Refusal::None)
      {
        // acceptIntoSlot already dropped the slot captured second; the capture reports the pair's reason.
        phEngine.state = ph::accept::CaptureState::Refused;
        phEngine.reason = r.reason;
        phEngine.reasonValue = r.reasonValue;
        phReviewValid = false;
        Serial.printf("[provisioning] pH capture %s refused (%s)\n", slotTok, ph::accept::refusalToken(r.reason));
        return;
      }
      if (r.complete)
      {
        phReview = r;
        phReviewValid = true;
      }
      Serial.printf("[provisioning] pH capture %s accepted: nominal %u, %.1f C, buffer pH %.2f, %u mV\n", slotTok,
                    static_cast<unsigned>(phEngine.point.nominal), phEngine.point.tempC,
                    phEngine.point.bufHundredths / 100.0, static_cast<unsigned>(phEngine.point.mv));
    }
    else if (phEngine.state == ph::accept::CaptureState::Refused)
    {
      Serial.printf("[provisioning] pH capture %s refused (%s), value %.1f\n", slotTok,
                    ph::accept::refusalToken(phEngine.reason), phEngine.reasonValue);
    }
  }

  // Starts a pH capture into one slot. Never writes NVS and never sets savedThisSession: a held point only
  // becomes a calibration through the Save step (11-06).
  bool startPhCapture(ph::accept::Slot slot, uint16_t nominal)
  {
    if (anyCaptureRunning())
    {
      return false;
    }
    // A re-capture (or a changed picker) replaces that slot, so the old point and any review go first.
    ph::accept::dropSlot(phHeld, slot);
    phReviewValid = false;
    ph::accept::startCapture(phEngine, nominal, config.pollTemperatureC != nullptr ? config.pollTemperatureC() : NAN);
    phCaptureSlot = slot;
    phCaptureStartMs = millis();
    phCaptureStartedInPortal = wm.getConfigPortalActive();
    // The window starts at the request: the next tick samples at once and restarts the 1 s cadence from there.
    phSamplerPrimed = false;
    if (phEngine.state == ph::accept::CaptureState::Refused)
    {
      Serial.printf("[provisioning] pH capture %s refused (%s)\n", ph::accept::slotToken(slot),
                    ph::accept::refusalToken(phEngine.reason));
    }
    else
    {
      Serial.printf("[provisioning] pH capture %s started (nominal %u)\n", ph::accept::slotToken(slot),
                    static_cast<unsigned>(nominal));
    }
    return true;
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
    // A running pH capture also answers busy: one capture at a time across both parameters.
    if (provisioning::turbidityCaptureStatus().state == provisioning::CaptureState::Capturing ||
        ph::accept::isCaptureRunning(phEngine))
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

  float roundTo(float v, float scale)
  {
    return roundf(v * scale) / scale;
  }

  // NAN never reaches the page as a number: it becomes JSON null, which the page renders as a dash.
  void putRounded(JsonDocument &doc, const char *key, float v, float scale)
  {
    if (isfinite(v))
    {
      doc[key] = roundTo(v, scale);
    }
    else
    {
      doc[key] = nullptr;
    }
  }

  void putPhPoint(JsonDocument &doc, const char *key, const ph::accept::Point &p)
  {
    if (!p.held)
    {
      doc[key] = nullptr;
      return;
    }
    JsonObject o = doc[key].to<JsonObject>();
    o["nominal"] = static_cast<unsigned>(p.nominal);
    o["tempC"] = roundTo(p.tempC, 10.0f);
    o["bufPh"] = p.bufHundredths / 100.0;
    o["mv"] = static_cast<unsigned>(p.mv);
  }

  // The stored calibration as the page shows it (D-18/D-19/D-20/D-22): the real buffer pH used, the cup
  // temperature, slope % of the board nominal (not Nernst), offset in pH, verdict and age. 11-06's save response
  // reuses this so the "Saved" line and the status line can never disagree.
  void putPhStored(JsonDocument &doc)
  {
    if (!ph::isValidCalibration(phCal))
    {
      doc["stored"] = nullptr;
      return;
    }
    const ph::Line line = ph::fit(phCal);
    const float slopePct = ph::cal::slopePercentOfNominal(line);
    const float offset = ph::cal::offsetPh(line, ph::accept::BOARD_ZERO_INPUT_SENSOR_MV);
    const ph::accept::Verdict verdict = ph::accept::healthVerdict(slopePct, offset);
    const ph::accept::AgeResult age =
        ph::accept::calibrationAge(phCal.calAt, static_cast<long long>(time(nullptr)));
    JsonObject o = doc["stored"].to<JsonObject>();
    o["calAt"] = static_cast<uint32_t>(phCal.calAt);
    o["bufA"] = phCal.bufA / 100.0;
    o["bufB"] = phCal.bufB / 100.0;
    o["tcalC"] = phCal.tcalCentiC / 100.0;
    if (isfinite(slopePct))
    {
      o["slopePct"] = roundTo(slopePct, 10.0f);
    }
    else
    {
      o["slopePct"] = nullptr;
    }
    if (isfinite(offset))
    {
      o["offsetPh"] = roundTo(offset, 100.0f);
    }
    else
    {
      o["offsetPh"] = nullptr;
    }
    o["verdict"] = ph::accept::verdictToken(verdict);
    o["age"] = ph::accept::ageToken(age.age);
    o["ageDays"] = age.days;
  }

  // A fixed key set and nothing else (T-05-21, T-11-17): the hotspot password is shared across every unit, so
  // this response must never carry the device identity, its secret, the saved network name or the hardware
  // address. Serves cached values only - no burst and no DS18B20 read inside a handler - so polling it costs nothing.
  void handlePhStatus()
  {
    JsonDocument doc;
    if (isfinite(phLatestMv))
    {
      doc["mv"] = lroundf(phLatestMv);
    }
    else
    {
      doc["mv"] = nullptr;
    }
    putRounded(doc, "ph", phLatestPh, 100.0f);
    doc["calibrated"] = ph::isValidCalibration(phCal);
    putRounded(doc, "tempC", phLatestTempC, 10.0f);
    doc["state"] =
        ph::accept::liveStateToken(ph::accept::liveState(phLiveRing, phLiveNext, phLiveFilled));
    putRounded(doc, "driftMv", ph::accept::driftOf(phLiveRing, phLiveNext, phLiveFilled), 10.0f);
    doc["busy"] = anyCaptureRunning();

    const bool idle = phEngine.state == ph::accept::CaptureState::Idle;
    const bool refused = phEngine.state == ph::accept::CaptureState::Refused;
    doc["capture"] = ph::accept::captureStateToken(phEngine.state);
    if (phCaptureSlot == ph::accept::Slot::None)
    {
      doc["slot"] = nullptr;
    }
    else
    {
      doc["slot"] = ph::accept::slotToken(phCaptureSlot);
    }
    doc["samples"] = static_cast<unsigned>(phEngine.filled);
    doc["needed"] = static_cast<unsigned>(ph::cal::CAPTURE_WINDOW_SAMPLES);
    doc["elapsedS"] = static_cast<unsigned>(phEngine.elapsedS);
    doc["timeoutS"] = static_cast<unsigned>(ph::accept::SETTLE_TIMEOUT_S);
    if (refused)
    {
      doc["reason"] = ph::accept::refusalToken(phEngine.reason);
    }
    else
    {
      doc["reason"] = nullptr;
    }
    // A value-initialised engine holds 0, not NAN, so the numbers are only meaningful once a capture has run.
    putRounded(doc, "reasonValue", refused ? phEngine.reasonValue : NAN, 10.0f);
    putRounded(doc, "lastDriftMv", idle ? NAN : phEngine.lastDriftMv, 10.0f);
    putPhPoint(doc, "a", phHeld.a);
    putPhPoint(doc, "b", phHeld.b);
    if (phReviewValid)
    {
      JsonObject r = doc["review"].to<JsonObject>();
      r["slopePct"] = roundTo(phReview.slopePct, 10.0f);
      r["offsetPh"] = roundTo(phReview.offsetPh, 100.0f);
      r["verdict"] = ph::accept::verdictToken(phReview.verdict);
    }
    else
    {
      doc["review"] = nullptr;
    }
    putPhStored(doc);
    doc["overdueDays"] = static_cast<unsigned>(ph::accept::OVERDUE_AFTER_DAYS);
    sendJson(200, doc);
  }

  // T-11-18: lengths are capped before parsing and both values go through exact-string whitelists; the response
  // is a token, never the raw input.
  void handlePhCapture()
  {
    JsonDocument doc;
    const String slotArg = wm.server->hasArg("slot") ? wm.server->arg("slot") : String();
    const String nominalArg = wm.server->hasArg("nominal") ? wm.server->arg("nominal") : String();
    const ph::accept::Slot slot =
        slotArg.length() > 1 ? ph::accept::Slot::None : ph::accept::parseSlot(slotArg.c_str());
    const uint16_t nominal = nominalArg.length() > 4 ? 0 : ph::accept::parseNominal(nominalArg.c_str());
    if (slot == ph::accept::Slot::None || nominal == 0)
    {
      Serial.println("[provisioning] pH capture bad_request");
      doc["result"] = "bad_request";
      sendJson(400, doc);
      return;
    }
    if (!startPhCapture(slot, nominal))
    {
      Serial.println("[provisioning] pH capture busy");
      doc["result"] = "busy";
      sendJson(409, doc);
      return;
    }
    doc["capture"] = ph::accept::captureStateToken(phEngine.state);
    sendJson(202, doc);
  }

  void handlePhPage()
  {
    wm.server->sendHeader("Cache-Control", "no-store");
    wm.server->send(200, "text/html; charset=utf-8", provisioning::phpage::PH_PAGE_HTML);
  }

  // The client only names the two buffers it is looking at; the points themselves come from the unit's own
  // captures held in RAM, so no request can forge a calibration (D-01, T-11-24). The posted nominals exist only
  // to catch a stale review: a picker changed after capturing is refused as "changed", not saved (D-24, T-11-25).
  // A refusal here, or the door's own validation refusing, leaves the calibration in effect untouched (D-14).
  // Calibrating never restarts the unit.
  void handlePhSave()
  {
    JsonDocument doc;
    if (anyCaptureRunning())
    {
      doc["result"] = "busy";
      sendJson(409, doc);
      return;
    }
    const String aArg = wm.server->hasArg("a") ? wm.server->arg("a") : String();
    const String bArg = wm.server->hasArg("b") ? wm.server->arg("b") : String();
    const uint16_t pa = aArg.length() > 4 ? 0 : ph::accept::parseNominal(aArg.c_str());
    const uint16_t pb = bArg.length() > 4 ? 0 : ph::accept::parseNominal(bArg.c_str());
    if (pa == 0 || pb == 0)
    {
      Serial.println("[provisioning] pH save bad_request");
      doc["result"] = "bad_request";
      sendJson(400, doc);
      return;
    }
    const ph::accept::PairResult r = ph::accept::checkSave(phHeld, pa, pb);
    if (r.reason != ph::accept::Refusal::None)
    {
      const char *token = ph::accept::refusalToken(r.reason);
      Serial.printf("[provisioning] pH save refused (%s)\n", token);
      doc["result"] = token;
      sendJson(409, doc);
      return;
    }
    // version and calAt are the door's to set.
    const ph::Calibration points{r.cal.mvA, r.cal.bufA, r.cal.mvB, r.cal.bufB, r.cal.tcalCentiC, 0, 0};
    if (!provisioning::storePhCalibration(points))
    {
      // The held points stay so the admin can retry; the door has already logged why.
      doc["result"] = "nvs";
      sendJson(500, doc);
      return;
    }
    phHeld = ph::accept::HeldPair{};
    phReview = ph::accept::PairResult{};
    phReviewValid = false;
    phEngine = ph::accept::CaptureEngine{};
    phCaptureSlot = ph::accept::Slot::None;
    doc["result"] = "saved";
    putPhStored(doc);
    sendJson(200, doc);
  }

  // Held points are not a calibration, so clearing leaves them alone; the admin can still save the pair.
  void handlePhClear()
  {
    JsonDocument doc;
    if (anyCaptureRunning())
    {
      doc["result"] = "busy";
      sendJson(409, doc);
      return;
    }
    if (!provisioning::clearPhCalibration())
    {
      doc["result"] = "nvs";
      sendJson(500, doc);
      return;
    }
    doc["result"] = "cleared";
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
    // store a value (T-05-18). The pH routes follow the same rule (T-11-19).
    wm.server->on("/cal", HTTP_GET, handleCalPage);
    wm.server->on("/cal/status", HTTP_GET, handleCalStatus);
    wm.server->on("/cal/capture", HTTP_POST, handleCalCapture);
    wm.server->on("/cal/set", HTTP_POST, handleCalSet);
    wm.server->on("/ph", HTTP_GET, handlePhPage);
    wm.server->on("/ph/status", HTTP_GET, handlePhStatus);
    wm.server->on("/ph/capture", HTTP_POST, handlePhCapture);
    wm.server->on("/ph/save", HTTP_POST, handlePhSave);
    wm.server->on("/ph/clear", HTTP_POST, handlePhClear);
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

    loadPhCalibration();
    // SENS-09: one line, only when unusable, for the same reason as turbidity's — on the dashboard an
    // uncalibrated pH and a dead pH board both show no value, and the boot log is where an admin tells them apart.
    if (!ph::isValidCalibration(phCal))
    {
      Serial.println("[provisioning] pH uncalibrated: no valid buffer calibration stored");
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
    // "custom" slot holding the two Calibrate buttons (turbidity and pH). Listing "param" still keeps the Setup
    // page separate from the WiFi page. There is deliberately no "update", "erase" or "restart": update is web
    // OTA, and all three are also blocked at the route level in registerCalibrationRoutes(). Calibrating either
    // sensor never sets savedThisSession, so it never restarts the unit (CAL-07).
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

  ph::Calibration phCalibration()
  {
    return phCal;
  }

  bool storePhCalibration(const ph::Calibration &points)
  {
    ph::Calibration next = points;
    next.version = ph::PH_CAL_VERSION;
    next.calAt = turbidity::cal::calibrationStamp(static_cast<long long>(time(nullptr)));

    // Validated before any write: this is the only door into the keys, so a swapped buffer pair or a misread
    // capture is refused here rather than persisted. The loader's re-validation covers flash from older builds.
    const ph::cal::CalCheck check = ph::cal::checkCalibration(next);
    if (check != ph::cal::CalCheck::Ok)
    {
      Serial.printf("[provisioning] pH calibration NOT saved: %s\n", ph::cal::calCheckToken(check));
      return false;
    }

    if (!prefs.begin("unit", false))
    {
      Serial.println("[provisioning] pH calibration NOT saved: could not open NVS");
      return false;
    }

    // NVS only guarantees that each single key/value pair survives a power cut, not a group of them. ph_ver is
    // therefore the commit marker: removed FIRST, so the old version cannot vouch for a half-new set of points,
    // and written LAST, only after every other key landed. Any failure in between returns with ph_ver absent,
    // which the loader reads as version 0 = uncalibrated — the honest state for a calibration that did not finish.
    // remove() returning false just means the key was not there (first calibration), which is fine.
    prefs.remove("ph_ver");
    // The RAM copy goes uncalibrated at the same moment, so this run never reports from points that are no
    // longer what flash says.
    phCal = ph::Calibration{};
    applyPhCalibrationLive();

    const bool pointsWritten = prefs.putUShort("ph_mv_a", next.mvA) == sizeof(uint16_t) &&
                               prefs.putUShort("ph_buf_a", next.bufA) == sizeof(uint16_t) &&
                               prefs.putUShort("ph_mv_b", next.mvB) == sizeof(uint16_t) &&
                               prefs.putUShort("ph_buf_b", next.bufB) == sizeof(uint16_t) &&
                               prefs.putShort("ph_tcal", next.tcalCentiC) == sizeof(int16_t) &&
                               prefs.putUInt("ph_cal_at", next.calAt) == sizeof(uint32_t);
    if (!pointsWritten)
    {
      prefs.end();
      Serial.println("[provisioning] pH calibration NOT saved: NVS write failed (unit left uncalibrated)");
      return false;
    }
    if (prefs.putUChar("ph_ver", next.version) != sizeof(uint8_t))
    {
      prefs.end();
      Serial.println("[provisioning] pH calibration NOT saved: NVS commit marker write failed (unit left uncalibrated)");
      return false;
    }
    prefs.end();

    phCal = next;
    Serial.println("[provisioning] pH calibration saved");
    if (next.calAt == 0)
    {
      Serial.println("[provisioning] pH calibration date unknown: clock not set");
    }
    // Applied inside the door, so no caller can store a calibration and forget to make it live.
    applyPhCalibrationLive();
    return true;
  }

  bool clearPhCalibration()
  {
    if (!prefs.begin("unit", false))
    {
      Serial.println("[provisioning] pH calibration NOT cleared: could not open NVS");
      return false;
    }
    // Commit marker first: once ph_ver is gone the unit is uncalibrated even if a cut stops the rest.
    prefs.remove("ph_ver");
    prefs.remove("ph_mv_a");
    prefs.remove("ph_buf_a");
    prefs.remove("ph_mv_b");
    prefs.remove("ph_buf_b");
    prefs.remove("ph_tcal");
    prefs.remove("ph_cal_at");
    prefs.end();

    phCal = ph::Calibration{};
    Serial.println("[provisioning] pH calibration cleared");
    applyPhCalibrationLive();
    return true;
  }

  bool startTurbidityCapture()
  {
    // Both captures share the 1 Hz cadence and the admin's attention: one capture at a time, pH or turbidity.
    if (captureState == CaptureState::Capturing || ph::accept::isCaptureRunning(phEngine))
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
    tickPhSampler();

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
