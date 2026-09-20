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

namespace sensors
{
  void begin();
  SensorSample readAll();
}
