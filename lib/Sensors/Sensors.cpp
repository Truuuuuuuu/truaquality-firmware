#include "Sensors.h"

#include "PhMath.h"
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

  // GPIO35 is ADC1_CH7, for the same reason as GPIO34: WiFi owns ADC2, so only an ADC1 pin keeps reading once
  // the radio is up. It is input-only (no internal pull that could skew the divider) and not a strapping pin, so
  // the pH board's output cannot change how the chip boots. The divider and the external 1 MOhm pull-down fitted
  // in the pin's own row on the bench (D-12) are modelled in ph::DIVIDER_RATIO; the pull-down drags a floating
  // input to the fault floor instead of letting it drift to a believable pH.
  constexpr uint8_t PH_PIN = 35;

  OneWire oneWire(ONE_WIRE_PIN);
  DallasTemperature ds18b20(&oneWire);

  // The unit's clear-water reference, handed in by main.cpp once provisioning::begin() has loaded it from
  // NVS. 0 until then, and 0 for the whole run on a unit nobody calibrated — turbidity::ntuFromPinMv turns
  // that into NAN, which is the D-04 behaviour: temperature keeps reporting, turbidity is simply absent.
  uint16_t clearWaterMv = 0;

  // All NAN before the first burst, so a diagnostics readout taken at boot cannot print a zero that looks
  // like a measurement.
  TurbidityDiagnostics lastDiagnostics{NAN, NAN, NAN, NAN, NAN};

  // The unit's stored pH calibration, handed in by main.cpp after provisioning::begin(). Zeroed until then (and
  // for the whole run on an uncalibrated unit): version 0 fails ph::isValidCalibration, so pH reads
  // Uncalibrated while temperature and turbidity keep reporting (SENS-09).
  ph::Calibration phCal{};

  // All NAN before the first burst, for the same reason as lastDiagnostics.
  PhDiagnostics lastPhDiag{NAN, NAN, NAN, NAN, NAN, NAN};

  // The pH report window (D-15), separate from turbidity's so the two sensors' bursts never mix.
  ph::BurstWindow phWindow{};
  unsigned long lastPhBurstMs = 0;
  bool phBurstTaken = false;

  // Exactly representable in float, so == is correct. The DS18B20 holds this in its scratchpad after a
  // power-on reset until a conversion completes; firmware before 0.6.0 published it as a real 85 C reading.
  constexpr float POWER_ON_RESET_C = 85.0f;

  // The portal's non-blocking poll (pollTemperatureCelsius). A cached value older than a few 1 Hz sampler ticks
  // means the probe stopped answering, so it is reported as no temperature rather than shown as live.
  constexpr unsigned long TEMPERATURE_CACHE_MAX_AGE_MS = 5000;
  bool tempConversionPending = false;
  unsigned long tempConversionStartMs = 0;
  float cachedTempC = NAN;
  unsigned long cachedTempMs = 0;
  bool cachedTempValid = false;

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

  // Same reasoning as toTurbidityStatus: ph::Status lives in PhMath.h, which must not include Sensors.h, so the
  // pairing is spelled out. NotFitted is deliberately unreachable from here (D-01: 0.7.0 never produces it).
  PhStatus toPhStatus(ph::Status status)
  {
    switch (status)
    {
    case ph::Status::Ok:
      return PhStatus::Ok;
    case ph::Status::NoSignal:
      return PhStatus::NoSignal;
    case ph::Status::Uncalibrated:
      return PhStatus::Uncalibrated;
    case ph::Status::OverRange:
      return PhStatus::OverRange;
    }
    return PhStatus::NoSignal;
  }

  // Seeded with Ok so a unit that boots healthy logs nothing, and one that boots faulted logs the fault once.
  TemperatureStatus lastTemperatureStatus = TemperatureStatus::Ok;
  TurbidityStatus lastTurbidityStatus = TurbidityStatus::Ok;
  // Seeded with NotFitted, which no 0.7.0 path produces, so the first real pH status always logs once — on the
  // bench that one line says whether the unit came up uncalibrated, faulted or reading.
  PhStatus lastPhStatus = PhStatus::NotFitted;

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

  // The one and only place TurbidityDiagnostics is written. Both public read paths go through it: readAll()
  // feeds it the window burst it is about to report, readTurbidityMillivolts() (via captureBurst()) the one
  // burst it just took. The bench's per-burst CSV line reads `ntu` after calling readTurbidityMillivolts(),
  // and that column is where the fault-floor evidence is read from — if the paths each filled the struct
  // their own way, the column could go stale or empty without anything failing. Window bursts taken by
  // pollTurbidity() deliberately do NOT come through here, so they never overwrite what a single-burst
  // caller (the calibration sampler, the bench CSV) is about to read back.
  // Returns the burst it recorded, so a caller can only ever go on to use the burst it just described.
  turbidity::Burst recordDiagnostics(const turbidity::Burst &burst)
  {
    lastDiagnostics.rawMeanMv = burst.rawMeanMv;
    lastDiagnostics.filteredPinMv = burst.filteredMv;
    lastDiagnostics.spreadMv = burst.spreadMv;
    lastDiagnostics.sensorMv = turbidity::sensorMvFromPinMv(burst.filteredMv);
    lastDiagnostics.ntu = turbidity::ntuFromBurst(burst, clearWaterMv);
    return burst;
  }

  turbidity::Burst captureBurst()
  {
    return recordDiagnostics(readBurst());
  }

  // The report window (firmware 0.6.1): the last turbidity::REPORT_BURSTS bursts, one taken every
  // REPORT_INTERVAL_MS / REPORT_BURSTS by sensors::pollTurbidity(). Never cleared after a report — it is a
  // rolling window, and at that spacing the last 5 bursts are the ones since the previous report.
  turbidity::BurstWindow reportWindow{};
  unsigned long lastWindowBurstMs = 0;
  bool windowBurstTaken = false;

  struct TurbidityRead
  {
    float ntu;
    TurbidityStatus status;
  };

  TurbidityRead readTurbidity()
  {
    // Warm-up: the first report after boot (or after a long NTP wait) may find fewer than MIN_REPORT_BURSTS
    // bursts in the window. Topping it up immediately guarantees the median always comes from an odd count of
    // at least 3 real bursts. The cost: these bursts are back to back, so they share one supply level and
    // only that first report is weaker against the two-level signal. Bounded by entry count, so a dead pin
    // cannot make this loop.
    for (size_t i = turbidity::topUpNeeded(reportWindow); i > 0; i--)
    {
      turbidity::pushBurst(reportWindow, readBurst());
    }

    // The window's median burst, whole. Then the one diagnostics write, fed that same burst.
    const turbidity::Burst reported = recordDiagnostics(turbidity::reportBurst(reportWindow));
    // No guard of its own, no local clamp. Every fault decision — below the fault floor, too unstable within
    // the burst (the D-12 spread backstop for a floating input), uncalibrated, implausible calibration, far
    // above the reference — lives in the burst-level turbidity::classify, which ntuFromBurst also runs, pinned
    // by native tests. It must be the burst-level one, given the WHOLE burst: when most of the window is
    // unstable, reportBurst() returns an unstable burst whose filteredMv can look like valid clear water, and
    // the scalar classify on filteredMv alone would call it Ok (Pitfall 5). Classifying the reported window
    // burst that recordDiagnostics() just converted keeps the status and the value from ever disagreeing, and
    // returning the field it just computed, rather than recomputing, guarantees the value on the wire is the
    // number an admin sees in the diagnostics for that same burst.
    const TurbidityStatus status = toTurbidityStatus(turbidity::classify(reported, clearWaterMv));
    return TurbidityRead{lastDiagnostics.ntu, status};
  }

  // One pH acquisition, the same 64-read trimmed burst as turbidity (WiFi TX spikes are the same problem on
  // every ADC1 pin). The arithmetic stays in ph::summarizeBurst, pinned by test_ph_math.
  ph::Burst readPhBurst()
  {
    uint16_t samplesMv[ph::BURST_SAMPLES];
    for (size_t i = 0; i < ph::BURST_SAMPLES; i++)
    {
      samplesMv[i] = analogReadMilliVolts(PH_PIN);
      delay(1);
    }
    return ph::summarizeBurst(samplesMv, ph::BURST_SAMPLES, ph::TRIM_EACH_END);
  }

  // The single writer of PhDiagnostics, for the reason recordDiagnostics is turbidity's: readAll() and
  // readPhMillivolts() must describe a burst the same way, or a bench column could go stale silently. Window
  // bursts taken by pollPh() never come through here. tempUsedC records what the compensation actually used —
  // the DS18B20 reading, or Tcal when it was NAN (D-03) — and stays NAN when there is no valid calibration to
  // take a Tcal from.
  ph::Burst recordPhDiagnostics(const ph::Burst &burst, float tempC)
  {
    lastPhDiag.rawMeanMv = burst.rawMeanMv;
    lastPhDiag.filteredPinMv = burst.filteredMv;
    lastPhDiag.spreadMv = burst.spreadMv;
    lastPhDiag.sensorMv = ph::sensorMvFromPinMv(burst.filteredMv);
    lastPhDiag.ph = ph::phFromBurst(burst, phCal, tempC);
    if (std::isfinite(tempC))
    {
      lastPhDiag.tempUsedC = tempC;
    }
    else if (ph::isValidCalibration(phCal))
    {
      lastPhDiag.tempUsedC = ph::fit(phCal).tcalC;
    }
    else
    {
      lastPhDiag.tempUsedC = NAN;
    }
    return burst;
  }

  struct PhRead
  {
    float ph;
    PhStatus status;
  };

  // Mirrors readTurbidity: top the window up to MIN_REPORT_BURSTS on the first report (bounded by entry count,
  // so a dead pin cannot loop), take the window's median burst whole, record it, and classify that same burst so
  // the status and the value can never disagree. Every fault decision is ph::classify's.
  PhRead readPh(float tempC)
  {
    for (size_t i = ph::topUpNeeded(phWindow); i > 0; i--)
    {
      ph::pushBurst(phWindow, readPhBurst());
    }
    const ph::Burst reported = recordPhDiagnostics(ph::reportBurst(phWindow), tempC);
    const PhStatus status = toPhStatus(ph::classify(reported, phCal, tempC));
    return PhRead{lastPhDiag.ph, status};
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
    // Per pin, never the global attenuation call: each ADC1 input is set for its own signal range, and the pH
    // board's divided output sits inside the same 11 dB (about 150-2450 mV) eFuse-calibrated range.
    analogSetPinAttenuation(PH_PIN, ADC_11db);
  }

  void setTurbidityCalibration(unsigned short newClearWaterMv)
  {
    // A plain assignment on purpose. Validation belongs to Provisioning, which owns the NVS key: this setter
    // only ever receives a value that was already stored or already refused there, and a second window check
    // here would be a second place to update when the bench narrows the window.
    clearWaterMv = newClearWaterMv;
  }

  // 2026-10-03, DEVICE001 on firmware 0.6.0 in clean water: 6 of 8 readings were 0.0 NTU and two were
  // isolated spikes of 260.1 and 408.1 NTU, each opening a backend WARNING, because one reading was one
  // ~64 ms burst and the clear-water signal sits on one of two levels ~120 mV apart that flip every 1-17 s.
  // Spreading the report's bursts across the whole interval means a low dwell must last longer than about
  // 12 s to reach the wire. Cooperative by design: each call costs nothing or one ~64 ms burst, so loop()
  // keeps servicing MQTT, the setup portal and the bench console. The interval is passed in because
  // REPORT_INTERVAL_MS lives in unit_config.h, which this library must not include. This is a software
  // mitigation only — the hardware cause (likely 5 V USB supply dips during WiFi TX) is still open.
  void pollTurbidity(unsigned long nowMs, unsigned long reportIntervalMs)
  {
    const unsigned long spacingMs = reportIntervalMs / turbidity::REPORT_BURSTS;
    // Unsigned subtraction, so millis() wrapping after ~49.7 days does not stall the window.
    if (windowBurstTaken && (nowMs - lastWindowBurstMs) < spacingMs)
    {
      return;
    }
    turbidity::pushBurst(reportWindow, readBurst());
    lastWindowBurstMs = nowMs;
    windowBurstTaken = true;
  }

  // Deliberately ONE burst per call, not the report window: the calibration sampler already takes the
  // median of 20 one-second samples of this, and the bench CSV exists to characterize the raw two-level
  // signal, so a median here would hide exactly what it measures.
  float readTurbidityMillivolts()
  {
    const turbidity::Burst burst = captureBurst();
    // Deliberately not gated on calibration: this is the number a clear-water capture stores, so refusing it
    // on an uncalibrated unit would make a unit impossible to calibrate. The shared burst signal test still
    // applies — the fault floor, because a dead signal or a dead 5 V supply must not hand back a confident
    // millivolt figure, and the D-12 spread backstop, so a floating or loose input can never be captured and
    // stored as the clear-water reference.
    if (!turbidity::isSignalBurst(burst))
    {
      return NAN;
    }
    return lastDiagnostics.sensorMv;
  }

  TurbidityDiagnostics lastTurbidityDiagnostics()
  {
    return lastDiagnostics;
  }

  void setPhCalibration(unsigned short mvA, unsigned short bufA, unsigned short mvB, unsigned short bufB,
                        short tcalCentiC, unsigned long calAt, unsigned char version)
  {
    // Plain assignment, like setTurbidityCalibration: Provisioning owns storage and refusal, and PhMath
    // re-validates on every use, so a third check here would only be another place to keep in step.
    phCal = ph::Calibration{mvA, bufA, mvB, bufB, tcalCentiC, static_cast<uint32_t>(calAt), version};
  }

  // Copy of pollTurbidity's scheduling with ph::REPORT_BURSTS (D-15): a report's pH is the median of bursts
  // spread across the whole interval, never one ~64 ms burst. Cooperative — nothing or one burst per call.
  void pollPh(unsigned long nowMs, unsigned long reportIntervalMs)
  {
    const unsigned long spacingMs = reportIntervalMs / ph::REPORT_BURSTS;
    // Unsigned subtraction, so millis() wrapping after ~49.7 days does not stall the window.
    if (phBurstTaken && (nowMs - lastPhBurstMs) < spacingMs)
    {
      return;
    }
    ph::pushBurst(phWindow, readPhBurst());
    lastPhBurstMs = nowMs;
    phBurstTaken = true;
  }

  // ONE burst, not the window: the bench console's buffer capture takes its own median of samples, and the
  // bench CSV characterizes the raw signal, which a median here would hide.
  float readPhMillivolts(float tempC)
  {
    const ph::Burst burst = recordPhDiagnostics(readPhBurst(), tempC);
    // Not gated on calibration (this is what makes one), but still on the shared signal test: a dead or
    // floating input must never be captured and stored as a buffer point.
    if (!ph::isSignalBurst(burst))
    {
      return NAN;
    }
    return lastPhDiag.sensorMv;
  }

  PhDiagnostics lastPhDiagnostics()
  {
    return lastPhDiag;
  }

  float readTemperatureCelsius()
  {
    const TemperatureRead temperature = readTemperature();
    return temperature.status == TemperatureStatus::Ok ? temperature.celsius : NAN;
  }

  float pollTemperatureCelsius(unsigned long nowMs)
  {
    // Unsigned subtraction, so a millis() wrap is harmless (same as pollPh). If a blocking readAll() ran between
    // the request and this read, its own conversion is what the scratchpad holds, which is just as fresh (A7).
    if (tempConversionPending && nowMs - tempConversionStartMs >= ds18b20.millisToWaitForConversion())
    {
      const float celsius = ds18b20.getTempCByIndex(0);
      const bool fault = celsius == DISCONNECTED_C || celsius == DEVICE_DISCONNECTED_C || celsius == POWER_ON_RESET_C;
      cachedTempC = fault ? NAN : celsius;
      cachedTempMs = nowMs;
      cachedTempValid = true;
      tempConversionPending = false;
    }
    if (!tempConversionPending)
    {
      // Re-scan as readTemperature() does, so a replugged probe recovers without a reboot.
      ds18b20.begin();
      if (ds18b20.getDeviceCount() == 0)
      {
        cachedTempC = NAN;
        cachedTempMs = nowMs;
        cachedTempValid = true;
      }
      else
      {
        // Restoring true straight away keeps readAll() and readTemperatureCelsius() blocking.
        ds18b20.setWaitForConversion(false);
        ds18b20.requestTemperatures();
        ds18b20.setWaitForConversion(true);
        tempConversionPending = true;
        tempConversionStartMs = nowMs;
      }
    }
    if (cachedTempValid && nowMs - cachedTempMs <= TEMPERATURE_CACHE_MAX_AGE_MS)
    {
      return cachedTempC;
    }
    return NAN;
  }

  SensorSample readAll()
  {
    const TemperatureRead temperature = readTemperature();
    const TurbidityRead turbidityRead = readTurbidity();
    // D-03 / SENS-11: the DS18B20 reading compensates pH only when it is a real measurement. A temperature fault
    // hands NAN, which PhMath turns into the calibration temperature — an uncompensated pH, never a missing one,
    // because a broken temperature probe must not take the pH reading down with it.
    const PhRead phRead = readPh(temperature.status == TemperatureStatus::Ok ? temperature.celsius : NAN);

    // The invariant the backend and the wire share: a status other than ok never travels with a value.
    const float celsius = temperature.status == TemperatureStatus::Ok ? temperature.celsius : NAN;
    const float ntu = turbidityRead.status == TurbidityStatus::Ok ? turbidityRead.ntu : NAN;
    const float phValue = phRead.status == PhStatus::Ok ? phRead.ph : NAN;

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
    if (phRead.status != lastPhStatus)
    {
      Serial.printf("[sensors] ph: %s\n", sensors::statusToken(phRead.status));
      lastPhStatus = phRead.status;
    }

    // Positional with all six fields spelled out — a positional initializer that names only temperature
    // would value-initialize turbidity to 0.0f, and 0.0f is the one value that must never reach the wire: it
    // is a perfectly plausible "crystal clear water" reading, so it would be stored and charted as a real
    // measurement instead of being omitted. An omitted status would likewise become Ok. wire::buildBody
    // drops the key entirely for NAN. Since 0.7.0 pH always carries a real status (never NotFitted), so
    // sensors.ph is always sent and the ph value only with ok.
    return SensorSample{celsius, ntu, phValue, temperature.status, turbidityRead.status, phRead.status};
  }
}
