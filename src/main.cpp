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

// Serialized into every signed body, so a bump here invalidates every golden signature at once: it only ever
// moves together with regenerated vectors (backend `npm run generate:signing-vectors`, then
// `node scripts/sync-golden-vectors.mjs`). 0.4.0 is the first version that can report turbidity; 0.5.0 also
// reports wifiSsid. That is how Device.firmwareVersion tells the backend which units can.
static const char *FIRMWARE_VERSION = "0.5.0";
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;

static unsigned long lastReadingMs = 0;
static bool readOnce = false;
static bool uplinkStarted = false;

#if TURBIDITY_BENCH
static void benchPump();
#endif

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
#if TURBIDITY_BENCH
    // This wait can last 15 s per uplink cycle on a provisioned unit with no reachable network, which is the
    // normal state of an unplugged protocol run. Pumping the bench logger here is what keeps the CSV at its own
    // 1 s cadence instead of losing about half of every cycle. The 250 ms delay bounds the granularity.
    benchPump();
#endif
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

#if TURBIDITY_BENCH
// Everything between here and the matching #endif is a bench instrument, not firmware a pond ever runs.
// `cal capture` and `cal set` let anyone holding a USB cable rewrite this unit's clear-water reference, which
// silently rescales every NTU it reports afterwards, so the whole block is compiled only by
// [env:nodemcu-32s-bench] (-DTURBIDITY_BENCH=1) and must never ship. Nothing here prints a device secret or a
// signed payload: serial is unencrypted, and the CSV carries millivolts, spread, NTU and the stored reference
// only.

// Printed once at boot so a bench unit announces itself, and greppable out of the shipping firmware.elf -
// `strings .pio/build/nodemcu-32s/firmware.elf | grep -c 'turbidity-bench'` must be 0, which is what proves
// the guard holds rather than merely looks right.
static const char *BENCH_MARKER = "turbidity-bench";

// The CSV cadence, deliberately its own timer and NOT the reporting interval below. The bench protocol logs
// about 5 minutes per water point and about 2 minutes per unplugged run; at 1 s that is ~300 and ~120 rows,
// which is what bench/README.md and the fit script's minimum-row check both assume - move this constant and
// those two move with it. At the shipping 30 s cadence the same runs would yield ~10 and ~4 rows, far too few
// for the standard deviation the noise floor is defined as. One burst is 64 one-shot reads about 1 ms apart,
// roughly 64 ms, so this uses well under a tenth of each second and leaves uplink::loop() running normally.
static const unsigned long BENCH_SAMPLE_INTERVAL_MS = 1000UL;

static unsigned long lastBenchSampleMs = 0;
static bool benchSampledOnce = false;
static char benchCommandBuf[48];
static size_t benchCommandLen = 0;

// Single writer of the calibration from this console. Validation is not repeated here on purpose:
// provisioning::storeTurbidityClearWaterMv() owns the plausible-window check and already logs its reason, so
// a refusal leaves both NVS and the in-memory reference untouched. On success the running unit is told too,
// so a technician can capture and then immediately see live NTU without a reboot.
static void benchStoreCalibration(uint16_t clearWaterMv, const char *source)
{
  if (!provisioning::storeTurbidityClearWaterMv(clearWaterMv))
  {
    Serial.printf("[bench] %s refused %u mV, nothing stored (the [provisioning] line above says why)\n", source, clearWaterMv);
    return;
  }
  sensors::setTurbidityCalibration(clearWaterMv);
  Serial.printf("[bench] %s stored clear-water reference %u mV\n", source, clearWaterMv);
}

static void benchShowCalibration()
{
  uint16_t stored = provisioning::turbidityClearWaterMv();
  char storedText[16];
  if (stored == 0)
  {
    strncpy(storedText, "uncalibrated", sizeof(storedText) - 1);
    storedText[sizeof(storedText) - 1] = '\0';
  }
  else
  {
    snprintf(storedText, sizeof(storedText), "%u", stored);
  }

  // This read refreshes all five diagnostics fields - including ntu, which Sensors computes inside the same
  // burst. The NTU is read back from there rather than worked out here: main.cpp never does turbidity
  // arithmetic of its own, so the number shown is provably the number that burst produced.
  float currentSensorMv = sensors::readTurbidityMillivolts();
  TurbidityDiagnostics diagnostics = sensors::lastTurbidityDiagnostics();
  Serial.printf(
      "[bench] cal show: clear_mv=%s current_sensor_mv=%.1f raw_mean_mv=%.1f filtered_pin_mv=%.1f spread_mv=%.1f sensor_mv=%.1f ntu=%.1f\n",
      storedText,
      currentSensorMv,
      diagnostics.rawMeanMv,
      diagnostics.filteredPinMv,
      diagnostics.spreadMv,
      diagnostics.sensorMv,
      diagnostics.ntu);
}

static void benchCaptureCalibration()
{
  float sensorMv = sensors::readTurbidityMillivolts();
  if (isnan(sensorMv))
  {
    Serial.println("[bench] cal capture refused: no turbidity reading (pin under the fault floor - check the 5 V supply and the divider)");
    return;
  }
  long rounded = lroundf(sensorMv);
  if (rounded < 0 || rounded > 65535)
  {
    Serial.printf("[bench] cal capture refused: %.1f mV does not fit the stored 16-bit value\n", sensorMv);
    return;
  }
  benchStoreCalibration(static_cast<uint16_t>(rounded), "cal capture");
}

static void benchSetCalibration(const char *argument)
{
  while (*argument == ' ' || *argument == '\t')
  {
    ++argument;
  }
  char *parseEnd = nullptr;
  long parsed = strtol(argument, &parseEnd, 10);
  if (parseEnd == argument || *parseEnd != '\0')
  {
    Serial.printf("[bench] cal set refused: '%s' is not a whole number of millivolts\n", argument);
    return;
  }
  if (parsed < 0 || parsed > 65535)
  {
    Serial.printf("[bench] cal set refused: %ld is outside the stored 16-bit range\n", parsed);
    return;
  }
  benchStoreCalibration(static_cast<uint16_t>(parsed), "cal set");
}

static void benchDispatchCommand(char *command)
{
  // Trim trailing blanks so a monitor that appends one doesn't turn a good command into an unknown one.
  size_t length = strlen(command);
  while (length > 0 && (command[length - 1] == ' ' || command[length - 1] == '\t'))
  {
    command[--length] = '\0';
  }

  if (strcmp(command, "cal show") == 0)
  {
    benchShowCalibration();
  }
  else if (strcmp(command, "cal capture") == 0)
  {
    benchCaptureCalibration();
  }
  else if (strncmp(command, "cal set", 7) == 0)
  {
    benchSetCalibration(command + 7);
  }
  else
  {
    Serial.printf("[bench] unknown command: %s\n", command);
  }
}

// Drains whatever bytes have already arrived and returns. No blocking read of any kind: loop() also drives
// uplink::loop() and the MQTT keepalive, so a stall here drops PUBACKs and the buffered batch gets republished
// for no reason.
static void benchPollSerial()
{
  while (Serial.available() > 0)
  {
    int incoming = Serial.read();
    if (incoming < 0)
    {
      return;
    }
    char c = static_cast<char>(incoming);
    if (c == '\n' || c == '\r')
    {
      if (benchCommandLen > 0)
      {
        benchCommandBuf[benchCommandLen] = '\0';
        benchDispatchCommand(benchCommandBuf);
      }
      benchCommandLen = 0;
      continue;
    }
    if (benchCommandLen + 1 >= sizeof(benchCommandBuf))
    {
      Serial.println("[bench] command too long, discarded");
      benchCommandLen = 0;
      continue;
    }
    benchCommandBuf[benchCommandLen++] = c;
  }
}

// One line per burst, columns exactly as the header printed in setup(). %.1f everywhere so a missing value
// prints as `nan` and the fit script can tell it apart from a real 0. This is the one place the "one concise
// line per reading" rule is deliberately broken, and it exists only in this environment.
static void benchLogSample()
{
  sensors::readTurbidityMillivolts();
  TurbidityDiagnostics diagnostics = sensors::lastTurbidityDiagnostics();
  Serial.printf(
      "[bench] csv,%lu,%.1f,%.1f,%.1f,%.1f,%.1f,%u\n",
      millis(),
      diagnostics.rawMeanMv,
      diagnostics.filteredPinMv,
      diagnostics.spreadMv,
      diagnostics.sensorMv,
      diagnostics.ntu,
      provisioning::turbidityClearWaterMv());
}

// The single place the CSV cadence is decided, shared by loop() and by ensureWiFi()'s connect wait so the two
// callers cannot drift apart.
static void benchPump()
{
  benchPollSerial();
  if (!benchSampledOnce || millis() - lastBenchSampleMs >= BENCH_SAMPLE_INTERVAL_MS)
  {
    benchSampledOnce = true;
    lastBenchSampleMs = millis();
    benchLogSample();
  }
}
#endif

void setup()
{
  Serial.begin(115200);
  sensors::begin();
  provisioning::begin(provisioning::Config{
      SETUP_AP_PASSWORD,
  });

  // After provisioning::begin(), never before it: Provisioning owns the "unit" NVS namespace and only reads
  // turb_clear_mv in there, so the value does not exist yet while sensors::begin() runs. Handing it across
  // here is also what keeps Sensors free of any NVS call of its own — it stays a hardware reader that is
  // told its reference rather than one that goes looking for it, and this file opens no NVS namespace
  // either. Moving this line above provisioning::begin() would
  // compile, run and log nothing, and every unit in the field would silently report no turbidity at all.
  sensors::setTurbidityCalibration(provisioning::turbidityClearWaterMv());

#if TURBIDITY_BENCH
  Serial.printf("[bench] %s build, commands: cal show | cal capture | cal set <mV>\n", BENCH_MARKER);
  Serial.println("[bench] csv,millis,raw_mean_mv,filtered_pin_mv,spread_mv,sensor_mv,ntu,clear_mv");
#endif
}

void loop()
{
#if TURBIDITY_BENCH
  // Both of these sit above the provisioning gate on purpose. Several protocol runs - the unplugged ones
  // especially - have no reason to be online, and a bench unit that is unprovisioned, off WiFi or still
  // waiting on NTP must keep logging. The sampling branch further down is untouched: this unit still
  // publishes on the normal schedule.
  benchPump();
#endif

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
