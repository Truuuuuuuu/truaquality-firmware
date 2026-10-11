#pragma once

// Keep this header free of <Arduino.h> and of every Arduino type, String and pin constant — they belong in
// Sensors.cpp. [env:native] lists Sensors in lib_ignore and reaches this library only through
// `build_flags = ... -Ilib/Sensors`, so Sensors.cpp is never compiled on the host and stays free to include
// <Arduino.h>, <OneWire.h> and <DallasTemperature.h>. This header is the exception: lib/WireFormat/
// includes it for SensorSample, so the host build does parse it. A hardware include landing here is the one
// way Arduino code can reach [env:native].
// Symptom, if it happens: `pio test -e native` failing right after a sensor change with
// "Sensors.h:<line>:10: fatal error: 'Arduino.h' file not found". The same error naming Sensors.cpp instead
// means something broke the lib_ignore / -Ilib/Sensors pair in platformio.ini, not this header.
//
// Why each sensor is (or isn't) reporting, sent in the signed body's "sensors" object (firmware >= 0.6.0).
// The tokens statusToken() returns are the backend's SENSOR_STATUSES in backend/src/schemas/ingest.ts; a
// token it doesn't know rejects the whole message, readings included.
enum class TemperatureStatus : unsigned char
{
  Ok,
  NotFound,     // no DS18B20 answered on the bus
  Disconnected, // the probe was found but a read came back -127
  PowerOnValue, // exactly 85.0 C: the DS18B20's power-on-reset register value, never a measurement
};

enum class TurbidityStatus : unsigned char
{
  Ok,
  NoSignal,     // pin under the fault floor: signal wire or 5 V supply unplugged
  Uncalibrated, // no plausible clear-water reference stored
  OverRange,    // far above the reference: supply drifted up or the calibration is stale
};

// pH joined the wire contract in Phase 9 and got its driver in Phase 10 (firmware 0.7.0). The enumerators reuse
// tokens already in the backend's SENSOR_STATUSES, because a new token would reject the whole message on any
// backend that predates it. The ph value is sent only with Ok, whatever sample.ph holds, so a finite number left
// beside any other status is never signed (09-REVIEW WR-03). Phase 10 mapping (D-01), decided in ph::classify:
//   NoSignal     — the burst is NAN, under the fault floor, or over the instability backstop: a dead amplifier,
//                  an unplugged signal wire, or a loose/floating input the 1 MOhm pull-down drags down.
//   Uncalibrated — no valid stored two-point buffer calibration (never written, wrong version, wrong sign).
//   OverRange    — calibrated pH outside the 2..12 plausibility window (D-14), never clamped to an edge.
// NotFitted stays in the enum for wire compatibility (wire::buildBody omits both ph keys for it), but no 0.7.0
// path produces it: a 0.7.0 unit always reports a real pH status.
enum class PhStatus : unsigned char
{
  NotFitted,    // no pH front end in this build: value and sensors.ph are both omitted (not produced by 0.7.0)
  Ok,
  NoSignal,     // probe or amplifier output missing, or the input floating/unstable
  Uncalibrated, // no valid stored buffer calibration
  OverRange,    // calibrated pH outside the 2..12 plausibility window
};

// One reading of every sensor. A value is NAN when its sensor failed or isn't wired up; NAN values are
// left out of the upload rather than sent as zeros. A status other than Ok always pairs with a NAN value,
// because the backend omits a parameter's value whenever its status isn't "ok" and the wire must agree.
// Every SensorSample literal must spell out all six fields: an omitted status value-initializes to Ok,
// which would report a broken sensor as healthy, just as an omitted value would become a plausible 0.0f.
// ph sits directly after turbidity (not at the end) on purpose: an old four-field literal then puts a
// TemperatureStatus where a float belongs and fails to compile, instead of silently sending ph = 0.0 —
// a plausible-looking and badly wrong reading.
struct SensorSample
{
  float temperature; // °C
  float turbidity;   // NTU
  float ph;          // pH (unitless)
  TemperatureStatus temperatureStatus;
  TurbidityStatus turbidityStatus;
  PhStatus phStatus;
};

// The intermediate numbers behind one turbidity acquisition, for the bench CSV and the serial calibration
// readout. Plain floats and nothing else: this is a mirror of turbidity::Burst plus two derived values, and
// it is spelled out by hand rather than embedding that struct precisely because including TurbidityMath.h
// here would drag a second library into every host build that only wanted SensorSample.
struct TurbidityDiagnostics
{
  float rawMeanMv;     // burst mean before trimming — shows what the trim threw away
  float filteredPinMv; // trimmed mean at the pin, the value the NTU conversion is fed
  float spreadMv;      // highest minus lowest of the kept window, i.e. how steady the burst was
  float sensorMv;      // filteredPinMv scaled back up through the divider: what the sensor itself put out
  float ntu;           // what that same burst converted to, NAN when it faulted or the unit is uncalibrated
};

// The intermediates behind one pH acquisition, for the bench console and CSV. Plain floats for the same reason
// as TurbidityDiagnostics: including PhMath.h here would drag it into every host build that wanted SensorSample.
struct PhDiagnostics
{
  float rawMeanMv;     // burst mean before trimming — shows what the trim threw away
  float filteredPinMv; // trimmed mean at the pin, the value the pH conversion is fed
  float spreadMv;      // highest minus lowest of the kept window; the instability backstop reads this
  float sensorMv;      // filteredPinMv scaled back up through the divider: what the pH board itself put out
  float ph;            // what that same burst converted to, NAN unless ph::classify says Ok
  float tempUsedC;     // temperature the compensation used: the DS18B20 reading, or Tcal when it was NAN
};

namespace sensors
{
  // One explicit case per enumerator and no default, so -Wswitch flags a new status that has no token yet.
  // The trailing return is unreachable for a valid enumerator; it returns a fault token rather than "ok",
  // because a corrupted value must never report a broken sensor as healthy.
  inline const char *statusToken(TemperatureStatus status)
  {
    switch (status)
    {
    case TemperatureStatus::Ok:
      return "ok";
    case TemperatureStatus::NotFound:
      return "not_found";
    case TemperatureStatus::Disconnected:
      return "disconnected";
    case TemperatureStatus::PowerOnValue:
      return "power_on_value";
    }
    return "not_found";
  }

  inline const char *statusToken(TurbidityStatus status)
  {
    switch (status)
    {
    case TurbidityStatus::Ok:
      return "ok";
    case TurbidityStatus::NoSignal:
      return "no_signal";
    case TurbidityStatus::Uncalibrated:
      return "uncalibrated";
    case TurbidityStatus::OverRange:
      return "over_range";
    }
    return "no_signal";
  }

  // NotFitted maps to a fault token, never "ok", so a slip that sent it could not report a missing sensor as
  // healthy; wire::buildBody never calls this for NotFitted, it omits the key instead.
  inline const char *statusToken(PhStatus status)
  {
    switch (status)
    {
    case PhStatus::NotFitted:
      return "no_signal";
    case PhStatus::Ok:
      return "ok";
    case PhStatus::NoSignal:
      return "no_signal";
    case PhStatus::Uncalibrated:
      return "uncalibrated";
    case PhStatus::OverRange:
      return "over_range";
    }
    return "no_signal";
  }

  void begin();
  SensorSample readAll();

  // Hands the unit's stored clear-water reference in; 0 means uncalibrated, which makes every turbidity read
  // NAN. Spelled `unsigned short` rather than uint16_t only because of this header's zero-include rule above
  // — it is the same 16-bit type on both toolchains, and callers pass provisioning::turbidityClearWaterMv()
  // straight in. Provisioning owns storage and validation; this just receives the result.
  void setTurbidityCalibration(unsigned short clearWaterMv);

  // One burst returned as SENSOR-side millivolts — the raw number an admin reads on the bench and the
  // number a clear-water capture stores. NAN on a faulted pin, and deliberately NOT gated on calibration,
  // since this is what produces a calibration in the first place.
  float readTurbidityMillivolts();

  // Call on every loop() pass with millis() and REPORT_INTERVAL_MS. Takes at most one burst (~64 ms) per
  // reportIntervalMs / 5 into the report window whose median readAll() reports (firmware 0.6.1). Required
  // for readAll()'s turbidity to be windowed at all: without it every report is a 3-burst back-to-back
  // warm-up that shares one supply level, which is the single-burst spike problem again.
  void pollTurbidity(unsigned long nowMs, unsigned long reportIntervalMs);

  // The intermediates of the most recent diagnostics write. After readAll(): the reported window burst, so
  // its ntu is the value on the wire. After readTurbidityMillivolts(): that single burst. Window bursts taken
  // by pollTurbidity() never write it. All NAN before the first read.
  TurbidityDiagnostics lastTurbidityDiagnostics();

  // Hands the unit's stored pH calibration in (the D-17 NVS fields one for one). Primitives rather than
  // ph::Calibration because of this header's zero-include rule; `unsigned short`/`short`/`unsigned long`/
  // `unsigned char` are the same widths as the PhMath fields on both toolchains, and the signature matches
  // provisioning::Config::applyPhCalibration so main.cpp passes this function straight in. A zeroed call (what
  // clearPhCalibration hands over) makes every pH read Uncalibrated. Validation is PhMath's, on every use.
  void setPhCalibration(unsigned short mvA, unsigned short bufA, unsigned short mvB, unsigned short bufB,
                        short tcalCentiC, unsigned long calAt, unsigned char version);

  // One burst returned as SENSOR-side millivolts — the number a buffer capture stores. NAN when the burst fails
  // the shared signal test (floor or instability backstop), deliberately NOT gated on calibration, since this is
  // what produces a calibration. tempC (or NAN) only feeds the diagnostics' ph/tempUsedC columns.
  float readPhMillivolts(float tempC);

  // Call on every loop() pass beside pollTurbidity. Takes at most one ~64 ms GPIO35 burst per
  // reportIntervalMs / ph::REPORT_BURSTS into the pH report window whose median readAll() reports (D-15).
  void pollPh(unsigned long nowMs, unsigned long reportIntervalMs);

  // The intermediates of the most recent pH diagnostics write: after readAll(), the reported window burst (its
  // ph is the value on the wire); after readPhMillivolts(), that one burst. All NAN before the first read.
  PhDiagnostics lastPhDiagnostics();

  // One blocking DS18B20 read (~750 ms), NAN on any fault. For the bench console's buffer temperature (D-11),
  // which needs the probe reading without running a whole readAll() and disturbing the report windows.
  float readTemperatureCelsius();
}
