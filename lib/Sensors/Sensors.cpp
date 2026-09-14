#include "Sensors.h"

#include <Arduino.h>
#include <DallasTemperature.h>
#include <OneWire.h>

// DS18B20 waterproof probe on GPIO 4, wired OneWire (data pin needs a ~4.7kΩ pull-up to 3V3 if the probe
// module doesn't already have one on board).
namespace
{
  constexpr uint8_t ONE_WIRE_PIN = 4;
  // 750ms conversion time at 12-bit resolution (the DallasTemperature default).
  constexpr float DISCONNECTED_C = -127.0f;

  OneWire oneWire(ONE_WIRE_PIN);
  DallasTemperature ds18b20(&oneWire);

  float readTemperature()
  {
    ds18b20.requestTemperatures();
    float celsius = ds18b20.getTempCByIndex(0);
    if (celsius == DISCONNECTED_C || celsius == DEVICE_DISCONNECTED_C)
    {
      return NAN;
    }
    return celsius;
  }

  float readDissolvedOxygen()
  {
    // TODO: dissolved oxygen probe driver (apply temperature compensation if the module needs it).
    return NAN;
  }

  float readSalinity()
  {
    // TODO: conductivity/salinity probe driver.
    return NAN;
  }
}

namespace sensors
{
  void begin()
  {
    ds18b20.begin();
    if (ds18b20.getDeviceCount() == 0)
    {
      Serial.println("[sensors] no DS18B20 found on the OneWire bus (check wiring/pull-up)");
    }
  }

  SensorSample readAll()
  {
    return SensorSample{readTemperature(), readDissolvedOxygen(), readSalinity()};
  }
}
