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
// One reading of every sensor. A value is NAN when its sensor failed or isn't wired up; NAN values are
// left out of the upload rather than sent as zeros.
struct SensorSample
{
  float temperature; // °C
  float turbidity;   // NTU
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
  void begin();
  SensorSample readAll();

  // Hands the unit's stored clear-water reference in; 0 means uncalibrated, which makes every turbidity read
  // NAN. Spelled `unsigned short` rather than uint16_t only because of this header's zero-include rule above
  // — it is the same 16-bit type on both toolchains, and callers pass provisioning::turbidityClearWaterMv()
  // straight in. Provisioning owns storage and validation; this just receives the result.
  void setTurbidityCalibration(unsigned short clearWaterMv);

  // One burst returned as SENSOR-side millivolts — the raw number a technician reads on the bench and the
  // number a clear-water capture stores. NAN on a faulted pin, and deliberately NOT gated on calibration,
  // since this is what produces a calibration in the first place.
  float readTurbidityMillivolts();

  // The intermediates from whichever of readAll() / readTurbidityMillivolts() ran most recently. All NAN
  // before the first read.
  TurbidityDiagnostics lastTurbidityDiagnostics();
}
