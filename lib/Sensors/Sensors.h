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

// pH joins the wire contract before its driver exists (Phase 9 fixes the signed bytes, Phase 10 builds the
// reader). NotFitted means this build has no pH front end at all: wire::buildBody then omits both the ph value
// and the sensors.ph key, so the body is exactly the 0.6.x shape and no unit ever sends a fabricated pH. The
// ph value is sent only with Ok, whatever sample.ph holds, so a finite number left beside any other status is
// never signed (09-REVIEW WR-03). The other enumerators reuse tokens already in the backend's SENSOR_STATUSES, because a new token would reject
// the whole message on any backend that predates it; which real fault maps to which is Phase 10's call.
enum class PhStatus : unsigned char
{
  NotFitted,    // no pH front end in this build: value and sensors.ph are both omitted
  Ok,
  NoSignal,     // probe or amplifier output missing
  Uncalibrated, // no stored buffer calibration
  OverRange,    // electrode output outside what the calibration can convert
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
}
