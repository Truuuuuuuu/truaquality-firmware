#pragma once

// One reading of every sensor. A value is NAN when its sensor failed or isn't wired up; NAN values are
// left out of the upload rather than sent as zeros.
struct SensorSample
{
  float temperature; // °C
};

namespace sensors
{
  void begin();
  SensorSample readAll();
}
