#include "Sensors.h"

#include "TurbidityMath.h"

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

  // GPIO34 is ADC1_CH6, and ADC1 is not a preference: WiFi owns ADC2 outright, so an ADC2 pin stops
  // returning anything usable the moment the radio is up. GPIO34 is also input-only, which means it has no
  // internal pull-up or pull-down that could pull the divider off its ratio, and it is not a strapping pin.
  // The signal reaches it through a 10 kΩ top / 12 kΩ bottom divider with a 100 nF ceramic from this pin to
  // GND: the divider puts the sensor's 4.5 V full scale at 2.45 V, and the cap holds the pin steady for the
  // ADC's sample capacitor against the divider's 5.5 kΩ source impedance.
  constexpr uint8_t TURBIDITY_PIN = 34;

  OneWire oneWire(ONE_WIRE_PIN);
  DallasTemperature ds18b20(&oneWire);

  // The unit's clear-water reference, handed in by main.cpp once provisioning::begin() has loaded it from
  // NVS. 0 until then, and 0 for the whole run on a unit nobody calibrated — turbidity::ntuFromPinMv turns
  // that into NAN, which is the D-04 behaviour: temperature keeps reporting, turbidity is simply absent.
  uint16_t clearWaterMv = 0;

  // All NAN before the first burst, so a diagnostics readout taken at boot cannot print a zero that looks
  // like a measurement.
  TurbidityDiagnostics lastDiagnostics{NAN, NAN, NAN, NAN, NAN};

  // Exactly representable in float, so == is correct. The DS18B20 holds this in its scratchpad after a
  // power-on reset until a conversion completes; firmware before 0.6.0 published it as a real 85 C reading.
  constexpr float POWER_ON_RESET_C = 85.0f;

  struct TemperatureRead
  {
    float celsius;
    TemperatureStatus status;
  };

  TemperatureRead readTemperature()
  {
    // Re-scan the bus on every read. getDeviceCount() is only the count cached by the last begin(), so
    // without this an unplugged-then-replugged probe would stay "not_found" until a reboot and the backend's
    // SENSOR_RECOVERED event would never fire. A search on an idle one-probe bus costs a few milliseconds.
    ds18b20.begin();
    if (ds18b20.getDeviceCount() == 0)
    {
      return TemperatureRead{NAN, TemperatureStatus::NotFound};
    }
    ds18b20.requestTemperatures();
    const float celsius = ds18b20.getTempCByIndex(0);
    if (celsius == DISCONNECTED_C || celsius == DEVICE_DISCONNECTED_C)
    {
      return TemperatureRead{NAN, TemperatureStatus::Disconnected};
    }
    if (celsius == POWER_ON_RESET_C)
    {
      return TemperatureRead{NAN, TemperatureStatus::PowerOnValue};
    }
    return TemperatureRead{celsius, TemperatureStatus::Ok};
  }

  // An explicit switch, not a cast: the two enums are declared separately (TurbidityMath.h must not include
  // Sensors.h) and a cast would silently pair the wrong statuses if either list were ever reordered.
  TurbidityStatus toTurbidityStatus(turbidity::Status status)
  {
    switch (status)
    {
    case turbidity::Status::Ok:
      return TurbidityStatus::Ok;
    case turbidity::Status::NoSignal:
      return TurbidityStatus::NoSignal;
    case turbidity::Status::Uncalibrated:
      return TurbidityStatus::Uncalibrated;
    case turbidity::Status::OverRange:
      return TurbidityStatus::OverRange;
    }
    return TurbidityStatus::NoSignal;
  }

  // Seeded with Ok so a unit that boots healthy logs nothing, and one that boots faulted logs the fault once.
  TemperatureStatus lastTemperatureStatus = TemperatureStatus::Ok;
  TurbidityStatus lastTurbidityStatus = TurbidityStatus::Ok;

  // One acquisition. Neither the 1 ms spacing nor the trim is arbitrary: WiFi TX bursts put one-sided spikes
  // on ESP32 ADC reads, so spreading the samples over ~64 ms and dropping the extremes rejects a spike the
  // way a median would while still averaging quantization noise away the way a mean does. The sort and the
  // averaging stay in turbidity::summarizeBurst, where native tests pin them — hand-rolling either here
  // would put untested arithmetic between the pin and the wire.
  turbidity::Burst readBurst()
  {
    uint16_t samplesMv[turbidity::BURST_SAMPLES];
    for (size_t i = 0; i < turbidity::BURST_SAMPLES; i++)
    {
      samplesMv[i] = analogReadMilliVolts(TURBIDITY_PIN);
      delay(1);
    }
    return turbidity::summarizeBurst(samplesMv, turbidity::BURST_SAMPLES, turbidity::TRIM_EACH_END);
  }

  // The one and only place TurbidityDiagnostics is written, which is why both public read paths go through
  // it. The bench's per-burst CSV line reads `ntu` after calling readTurbidityMillivolts(), and that column
  // is where the fault-floor evidence is read from — if the two paths each filled the struct their own way,
  // the column could go stale or empty without anything failing.
  turbidity::Burst captureBurst()
  {
    const turbidity::Burst burst = readBurst();
    lastDiagnostics.rawMeanMv = burst.rawMeanMv;
    lastDiagnostics.filteredPinMv = burst.filteredMv;
    lastDiagnostics.spreadMv = burst.spreadMv;
    lastDiagnostics.sensorMv = turbidity::sensorMvFromPinMv(burst.filteredMv);
    lastDiagnostics.ntu = turbidity::ntuFromPinMv(burst.filteredMv, clearWaterMv);
    return burst;
  }

  struct TurbidityRead
  {
    float ntu;
    TurbidityStatus status;
  };

  TurbidityRead readTurbidity()
  {
    const turbidity::Burst burst = captureBurst();
    // No guard of its own, no local clamp. Every fault decision — below the fault floor, uncalibrated,
    // implausible calibration, far above the reference — lives in turbidity::classify, which ntuFromPinMv
    // also runs, pinned by native tests. Classifying the same burst captureBurst() just converted keeps the
    // status and the value from ever disagreeing, and returning the field captureBurst() just computed,
    // rather than recomputing, guarantees the value on the wire is the number an admin sees in the
    // diagnostics for that same burst.
    const TurbidityStatus status = toTurbidityStatus(turbidity::classify(burst.filteredMv, clearWaterMv));
    return TurbidityRead{lastDiagnostics.ntu, status};
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

    // The divided signal spans roughly 0–2.45 V at the pin, which is exactly the 11 dB range (about
    // 150–2450 mV); analogReadMilliVolts then applies this particular chip's factory eFuse calibration, so
    // the millivolts it returns already account for a Vref that varies from 1000 to 1200 mV between chips.
    // Nothing else is needed here — in particular, no call that routes the internal reference out to a GPIO:
    // that targets an ADC2 pin and is pointless on a chip that already carries eFuse calibration.
    // No boot log either: the one line about an unusable calibration is Provisioning's, and saying it twice
    // would make an admin hunt for two different faults.
    analogSetPinAttenuation(TURBIDITY_PIN, ADC_11db);
  }

  void setTurbidityCalibration(unsigned short newClearWaterMv)
  {
    // A plain assignment on purpose. Validation belongs to Provisioning, which owns the NVS key: this setter
    // only ever receives a value that was already stored or already refused there, and a second window check
    // here would be a second place to update when the bench narrows the window.
    clearWaterMv = newClearWaterMv;
  }

  float readTurbidityMillivolts()
  {
    const turbidity::Burst burst = captureBurst();
    // Deliberately not gated on calibration: this is the number a clear-water capture stores, so refusing it
    // on an uncalibrated unit would make a unit impossible to calibrate. The fault floor still applies,
    // because a dead signal or a dead 5 V supply must not hand back a confident millivolt figure either.
    if (std::isnan(burst.filteredMv) || burst.filteredMv < turbidity::FAULT_FLOOR_PIN_MV)
    {
      return NAN;
    }
    return lastDiagnostics.sensorMv;
  }

  TurbidityDiagnostics lastTurbidityDiagnostics()
  {
    return lastDiagnostics;
  }

  SensorSample readAll()
  {
    const TemperatureRead temperature = readTemperature();
    const TurbidityRead turbidityRead = readTurbidity();

    // The invariant the backend and the wire share: a status other than ok never travels with a value.
    const float celsius = temperature.status == TemperatureStatus::Ok ? temperature.celsius : NAN;
    const float ntu = turbidityRead.status == TurbidityStatus::Ok ? turbidityRead.ntu : NAN;

    // Only on a change, never every read: a probe that stays unplugged would otherwise print the same line
    // every REPORT_INTERVAL_MS forever.
    if (temperature.status != lastTemperatureStatus)
    {
      Serial.printf("[sensors] temperature: %s\n", sensors::statusToken(temperature.status));
      lastTemperatureStatus = temperature.status;
    }
    if (turbidityRead.status != lastTurbidityStatus)
    {
      Serial.printf("[sensors] turbidity: %s\n", sensors::statusToken(turbidityRead.status));
      lastTurbidityStatus = turbidityRead.status;
    }

    // Positional with all four fields spelled out — a positional initializer that names only temperature
    // would value-initialize turbidity to 0.0f, and 0.0f is the one value that must never reach the wire: it
    // is a perfectly plausible "crystal clear water" reading, so it would be stored and charted as a real
    // measurement instead of being omitted. An omitted status would likewise become Ok. wire::buildBody
    // drops the key entirely for NAN.
    return SensorSample{celsius, ntu, temperature.status, turbidityRead.status};
  }
}
