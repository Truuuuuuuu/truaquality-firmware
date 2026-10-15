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
// reports wifiSsid; 0.6.0 also reports diag and per-sensor status; 0.6.1 windows turbidity across the interval;
// 0.7.0 also reports ph and sensors.ph (Phase 10). That is how Device.firmwareVersion tells the backend which
// units can. Per D-01 the bump and the vectors land together: the 0.7.0 pH vectors were already signed in Phase 9
// and test_wireformat keeps its own version constants, so this change proves sync with
// `node scripts/sync-golden-vectors.mjs --check` (regenerate from the backend only on drift, never hand-edit).
#if PH_DEMO_ANCHOR
static const char *FIRMWARE_VERSION = "0.7.0-ph-demo";
#else
static const char *FIRMWARE_VERSION = "0.7.0";
#endif
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;
static const unsigned long NTP_RETRY_INTERVAL_MS = 1000;
static const unsigned long NTP_WAIT_LOG_INTERVAL_MS = 10000;

static unsigned long lastReadingMs = 0;
static bool readOnce = false;
static bool samplePending = false;
static unsigned long lastNtpCheckMs = 0;
static unsigned long lastNtpLogMs = 0;
static bool uplinkStarted = false;

#if TURBIDITY_BENCH
// The capture window, buffer table and printout helpers the pH console uses. Bench-only, so the shipping image
// never even parses them; the arithmetic all lives there, main.cpp only calls it.
#include <math.h>
#include <time.h>

#include "PhCalibration.h"

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

// The pH console's own marker, for the same reason: the `ph ...` commands below rewrite the unit's pH line over
// USB (which silently shifts every pH it reports afterwards), so `strings .pio/build/nodemcu-32s/firmware.elf |
// grep -c 'ph-bench'` must be 0 and the bench ELF's count at least 1. A second string rather than reusing the
// turbidity one, so a future split of the two consoles cannot leave either half unproven.
static const char *PH_BENCH_MARKER = "ph-bench";

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
// a refusal leaves both NVS and the in-memory reference untouched. On success that same door stamps the date
// and applies the value to the running sensor code, so live NTU follows at once without a reboot.
static void benchStoreCalibration(uint16_t clearWaterMv, const char *source)
{
  if (!provisioning::storeTurbidityClearWaterMv(clearWaterMv))
  {
    Serial.printf("[bench] %s refused %u mV, nothing stored (the [provisioning] line above says why)\n", source, clearWaterMv);
    return;
  }
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

// Set while a `cal capture` from this console is running, so benchPump() prints its outcome exactly once.
static bool benchCaptureWatching = false;

// D-02: the same 20-sample capture the portal's Calibrate screen runs, not a single burst of its own - the bench
// and the portal must never disagree about what a capture is. Asynchronous: benchPump() reports the result.
static void benchCaptureCalibration()
{
  if (!provisioning::startTurbidityCapture())
  {
    Serial.println("[bench] cal capture refused: a capture is already running");
    return;
  }
  Serial.println("[bench] cal capture started: hold the probe still in clean water, 20 samples over about 20 s");
  benchCaptureWatching = true;
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

// ---- pH console (D-16, D-18) -------------------------------------------------------------------------------
// Calibration here is the bench's only path until Phase 11 puts it in the portal. Every write goes through
// provisioning::storePhCalibration / clearPhCalibration (D-17), the one door that validates, stamps the version and
// date, uses ph_ver as the commit marker and applies the line live, so this console never opens NVS itself. NVS
// survives reflashing to the normal image, which is the point: calibrate here, reflash, deploy.

// The DS18B20 temperature the phcsv rows (and `ph show`) hand to the pH conversion. Refreshed only every
// BENCH_TEMP_EVERY_ROWS rows: the DS18B20 conversion blocks ~750 ms, so reading it every second would eat almost
// the whole 1 s budget and the CSV would drift off its cadence. Buffer temperature moves far slower than that.
static const unsigned long BENCH_TEMP_EVERY_ROWS = 10;
static float benchTempC = NAN;
static unsigned long benchPhRowCount = 0;

// One staged buffer point. RAM only: nothing touches NVS until `ph save`, so an aborted or refused capture leaves
// the stored calibration exactly as it was.
struct BenchPhPoint
{
  bool staged;
  uint16_t mv;            // median sensor-side mV over the capture window, rounded
  uint16_t bufHundredths; // the buffer's real pH at the capture temperature (D-11), not the printed nominal
  float tempC;            // mean of the start and end DS18B20 readings
  float spreadMv;         // max - min over the window: the point's noise, printed again at save
};

static BenchPhPoint benchPhPointA = {false, 0, 0, NAN, NAN};
static BenchPhPoint benchPhPointB = {false, 0, 0, NAN, NAN};

// The capture in flight, fed one sample per phcsv row by benchLogSample(), so it costs no extra bursts and the 20
// samples are provably the same numbers the CSV shows for those 20 seconds.
struct BenchPhCapture
{
  bool active;
  char point; // 'a' or 'b'
  uint16_t nominalHundredths;
  float startTempC;
  size_t count;
  float samples[ph::cal::CAPTURE_WINDOW_SAMPLES];
};

static BenchPhCapture benchPhCapture = {false, 'a', 0, NAN, 0, {0}};

// WiFi.getMode() named as the fit script's RADIO_STATE_PATTERN expects, plus "-conn" while the station is
// associated, so radio-state runs (the B10 gate) are labelled by the firmware rather than by the operator's memory.
static const char *benchRadioState()
{
  const bool connected = WiFi.status() == WL_CONNECTED;
  switch (WiFi.getMode())
  {
  case WIFI_MODE_STA:
    return connected ? "sta-conn" : "sta";
  case WIFI_MODE_AP:
    return connected ? "ap-conn" : "ap";
  case WIFI_MODE_APSTA:
    return connected ? "ap+sta-conn" : "ap+sta";
  default:
    return connected ? "off-conn" : "off";
  }
}

static const char *benchPhStatusToken(ph::Status status)
{
  switch (status)
  {
  case ph::Status::Ok:
    return "ok";
  case ph::Status::NoSignal:
    return "no_signal";
  case ph::Status::Uncalibrated:
    return "uncalibrated";
  case ph::Status::OverRange:
    return "over_range";
  }
  return "no_signal";
}

static const char *benchSkipBlanks(const char *cursor)
{
  while (*cursor == ' ' || *cursor == '\t')
  {
    ++cursor;
  }
  return cursor;
}

// Argument parsers for untrusted serial text (T-10-06-02). Each consumes one whitespace-delimited token and
// refuses anything the end pointer shows was only partly a number ("6.8x", "12abc"), or a float that is not
// finite (strtof happily accepts "nan" and "inf").
static bool benchParseLong(const char *&cursor, long &out)
{
  cursor = benchSkipBlanks(cursor);
  char *parseEnd = nullptr;
  const long parsed = strtol(cursor, &parseEnd, 10);
  if (parseEnd == cursor || (*parseEnd != '\0' && *parseEnd != ' ' && *parseEnd != '\t'))
  {
    return false;
  }
  out = parsed;
  cursor = parseEnd;
  return true;
}

static bool benchParseFloat(const char *&cursor, float &out)
{
  cursor = benchSkipBlanks(cursor);
  char *parseEnd = nullptr;
  const float parsed = strtof(cursor, &parseEnd);
  if (parseEnd == cursor || (*parseEnd != '\0' && *parseEnd != ' ' && *parseEnd != '\t') || !isfinite(parsed))
  {
    return false;
  }
  out = parsed;
  cursor = parseEnd;
  return true;
}

static bool benchAtEnd(const char *cursor)
{
  return *benchSkipBlanks(cursor) == '\0';
}

// Prints a calibration's fit the way the bench record wants it. Signed slope, because the sign is the polarity
// check; "% of DFRobot nominal" and not a Nernst %, because this board's gain is unpublished (PhCalibration.h).
static void benchPrintFit(const ph::Calibration &cal)
{
  if (cal.bufA == cal.bufB)
  {
    Serial.println("[bench] ph fit: none (both points use the same buffer)");
    return;
  }
  const ph::Line line = ph::fit(cal);
  Serial.printf("[bench] ph fit: slope=%.2f mV/pH slope_pct=%.1f %% of DFRobot nominal (not a Nernst %%) mv_at_ph7=%.1f tcal_c=%.2f\n",
                line.slopeMvPerPh, ph::cal::slopePercentOfNominal(line), line.mvAtIsopotential, line.tcalC);
}

static void benchShowPh()
{
  const float sensorMv = sensors::readPhMillivolts(benchTempC);
  const PhDiagnostics diagnostics = sensors::lastPhDiagnostics();
  const ph::Calibration stored = provisioning::phCalibration();
  // The status comes from ph::classify on the very burst just taken, the same decision Sensors makes for the
  // wire, so the token shown here can never disagree with what this unit would report.
  const ph::Burst burst = {diagnostics.rawMeanMv, diagnostics.filteredPinMv, diagnostics.spreadMv};
  const ph::Status status = ph::classify(burst, stored, diagnostics.tempUsedC);
  Serial.printf("[bench] ph show: pin_mv=%.1f sensor_mv=%.1f spread_mv=%.1f temp_used_c=%.2f ph=%.2f status=%s\n",
                diagnostics.filteredPinMv, sensorMv, diagnostics.spreadMv, diagnostics.tempUsedC, diagnostics.ph,
                benchPhStatusToken(status));

  if (stored.version == 0 && stored.mvA == 0 && stored.mvB == 0)
  {
    Serial.println("[bench] ph cal: none stored (unit reports uncalibrated)");
    return;
  }
  char dateText[24];
  if (stored.calAt == 0)
  {
    strncpy(dateText, "unknown", sizeof(dateText) - 1);
    dateText[sizeof(dateText) - 1] = '\0';
  }
  else
  {
    const time_t calAt = static_cast<time_t>(stored.calAt);
    struct tm utc;
    gmtime_r(&calAt, &utc);
    strftime(dateText, sizeof(dateText), "%Y-%m-%dT%H:%M:%SZ", &utc);
  }
  Serial.printf("[bench] ph cal: a=%u mV @ pH %.2f, b=%u mV @ pH %.2f, tcal_c=%.2f, date=%s, version=%u, check=%s\n",
                stored.mvA, stored.bufA / 100.0f, stored.mvB, stored.bufB / 100.0f, stored.tcalCentiC / 100.0f,
                dateText, static_cast<unsigned>(stored.version),
                ph::cal::calCheckToken(ph::cal::checkCalibration(stored)));
  benchPrintFit(stored);
}

static void benchPhCaptureStart(const char *argument)
{
  argument = benchSkipBlanks(argument);
  const char point = *argument;
  if ((point != 'a' && point != 'b') || (argument[1] != ' ' && argument[1] != '\t'))
  {
    Serial.println("[bench] ph capture refused: usage is ph capture a|b <4.01|6.86|9.18>");
    return;
  }
  const char *cursor = argument + 1;
  float nominal = NAN;
  if (!benchParseFloat(cursor, nominal) || !benchAtEnd(cursor))
  {
    Serial.println("[bench] ph capture refused: the buffer nominal is not a number (use 4.01, 6.86 or 9.18)");
    return;
  }
  const long nominalHundredths = lroundf(nominal * 100.0f);
  if (nominalHundredths != 401 && nominalHundredths != 686 && nominalHundredths != 918)
  {
    Serial.printf("[bench] ph capture refused: %.2f is not a buffer this unit knows (4.01, 6.86, 9.18)\n", nominal);
    return;
  }
  if (benchPhCapture.active)
  {
    Serial.println("[bench] ph capture refused: a pH capture is already running");
    return;
  }

  // D-11: the DS18B20 sits in the buffer, and the buffer's real pH depends on that temperature. Checked now as
  // well as at the end, so a missing probe or a cup outside the table costs the operator nothing, not 20 s.
  const float startTempC = sensors::readTemperatureCelsius();
  if (isnan(startTempC))
  {
    Serial.println("[bench] ph capture refused: buffer temperature unavailable (DS18B20 fault), nothing staged");
    return;
  }
  if (isnan(ph::cal::bufferPhAt(static_cast<uint16_t>(nominalHundredths), startTempC)))
  {
    Serial.printf("[bench] ph capture refused: buffer at %.2f C is outside the 20..35 C buffer table, nothing staged\n",
                  startTempC);
    return;
  }

  benchPhCapture.active = true;
  benchPhCapture.point = point;
  benchPhCapture.nominalHundredths = static_cast<uint16_t>(nominalHundredths);
  benchPhCapture.startTempC = startTempC;
  benchPhCapture.count = 0;
  Serial.printf("[bench] ph capture %c started: pH %.2f buffer at %.2f C, hold the probe still, %u samples over about %u s\n",
                point, nominal, startTempC, static_cast<unsigned>(ph::cal::CAPTURE_WINDOW_SAMPLES),
                static_cast<unsigned>(ph::cal::CAPTURE_WINDOW_SAMPLES));
}

// Fed the sensor mV of every phcsv row while a capture runs. A NAN sample ends it at once: no signal means the
// probe or board is not delivering, and a median over the rest would hide exactly that (T-10-06-04).
static void benchPhCaptureFeed(float sensorMv)
{
  if (!benchPhCapture.active)
  {
    return;
  }
  if (isnan(sensorMv))
  {
    benchPhCapture.active = false;
    Serial.printf("[bench] ph capture %c refused (no_signal): sample %u of %u had no signal, nothing staged\n",
                  benchPhCapture.point, static_cast<unsigned>(benchPhCapture.count + 1),
                  static_cast<unsigned>(ph::cal::CAPTURE_WINDOW_SAMPLES));
    return;
  }
  benchPhCapture.samples[benchPhCapture.count++] = sensorMv;
  if (benchPhCapture.count < ph::cal::CAPTURE_WINDOW_SAMPLES)
  {
    return;
  }

  benchPhCapture.active = false;
  const char point = benchPhCapture.point;
  const float endTempC = sensors::readTemperatureCelsius();
  if (isnan(endTempC))
  {
    Serial.printf("[bench] ph capture %c refused: buffer temperature unavailable at the end, nothing staged\n", point);
    return;
  }
  const float meanTempC = (benchPhCapture.startTempC + endTempC) / 2.0f;
  const float bufferPh = ph::cal::bufferPhAt(benchPhCapture.nominalHundredths, meanTempC);
  if (isnan(bufferPh))
  {
    Serial.printf("[bench] ph capture %c refused: mean buffer temperature %.2f C is outside the 20..35 C table, nothing staged\n",
                  point, meanTempC);
    return;
  }
  const float medianMv = ph::cal::medianOf(benchPhCapture.samples, benchPhCapture.count);
  const float spreadMv = ph::cal::spreadOf(benchPhCapture.samples, benchPhCapture.count);
  const long roundedMv = lroundf(medianMv);
  if (isnan(medianMv) || roundedMv <= 0 || roundedMv > 65535)
  {
    Serial.printf("[bench] ph capture %c refused: median %.1f mV is not a storable reading, nothing staged\n", point,
                  medianMv);
    return;
  }

  BenchPhPoint &target = point == 'a' ? benchPhPointA : benchPhPointB;
  target.staged = true;
  target.mv = static_cast<uint16_t>(roundedMv);
  target.bufHundredths = static_cast<uint16_t>(lroundf(bufferPh * 100.0f));
  target.tempC = meanTempC;
  target.spreadMv = spreadMv;
  Serial.printf("[bench] ph capture %c staged: median %u mV, spread %.1f mV over %u samples, mean temp %.2f C, buffer pH used %.2f (nominal %.2f), not saved yet\n",
                point, target.mv, spreadMv, static_cast<unsigned>(ph::cal::CAPTURE_WINDOW_SAMPLES), meanTempC,
                target.bufHundredths / 100.0f, benchPhCapture.nominalHundredths / 100.0f);
}

// Both writers (`ph save`, `ph set`) call provisioning::storePhCalibration themselves and hand the outcome here.
// The door has already refused (and logged why) on anything checkCalibration rejects; on success it has set version
// and date and applied the line live, so what is printed back is read from provisioning, i.e. what was stored.
static bool benchPhReportStore(bool stored, const ph::Calibration &points, const char *source)
{
  if (!stored)
  {
    Serial.printf("[bench] %s refused: %s, nothing stored (the [provisioning] line above says why)\n", source,
                  ph::cal::calCheckToken(ph::cal::checkCalibration(ph::Calibration{
                      points.mvA, points.bufA, points.mvB, points.bufB, points.tcalCentiC, 0, ph::PH_CAL_VERSION})));
    return false;
  }
  const ph::Calibration saved = provisioning::phCalibration();
  Serial.printf("[bench] %s stored: a=%u mV @ pH %.2f, b=%u mV @ pH %.2f, tcal_c=%.2f\n", source, saved.mvA,
                saved.bufA / 100.0f, saved.mvB, saved.bufB / 100.0f, saved.tcalCentiC / 100.0f);
  benchPrintFit(saved);
  return true;
}

static void benchPhSave(const char *argument)
{
  bool haveShorted = false;
  float shortedMv = NAN;
  if (!benchAtEnd(argument))
  {
    const char *cursor = argument;
    if (!benchParseFloat(cursor, shortedMv) || !benchAtEnd(cursor))
    {
      Serial.println("[bench] ph save refused: usage is ph save [shortedMv]");
      return;
    }
    haveShorted = true;
  }
  if (!benchPhPointA.staged || !benchPhPointB.staged)
  {
    Serial.printf("[bench] ph save refused: point a %s, point b %s - capture both first\n",
                  benchPhPointA.staged ? "staged" : "missing", benchPhPointB.staged ? "staged" : "missing");
    return;
  }
  const ph::Calibration points = {
      benchPhPointA.mv,
      benchPhPointA.bufHundredths,
      benchPhPointB.mv,
      benchPhPointB.bufHundredths,
      static_cast<int16_t>(lroundf((benchPhPointA.tempC + benchPhPointB.tempC) / 2.0f * 100.0f)),
      0,
      0,
  };
  if (!benchPhReportStore(provisioning::storePhCalibration(points), points, "ph save"))
  {
    return;
  }
  Serial.printf("[bench] ph save noise: a_spread_mv=%.1f b_spread_mv=%.1f\n", benchPhPointA.spreadMv,
                benchPhPointB.spreadMv);
  if (haveShorted)
  {
    const ph::Line line = ph::fit(provisioning::phCalibration());
    Serial.printf("[bench] ph save offset: %.3f pH (shorted level %.1f mV vs fitted pH 7 at %.1f mV)\n",
                  ph::cal::offsetPh(line, shortedMv), shortedMv, line.mvAtIsopotential);
  }
  benchPhPointA.staged = false;
  benchPhPointB.staged = false;
}

// Manual re-entry from 10-BENCH-RECORD.md, never a way to guess values: the same door re-validates it.
static void benchPhSet(const char *argument)
{
  const char *cursor = argument;
  long mvA = 0;
  long mvB = 0;
  float bufA = NAN;
  float bufB = NAN;
  float tcalC = NAN;
  if (!benchParseLong(cursor, mvA) || !benchParseFloat(cursor, bufA) || !benchParseLong(cursor, mvB) ||
      !benchParseFloat(cursor, bufB) || !benchParseFloat(cursor, tcalC) || !benchAtEnd(cursor))
  {
    Serial.println("[bench] ph set refused: usage is ph set <mvA> <bufA> <mvB> <bufB> <tcalC>");
    return;
  }
  const long bufAHundredths = lroundf(bufA * 100.0f);
  const long bufBHundredths = lroundf(bufB * 100.0f);
  const long tcalCentiC = lroundf(tcalC * 100.0f);
  if (mvA < 0 || mvA > 65535 || mvB < 0 || mvB > 65535 || bufAHundredths < 0 || bufAHundredths > 65535 ||
      bufBHundredths < 0 || bufBHundredths > 65535 || tcalCentiC < -32768 || tcalCentiC > 32767)
  {
    Serial.println("[bench] ph set refused: a value is outside its stored field's range");
    return;
  }
  const ph::Calibration points = {
      static_cast<uint16_t>(mvA),
      static_cast<uint16_t>(bufAHundredths),
      static_cast<uint16_t>(mvB),
      static_cast<uint16_t>(bufBHundredths),
      static_cast<int16_t>(tcalCentiC),
      0,
      0,
  };
  benchPhReportStore(provisioning::storePhCalibration(points), points, "ph set");
}

static void benchPhClear()
{
  if (!provisioning::clearPhCalibration())
  {
    Serial.println("[bench] ph clear failed: NVS unavailable, calibration unchanged");
    return;
  }
  benchPhPointA.staged = false;
  benchPhPointB.staged = false;
  Serial.println("[bench] ph clear: calibration removed, the unit now reports pH uncalibrated");
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
  else if (strcmp(command, "ph show") == 0)
  {
    benchShowPh();
  }
  else if (strncmp(command, "ph capture ", 11) == 0)
  {
    benchPhCaptureStart(command + 11);
  }
  else if (strcmp(command, "ph save") == 0 || strncmp(command, "ph save ", 8) == 0)
  {
    benchPhSave(command + 7);
  }
  else if (strncmp(command, "ph set ", 7) == 0)
  {
    benchPhSet(command + 7);
  }
  else if (strcmp(command, "ph clear") == 0)
  {
    benchPhClear();
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

// The CSV keeps its own burst per row, even while a capture is running. It is the Phase 3 measurement
// instrument, and its rows must mean the same thing whether or not a capture is in flight; the capture
// sampler's bursts are separate and cached inside Provisioning. During a capture that is two ~64 ms bursts per
// second, well under the 1 s budget, and the ~20 CSV rows logged meanwhile give an independent cross-check of
// the capture's median (05-06).
//
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

  // SENS-10: the pH row for the same second, its own `[bench] phcsv,` prefix so fit-turbidity.mjs's strict
  // check on the csv line is untouched. Columns exactly as the phcsv header in setup() and fit-ph.mjs COLUMNS.
  // turb_filtered_pin_mv is the GPIO34 burst just above, so GPIO34/35 ghosting shows up row by row.
  if (benchPhRowCount % BENCH_TEMP_EVERY_ROWS == 0)
  {
    benchTempC = sensors::readTemperatureCelsius();
  }
  ++benchPhRowCount;
  const float phSensorMv = sensors::readPhMillivolts(benchTempC);
  const PhDiagnostics phDiagnostics = sensors::lastPhDiagnostics();
  Serial.printf(
      "[bench] phcsv,%lu,%.1f,%.1f,%.1f,%.1f,%.2f,%.2f,%.1f,%s\n",
      millis(),
      phDiagnostics.rawMeanMv,
      phDiagnostics.filteredPinMv,
      phDiagnostics.spreadMv,
      phDiagnostics.sensorMv,
      phDiagnostics.ph,
      benchTempC,
      diagnostics.filteredPinMv,
      benchRadioState());
  benchPhCaptureFeed(phSensorMv);
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

  if (benchCaptureWatching)
  {
    const provisioning::CaptureStatus status = provisioning::turbidityCaptureStatus();
    if (status.state != provisioning::CaptureState::Capturing)
    {
      benchCaptureWatching = false;
      if (status.state == provisioning::CaptureState::Accepted)
      {
        Serial.printf("[bench] cal capture accepted: stored median %u mV (spread %.1f mV over 20 samples)\n",
                      static_cast<unsigned>(status.medianMv), status.spreadMv);
      }
      else
      {
        Serial.printf("[bench] cal capture refused (%s): median %u mV, spread %.1f mV, nothing stored\n",
                      status.reason != nullptr ? status.reason : "unknown", static_cast<unsigned>(status.medianMv),
                      status.spreadMv);
      }
    }
  }
}
#endif

// NTU of the burst sensors::readTurbidityMillivolts() just took, handed to Provisioning's calibration sampler.
// Read back from the diagnostics rather than worked out here: main.cpp does no turbidity arithmetic of its own.
static float lastTurbidityNtu()
{
  return sensors::lastTurbidityDiagnostics().ntu;
}

// pH of the portal's last burst, read back from Sensors so this file does no pH arithmetic of its own.
static float lastPhValue()
{
  return sensors::lastPhDiagnostics().ph;
}

// The portal's temperature: the cached non-blocking poll, never a 750 ms DS18B20 conversion inside a portal tick.
static float pollTemperatureC()
{
  return sensors::pollTemperatureCelsius(millis());
}

void setup()
{
  Serial.begin(115200);
#if PH_DEMO_ANCHOR
  Serial.println("[ph-demo] synthetic anchor: 1819 sensor mV = pH 7.5; 10x lower sensitivity; not verified calibration");
#endif
  sensors::begin();
  provisioning::begin(provisioning::Config{
      SETUP_AP_PASSWORD,
      sensors::readTurbidityMillivolts,
      lastTurbidityNtu,
      sensors::setTurbidityCalibration,
      sensors::setPhCalibration,
      sensors::readPhMillivolts,
      lastPhValue,
      pollTemperatureC,
  });

  // After provisioning::begin(), never before it: Provisioning owns the "unit" NVS namespace and only reads
  // turb_clear_mv in there, so the value does not exist yet while sensors::begin() runs. Handing it across
  // here is also what keeps Sensors free of any NVS call of its own — it stays a hardware reader that is
  // told its reference rather than one that goes looking for it, and this file opens no NVS namespace
  // either. Moving this line above provisioning::begin() would
  // compile, run and log nothing, and every unit in the field would silently report no turbidity at all.
  sensors::setTurbidityCalibration(provisioning::turbidityClearWaterMv());

  // The pH calibration crosses the same way and for the same reason: after provisioning::begin(), never before
  // it, or Sensors keeps its zeroed calibration and every unit silently reports pH uncalibrated.
  const ph::Calibration phCal = provisioning::phCalibration();
  sensors::setPhCalibration(phCal.mvA, phCal.bufA, phCal.mvB, phCal.bufB, phCal.tcalCentiC, phCal.calAt,
                            phCal.version);

#if TURBIDITY_BENCH
  Serial.printf("[bench] %s build, commands: cal show | cal capture (20 s median) | cal set <mV>\n", BENCH_MARKER);
  Serial.println("[bench] csv,millis,raw_mean_mv,filtered_pin_mv,spread_mv,sensor_mv,ntu,clear_mv");
  Serial.printf("[bench] %s build, commands: ph show | ph capture a|b <nominal> | ph save [shortedMv] | ph set <mvA> "
                "<bufA> <mvB> <bufB> <tcalC> | ph clear\n",
                PH_BENCH_MARKER);
  Serial.println("[bench] phcsv,millis,ph_raw_mean_mv,ph_filtered_pin_mv,ph_spread_mv,ph_sensor_mv,ph,temp_c,turb_filtered_pin_mv,radio_state");
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

  // Above the provisioning gate on purpose: the turbidity report window must already be warm when the first
  // report comes, right after provisioning and NTP sync, or that report falls back to 3 back-to-back bursts
  // sharing one supply level. Pumping it while unprovisioned costs one ~64 ms burst every ~6 s.
  sensors::pollTurbidity(millis(), REPORT_INTERVAL_MS);
  // Same reason for pH (D-15): its window must be warm before the first report, not 3 back-to-back bursts.
  sensors::pollPh(millis(), REPORT_INTERVAL_MS);

  provisioning::loop(WiFi.status() == WL_CONNECTED);

  // Nothing else to do until an admin has provisioned WiFi + device identity through the setup hotspot.
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

  bool checkNow = false;
  if (!readOnce || millis() - lastReadingMs >= REPORT_INTERVAL_MS)
  {
    readOnce = true;
    lastReadingMs = millis();
    samplePending = true;
    checkNow = true;
    // ensureWiFi() lives only on this cadence, never in the 1 s retry below: with WiFi down it blocks for up to
    // WIFI_CONNECT_TIMEOUT_MS, so calling it every second would freeze the loop (MQTT keepalive, the setup portal)
    // almost permanently. NTP can't sync without WiFi anyway, so retrying WiFi every REPORT_INTERVAL_MS is enough.
    ensureWiFi();
  }

  // A sample without a trustworthy timestamp can't be placed on the pond's timeline, so it waits until NTP syncs.
  // It waits on a 1 s check rather than the report cadence because each miss used to cost a whole
  // REPORT_INTERVAL_MS, which kept the unit from showing online for a long time after boot (2026-10-02 demo).
  if (samplePending && (checkNow || millis() - lastNtpCheckMs >= NTP_RETRY_INTERVAL_MS))
  {
    lastNtpCheckMs = millis();
    if (uplink::timeSynced())
    {
      // Read the sensors now, not when the sample was armed, so the reading is fresh; the cadence restarts from
      // this first real enqueue.
      uplink::enqueue(sensors::readAll());
      samplePending = false;
      lastReadingMs = millis();
      lastNtpLogMs = 0;
    }
    else if (lastNtpLogMs == 0 || millis() - lastNtpLogMs >= NTP_WAIT_LOG_INTERVAL_MS)
    {
      Serial.println("[time] waiting for NTP sync, sample deferred");
      lastNtpLogMs = millis();
    }
  }

  uplink::loop();
  delay(10);
}
