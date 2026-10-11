#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Every pH number and every decision that turns a divided pin voltage into a reported pH lives here and nowhere
// else. Like lib/TurbidityMath this header is free of the Arduino core header, of pin constants and of the
// calibrated millivolt ADC call, so [env:native] compiles it and test_ph_math (TEST-08) pins the fit, the
// temperature compensation and every fault decision in about a second, without a board, a probe or a buffer
// sachet on the desk. The hardware-touching part (attenuation, the burst reads, the window's scheduling) stays
// in Sensors.cpp, which is never compiled natively.
//
// Header-only on purpose: it needs no .cpp and must never be added to [env:native]'s lib_ignore, which would
// strip its include path along with its (non-existent) sources. It must not include Sensors.h (lib/Provisioning
// includes this header, and LDF would then chase Sensors across libraries) nor TurbidityMath.h (the two sensors
// share a pattern, not a dependency), which is why ph::Status is its own enum, mapped onto PhStatus in
// Sensors.cpp.
//
// D-01: this is our own math. No DFRobot_PH / DFRobot_ESP_PH library and no vendor "3.5 * V" formula: the
// conversion is a measured two-point line, so the board's real gain and offset come from the buffers, not from
// a datasheet constant for a different revision of the board.
//
// Constants marked PROVISIONAL are the ones plan 10-09 replaces from the recorded bench CSVs; a reading that turns
// out wrong on the bench is fixed by editing one named number, not arithmetic. test_ph_math is written against
// the names, so changing a value does not require rewriting the suite.
namespace ph
{
  // PROVISIONAL: same 64-read trimmed-mean burst as turbidity (WiFi TX puts one-sided spikes on ESP32 ADC reads,
  // which a trimmed mean rejects). Kept until the 10-07 noise data says pH needs a different N or trim.
  constexpr size_t BURST_SAMPLES = 64;
  constexpr size_t TRIM_EACH_END = 16;

  // D-15: the reported pH is the median of the last REPORT_BURSTS bursts spread across the report interval,
  // from day one, for the same reason turbidity moved to it in 0.6.1 — one ~64 ms burst must not become the
  // reading. Odd so the median is one real burst.
  constexpr size_t REPORT_BURSTS = 5;
  static_assert(REPORT_BURSTS % 2 == 1, "REPORT_BURSTS must be odd so the median is one real burst");

  // The fewest bursts a report may be taken from; 3 is the smallest count where a single bad burst is outvoted.
  constexpr size_t MIN_REPORT_BURSTS = 3;
  static_assert(MIN_REPORT_BURSTS % 2 == 1, "MIN_REPORT_BURSTS must be odd");
  static_assert(MIN_REPORT_BURSTS <= REPORT_BURSTS, "MIN_REPORT_BURSTS cannot exceed the window size");

  // Wiring facts, not bench numbers: 10 kO on top, 12 kO to GND, and (D-12) a 1 MO pull-down in the GPIO pin's
  // own row, which sits in parallel with the bottom resistor. The pull-down is what drags a floating input to
  // the floor instead of letting it wander to a believable pH. Changing the resistors is the only thing that may
  // change these lines; the ratio is computed from them so it cannot drift from the parts list.
  constexpr float R_TOP_OHM = 10000.0f;
  constexpr float R_BOTTOM_OHM = 12000.0f;
  constexpr float R_PULLDOWN_OHM = 1000000.0f;
  constexpr float DIVIDER_RATIO = ((R_BOTTOM_OHM * R_PULLDOWN_OHM) / (R_BOTTOM_OHM + R_PULLDOWN_OHM)) /
                                  (R_TOP_OHM + (R_BOTTOM_OHM * R_PULLDOWN_OHM) / (R_BOTTOM_OHM + R_PULLDOWN_OHM));

  // PROVISIONAL: borrowed from turbidity. On this ESP32 an unplugged turbidity pin read a constant 142 mV and 250
  // sat clear of both that and the lowest real reading (03-BENCH-RECORD). The pH pin has a different source and a
  // pull-down, so 10-09 replaces this from the 10-07/10-08 floating-pin and lowest-buffer CSVs. Note (RESEARCH
  // divider table): on a rising board pH ~2 lands near this floor, so a very acidic reading may report no_signal
  // rather than over_range — both send no value.
  constexpr float FAULT_FLOOR_PIN_MV = 250.0f;

  // PROVISIONAL (D-12 instability backstop): a burst whose kept-window spread exceeds this is a floating or
  // disturbed input, not a measurement. 300 sits well above turbidity's largest normal burst spread on this ADC
  // (60-71 mV, 03-BENCH-RECORD section 2.5); 10-09 replaces it from the pH CSVs, below the floating-pin spread
  // measured before the pull-down is fitted.
  constexpr float MAX_BURST_SPREAD_PIN_MV = 300.0f;

  // PROVISIONAL: DFRobot-class pH boards rise with pH (higher pH -> higher output). The bench step B3 decides the
  // real polarity of this board. A calibration with the other sign means swapped buffers or a misread capture,
  // so it is refused as uncalibrated rather than silently inverting every reading.
  constexpr int EXPECTED_SLOPE_SIGN = +1;

  // D-14: a computed pH outside this window is over_range and never sent — not clamped to an edge, and never 0
  // or 14. Edges are confirmed in 10-09 and pinned by test_classify_plausible_window_edges.
  constexpr float PLAUSIBLE_MIN_PH = 2.0f;
  constexpr float PLAUSIBLE_MAX_PH = 12.0f;

  // The electrode's isopotential point, where the Nernst slope pivots and temperature has no effect.
  constexpr float ISOPOTENTIAL_PH = 7.0f;
  constexpr float KELVIN_OFFSET = 273.15f;

  // SENS-07: two decimals. Rounding before serialization keeps the signed JSON bytes deterministic.
  constexpr float PH_ROUND_DIVISOR = 100.0f;

  // Calibration validity. A never-written NVS reads back zeros (version 0), which fails here -> uncalibrated.
  constexpr uint8_t PH_CAL_VERSION = 1;
  // 1.00 pH: keeps the fit away from a divide-by-near-zero. Storage validity only: the portal's ~2 pH acceptance rule is ph::accept::PORTAL_MIN_BUFFER_GAP_HUNDREDTHS in PhAcceptance.h, and this stays 100 so a stored bench calibration is never silently invalidated (Phase 11 D-09).
  constexpr uint16_t MIN_BUFFER_GAP_HUNDREDTHS = 100;
  // Above the sensor's ~4.6 V supply; a stored point beyond it is corrupt.
  constexpr uint16_t MAX_SENSOR_MV = 5000;
  // 0..60 C in centi-degC.
  constexpr int16_t MAX_TCAL_CENTI_C = 6000;

  // One ADC burst reduced to three numbers.
  struct Burst
  {
    float rawMeanMv;   // mean of every sample, diagnostics only
    float filteredMv;  // trimmed mean (pin-side), the value converted
    float spreadMv;    // highest minus lowest of the kept window, the burst's stability
  };

  // Mirrors the D-17 NVS keys one for one: ph_mv_a, ph_buf_a, ph_mv_b, ph_buf_b (sensor-side mV and the buffer pH
  // actually used, in hundredths), ph_tcal (centi-degC), ph_cal_at (Unix seconds, 0 = unknown), ph_ver.
  struct Calibration
  {
    uint16_t mvA;
    uint16_t bufA;
    uint16_t mvB;
    uint16_t bufB;
    int16_t tcalCentiC;
    uint32_t calAt;
    uint8_t version;
  };

  // The fitted line: signed sensor-side mV per pH, the mV at pH 7, and the calibration temperature.
  struct Line
  {
    float slopeMvPerPh;
    float mvAtIsopotential;
    float tcalC;
  };

  enum class Status : uint8_t
  {
    Ok,
    NoSignal,
    Uncalibrated,
    OverRange,
  };

  // Trimmed mean of `count` samples, dropping `trimEachEnd` from each end after sorting. Copy of
  // turbidity::summarizeBurst (a copy, so this library has no cross-library include). NOTE: sorts `samplesMv`
  // in place. All-NAN when nothing is left to average.
  inline Burst summarizeBurst(uint16_t *samplesMv, size_t count, size_t trimEachEnd)
  {
    if (samplesMv == nullptr || count == 0 || count <= 2 * trimEachEnd)
    {
      return Burst{NAN, NAN, NAN};
    }

    double rawSum = 0.0;
    for (size_t i = 0; i < count; i++)
    {
      rawSum += static_cast<double>(samplesMv[i]);
    }

    std::sort(samplesMv, samplesMv + count);

    const size_t first = trimEachEnd;
    const size_t last = count - trimEachEnd - 1;
    double keptSum = 0.0;
    for (size_t i = first; i <= last; i++)
    {
      keptSum += static_cast<double>(samplesMv[i]);
    }
    const size_t kept = last - first + 1;

    Burst out;
    out.rawMeanMv = static_cast<float>(rawSum / static_cast<double>(count));
    out.filteredMv = static_cast<float>(keptSum / static_cast<double>(kept));
    out.spreadMv = static_cast<float>(samplesMv[last]) - static_cast<float>(samplesMv[first]);
    return out;
  }

  // Undoes the divider (pull-down included): what the sensor board put out, given what the pin saw.
  inline float sensorMvFromPinMv(float pinMv)
  {
    return pinMv / DIVIDER_RATIO;
  }

  // The single burst-level signal test, shared by the report window and classify() step 1 (Pitfall 5). If the
  // backstop lived only in the window, a majority of unstable bursts would make reportBurst return the newest
  // failed burst — whose filtered value may be well above the floor — and a classify() that only looked at the
  // filtered value would call it Ok and send a number. Written as NOT (spread > max) so the comparison has one
  // obvious boundary (a spread exactly at the backstop is still signal).
  inline bool isSignalBurst(const Burst &b)
  {
    return !std::isnan(b.filteredMv) && b.filteredMv >= FAULT_FLOOR_PIN_MV && !(b.spreadMv > MAX_BURST_SPREAD_PIN_MV);
  }

  // A ring of the most recent bursts. A value-initialized BurstWindow{} is empty.
  struct BurstWindow
  {
    Burst bursts[REPORT_BURSTS];
    size_t count; // entries held, capped at REPORT_BURSTS
    size_t next;  // slot the next push writes, i.e. the oldest entry once the ring is full
  };

  inline void pushBurst(BurstWindow &w, const Burst &b)
  {
    w.bursts[w.next] = b;
    w.next = (w.next + 1) % REPORT_BURSTS;
    if (w.count < REPORT_BURSTS)
    {
      w.count++;
    }
  }

  // Counted on entries, not valid entries, so a dead pin bounds the warm-up instead of looping forever.
  inline size_t topUpNeeded(const BurstWindow &w)
  {
    return w.count < MIN_REPORT_BURSTS ? MIN_REPORT_BURSTS - w.count : 0;
  }

  // Same rules as turbidity::reportBurst: the median (by rank count, no reordering) of the valid bursts when a
  // strict majority of the window is valid, an even valid count dropping the oldest; otherwise the newest failed
  // burst as-is, so classify() reports NoSignal and the diagnostics show the real failed millivolts.
  inline Burst reportBurst(const BurstWindow &w)
  {
    if (w.count == 0)
    {
      return Burst{NAN, NAN, NAN};
    }

    const size_t oldest = (w.next + REPORT_BURSTS - w.count) % REPORT_BURSTS;
    size_t validIdx[REPORT_BURSTS];
    size_t valid = 0;
    for (size_t k = 0; k < w.count; k++)
    {
      const size_t idx = (oldest + k) % REPORT_BURSTS;
      if (isSignalBurst(w.bursts[idx]))
      {
        validIdx[valid++] = idx;
      }
    }

    if (2 * valid > w.count)
    {
      const size_t start = (valid % 2 == 0) ? 1 : 0;
      const size_t n = valid - start;
      for (size_t i = start; i < valid; i++)
      {
        const float fi = w.bursts[validIdx[i]].filteredMv;
        size_t below = 0;
        size_t atOrBelow = 0;
        for (size_t j = start; j < valid; j++)
        {
          const float fj = w.bursts[validIdx[j]].filteredMv;
          if (fj < fi)
          {
            below++;
          }
          if (fj <= fi)
          {
            atOrBelow++;
          }
        }
        if (below <= n / 2 && n / 2 < atOrBelow)
        {
          return w.bursts[validIdx[i]];
        }
      }
    }

    for (size_t k = w.count; k > 0; k--)
    {
      const size_t idx = (oldest + k - 1) % REPORT_BURSTS;
      if (!isSignalBurst(w.bursts[idx]))
      {
        return w.bursts[idx];
      }
    }
    return Burst{NAN, NAN, NAN};
  }

  // SENS-09: a stored calibration is used only if every field is plausible. Zeros (never written), a different
  // schema version, buffers too close to fit reliably, equal millivolts or a slope of the wrong sign all mean the
  // line cannot be trusted. NVS is not atomic across keys, so this re-validates on every use rather than trusting
  // the writer.
  inline bool isValidCalibration(const Calibration &c)
  {
    if (c.version != PH_CAL_VERSION)
    {
      return false;
    }
    if (c.mvA == 0 || c.mvB == 0 || c.mvA > MAX_SENSOR_MV || c.mvB > MAX_SENSOR_MV || c.mvA == c.mvB)
    {
      return false;
    }
    if (c.bufA == 0 || c.bufB == 0 || c.bufA >= 1400 || c.bufB >= 1400)
    {
      return false;
    }
    const int32_t dBuf = static_cast<int32_t>(c.bufB) - static_cast<int32_t>(c.bufA);
    if (dBuf < MIN_BUFFER_GAP_HUNDREDTHS && -dBuf < MIN_BUFFER_GAP_HUNDREDTHS)
    {
      return false;
    }
    if (c.tcalCentiC < 0 || c.tcalCentiC > MAX_TCAL_CENTI_C)
    {
      return false;
    }
    const int32_t dMv = static_cast<int32_t>(c.mvB) - static_cast<int32_t>(c.mvA);
    const int slopeSign = ((dMv > 0) == (dBuf > 0)) ? 1 : -1;
    return slopeSign == EXPECTED_SLOPE_SIGN;
  }

  // Two-point line through the stored points, signed, extrapolated to the isopotential point. Polarity-agnostic
  // on purpose: the sign rule is isValidCalibration's, so the bench console can still show a wrong-sign fit.
  inline Line fit(const Calibration &c)
  {
    const float phA = static_cast<float>(c.bufA) / 100.0f;
    const float phB = static_cast<float>(c.bufB) / 100.0f;
    const float slope = (static_cast<float>(c.mvB) - static_cast<float>(c.mvA)) / (phB - phA);
    const float mv7 = static_cast<float>(c.mvA) + slope * (ISOPOTENTIAL_PH - phA);
    return Line{slope, mv7, static_cast<float>(c.tcalCentiC) / 100.0f};
  }

  // D-03 / SENS-11: slope-only Nernst compensation. The electrode slope scales with absolute temperature and
  // pivots on the isopotential point, so pH 7 is invariant and 9.50 at Tcal 25 C reads 9.46 at 30 C. A missing
  // temperature (NAN from a faulted DS18B20, or any non-finite value) falls back to Tcal — an uncompensated
  // reading, never a NAN pH, because a temperature fault must not take the pH reading down with it.
  inline float phFromSensorMv(float sensorMv, const Line &l, float tempC)
  {
    const float t = std::isfinite(tempC) ? tempC : l.tcalC;
    const float slopeAtT = l.slopeMvPerPh * ((t + KELVIN_OFFSET) / (l.tcalC + KELVIN_OFFSET));
    return ISOPOTENTIAL_PH + (sensorMv - l.mvAtIsopotential) / slopeAtT;
  }

  // Divide (not multiply by 0.01) so the result is the float nearest k/100. NAN stays the "no reading" marker.
  inline float roundPh(float v)
  {
    if (std::isnan(v))
    {
      return NAN;
    }
    return std::round(v * PH_ROUND_DIVISOR) / PH_ROUND_DIVISOR;
  }

  // The single home of every fault decision, in this order (first match wins):
  //   1. NoSignal     — isSignalBurst(burst) fails: NAN, under the floor, or over the instability backstop.
  //   2. Uncalibrated — the stored calibration cannot be trusted (checked before the conversion needs it).
  //   3. OverRange    — the unrounded computed pH is outside 2..12 or not finite (D-14).
  //   4. Ok.
  // phFromBurst refuses on anything but Ok, so the status a unit reports and whether it sends a value can never
  // disagree (test_classify_ok_exactly_when_ph_is_finite sweeps that).
  inline Status classify(const Burst &burst, const Calibration &cal, float tempC)
  {
    if (!isSignalBurst(burst))
    {
      return Status::NoSignal;
    }
    if (!isValidCalibration(cal))
    {
      return Status::Uncalibrated;
    }
    const float value = phFromSensorMv(sensorMvFromPinMv(burst.filteredMv), fit(cal), tempC);
    if (!std::isfinite(value) || value < PLAUSIBLE_MIN_PH || value > PLAUSIBLE_MAX_PH)
    {
      return Status::OverRange;
    }
    return Status::Ok;
  }

  // The single entry point. NAN unless classify() says Ok; never clamped to an edge (D-14).
  inline float phFromBurst(const Burst &burst, const Calibration &cal, float tempC)
  {
    if (classify(burst, cal, tempC) != Status::Ok)
    {
      return NAN;
    }
    return roundPh(phFromSensorMv(sensorMvFromPinMv(burst.filteredMv), fit(cal), tempC));
  }

  // C++11 only: the ESP32 Arduino core compiles at gnu++11 and will compile this header once Sensors.cpp and
  // Provisioning include it, while [env:native] builds at gnu++17. So no C++17 clamp helper, no structured bindings, no
  // multi-statement constexpr functions — std::min/std::max and single-expression constexpr only. A C++17 call
  // here builds green on the host and fails the board with "'clamp' is not a member of 'std'".
}
