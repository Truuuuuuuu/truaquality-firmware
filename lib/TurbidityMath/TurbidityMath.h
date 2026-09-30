#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Every number the bench (plan 06) will change, and every decision that turns a divided sensor voltage into
// a reported NTU, lives here and nowhere else. The module is deliberately free of the Arduino core header,
// of pin constants and of the calibrated millivolt ADC call, so [env:native] compiles it and
// test_turbidity_math can pin the fault floor and both clamps in about a second without a board, a 5 V
// supply or a mud sample on the desk. The hardware-touching part — attenuation, the 64-read burst, the 1 ms
// spacing — stays in Sensors.cpp, which is never compiled natively and is free to depend on Arduino.
//
// Symptom, if the rule is broken here: `pio test -e native` failing with a missing-Arduino-core-header
// error that names *this* file. The same error naming Sensors.cpp instead means something broke the
// lib_ignore / -Ilib/Sensors pair in platformio.ini, not this header. This library is header-only on
// purpose — it needs no .cpp and must never be added to lib_ignore, which would strip its include path.
//
// The constants below are the whole point of the module: a reading that turns out wrong on the bench is
// fixed by editing one named number with its origin in the comment, not by editing arithmetic.
namespace turbidity
{
  // Espressif's multisampling guidance is around 64 reads with a 100 nF bypass on the pin. A *trimmed*
  // mean, not a plain average and not a plain median: WiFi TX bursts put one-sided spikes on ESP32 ADC
  // reads, which a trimmed mean rejects like a median while still averaging quantization noise away like a
  // mean. PROVISIONAL count/trim — the bench's measured burst spread may justify a different N (D-14).
  constexpr size_t BURST_SAMPLES = 64;
  constexpr size_t TRIM_EACH_END = 16;

  // Not provisional and not a bench number: a wiring fact. R1 = 10 kO on top, R2 = 12 kO to GND, so the pin
  // sees 12/22 = 0.545 of the sensor output and the sensor's 4.5 V full scale lands at 2.45 V, the top of
  // the ADC's 11 dB range. Changing the resistors is the only thing that may change this line.
  constexpr float DIVIDER_RATIO = 12.0f / 22.0f;

  // MEASURED (03-06 bench, D-01). With the signal wire or the 5 V supply unplugged the pin reads a constant
  // 142 mV (runs 02 and 03, 121 and 124 rows); the muddiest real sample (40 caps of cornstarch stock, run 11)
  // never went below 357.5 mV. 250 is a round number between them (midpoint 249.8), clear of both. Below it
  // the signal or the supply is gone: R2 pulls the pin toward GND and an unguarded curve would report maximum
  // turbidity for a dead sensor, which is exactly the false CRITICAL alert SENS-03 exists to prevent. Bench
  // record: .planning/phases/03-turbidity-sensor-read-bench-characterization/03-BENCH-RECORD.md.
  constexpr float FAULT_FLOOR_PIN_MV = 250.0f;

  // The DFRobot SEN0189 vendor curve, NTU = A*V^2 + B*V + C, valid for 2.5 V <= V <= 4.2 V sensor-side,
  // evaluated on a ratio-normalized voltage (D-05). PROVISIONAL as a block: if the bench data disagrees
  // with the vendor curve, the refit replaces these four coefficients together and nothing else in this
  // file has to move. NTU_CEILING is the saturated value reported below the curve's range (D-02), not an
  // extrapolation and not a fault.
  constexpr float VENDOR_ZERO_V = 4.2f;
  constexpr float CURVE_MIN_V = 2.5f;
  constexpr float CURVE_A = -1120.4f;
  constexpr float CURVE_B = 5742.3f;
  constexpr float CURVE_C = -4352.9f;
  constexpr float NTU_CEILING = 3000.0f;

  // NOT provisional and NOT a bench number — do not replace this with a measured figure and do not write
  // the residue in as a literal. The published coefficients do not land on exactly 0 at their own zero
  // point; they leave a small positive residue there. Without subtracting it, the zero clamp below and the
  // curve branch disagree by that residue right at the boundary, so a unit in clear water steps between two
  // different "zero" answers depending on which branch it takes. Slack wide enough to paper over the
  // disagreement would also be wide enough to hide real turbidity, so the curve is zero-referenced instead.
  // Computing the offset from the same coefficients means a refit in plan 06 re-zeroes the curve for free.
  constexpr float CURVE_ZERO_OFFSET_NTU =
      CURVE_A * VENDOR_ZERO_V * VENDOR_ZERO_V + CURVE_B * VENDOR_ZERO_V + CURVE_C;

  // MEASURED on one bench rig (03-06, D-17). Plausible window for a stored clear-water reference. A value
  // outside it, or the 0 that NVS returns when the unit was never calibrated, means the calibration cannot
  // be trusted — not that the water is turbid. The datasheet's "pure water outputs 4.1 +/- 0.3 V" (3800-4400)
  // refused every clear-water capture on this rig: the module runs from the ESP32's 5V pin (4.5-4.7 V over
  // USB, after the board's diode) and clear water read 3,025-3,428 mV sensor-side across the session (mean
  // of the clear-water runs, two glasses, two days). The bounds are that spread plus 150 mV of margin, rounded
  // to 10 mV: 3,025 - 150 -> 2870, 3,428 + 150 -> 3580. The container matters (the same water read 3,388 mV in
  // one glass and 3,156 mV in another), so this window is a property of this rig's supply and geometry, not of
  // the sensor model; a production unit with a regulated 5 V supply needs its own bench check.
  constexpr uint16_t CLEAR_WATER_MIN_MV = 2870;
  constexpr uint16_t CLEAR_WATER_MAX_MV = 3580;

  // Checked against the bench (03-06, D-03): the largest excursion above the stored reference seen in any
  // calibrated water run was 0.080 (a fraction of the reference), so a margin of at least 0.100 is needed for
  // a working unit's normal two-level supply jump not to become a fault. 0.15 keeps that with headroom. A
  // normalized voltage more than this fraction above the reference means the 5 V rail drifted up or the
  // stored calibration is stale, so the reading is not trustworthy and must be NAN rather than a confident 0.
  constexpr float HIGH_VOLTAGE_MARGIN = 0.15f;

  // NOT provisional and NOT a bench number: a float-robustness guard and nothing else. Plan 06 must not
  // "fix" it from measured data.
  // Why it exists: a reading genuinely sitting at the unit's calibrated clear-water reference still travels
  // through a divide by DIVIDER_RATIO, a divide by the stored millivolts and a multiply by VENDOR_ZERO_V.
  // A float error of roughly 1e-6 V accumulated in that chain is enough to land the normalized voltage a
  // hair *below* the reference and fall through to the curve, so an exact comparison there is a knife-edge.
  // Why 0.0001 V and not more: the curve's slope at the zero point is about -3669 NTU/V, i.e. about
  // 3.7 NTU per millivolt, so every millivolt of slack here is a dead zone worth about 3.7 NTU. A 0.01 V
  // epsilon would silently clamp roughly 37 NTU of real fishpond range to 0. This value is a hundred times
  // the float error and worth under 0.4 NTU, so it removes the knife-edge and hides nothing measurable.
  // A deliberate dead band, if the bench ever justifies one, belongs in NTU_ROUND_STEP or in a separate
  // named bench-derived constant — never here.
  constexpr float ZERO_EPSILON_V = 0.0001f;

  // PROVISIONAL (D-07), deliberately left at 0.1 after the 03-06 bench. Rounding before serialization keeps
  // the signed JSON bytes deterministic, which the wire-format parity fixture depends on. The bench measured
  // the clear-water noise at 30.8-32.6 mV at the pin (standard deviation of filtered_pin_mv), but its NTU
  // equivalent (about 250-315 NTU) comes from the unvalidated vendor curve evaluated outside its 2.5-4.2 V
  // range, and most of it is the two-level supply jump (~120 mV sensor-side), which a separate 5 V supply
  // may remove. Rounding to 500 NTU from that number would make the 25 NTU BFAR safe line unreadable, so the
  // step stays until a turbidimeter validates the curve and the supply noise is fixed.
  constexpr float NTU_ROUND_STEP = 0.1f;

  // One ADC burst reduced to the three numbers the bench CSV (D-13) and the capture-stability check read.
  struct Burst
  {
    float rawMeanMv;   // mean of every sample, kept for diagnostics — shows what the trim removed
    float filteredMv;  // trimmed mean, the value the NTU conversion is fed
    float spreadMv;    // highest minus lowest of the kept window, the burst's stability
  };

  // Trimmed mean of `count` samples, dropping `trimEachEnd` from each end after sorting. NOTE: sorts
  // `samplesMv` in place — the caller's array comes back reordered. Returns all-NAN when there is nothing
  // left to average (count == 0, or count <= 2 * trimEachEnd), because a caller that shrinks the burst
  // below the trim must get a refusal rather than a silently degenerate average or a read past the array.
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

  // Undoes the divider: what the sensor put out, given what the pin saw. Only diagnostics and the fault
  // floor need it — the ratio normalization below cancels the divider out of the NTU result.
  inline float sensorMvFromPinMv(float pinMv)
  {
    return pinMv / DIVIDER_RATIO;
  }

  // A stored clear-water reading is usable only inside the plausible window. 0 is NVS's "never calibrated"
  // default and anything outside the window is a corrupted or hostile value; both are uncalibrated (D-17).
  inline bool isPlausibleClearWaterMv(uint16_t clearWaterMv)
  {
    return clearWaterMv >= CLEAR_WATER_MIN_MV && clearWaterMv <= CLEAR_WATER_MAX_MV;
  }

  // Quantizes to the declared step so the serialized bytes are reproducible. NAN survives as NAN, because a
  // rounded NAN must stay the "no reading" marker the upload omits rather than becoming a number.
  inline float roundNtu(float ntu)
  {
    if (std::isnan(ntu))
    {
      return NAN;
    }
    return std::round(ntu / NTU_ROUND_STEP) * NTU_ROUND_STEP;
  }

  // Why a reading is or isn't usable. Its own enum rather than sensors::TurbidityStatus on purpose: this
  // header must not include Sensors.h, because lib/Provisioning includes this header and LDF would then have
  // to chase Sensors across libraries. Sensors.cpp maps one onto the other with an explicit switch.
  enum class Status : uint8_t
  {
    Ok,
    NoSignal,
    Uncalibrated,
    OverRange,
  };

  // Step 3 of the conversion, on its own so classify() and ntuFromPinMv() compute the same number.
  // Ratio normalization (D-05): scaling by the unit's own clear-water reading cancels the divider
  // tolerance, the unit's actual 5 V rail and the LED/phototransistor spread between clones at once, which
  // is what makes one vendor curve usable across units.
  inline float normalizedVolts(float pinMv, uint16_t clearWaterMv)
  {
    const float sensorVolts = sensorMvFromPinMv(pinMv) / 1000.0f;
    return sensorVolts * (VENDOR_ZERO_V / (static_cast<float>(clearWaterMv) / 1000.0f));
  }

  // The single home of every fault decision (steps 1, 2 and 4), in the order ntuFromPinMv has always run
  // them. The unit reports the result as its turbidity status, and ntuFromPinMv refuses on anything but Ok,
  // so the status sent and whether a value is sent can never disagree (the native equivalence sweep pins it).
  inline Status classify(float pinMv, uint16_t clearWaterMv)
  {
    // 1. Dead signal or dead 5 V supply (D-01). R2 pulls an unplugged pin toward GND, which the curve would
    //    happily read as maximum turbidity; NAN is the only honest answer and the upload omits it.
    if (std::isnan(pinMv) || pinMv < FAULT_FLOOR_PIN_MV)
    {
      return Status::NoSignal;
    }

    // 2. No trustworthy reference to normalize against (D-04 uncalibrated, D-17 implausible). Checked
    //    before step 4, which needs a trustworthy reference to mean anything.
    if (!isPlausibleClearWaterMv(clearWaterMv))
    {
      return Status::Uncalibrated;
    }

    // 4. Far above the reference (D-03): the supply drifted up or the stored calibration is stale. Not 0 —
    //    "impossibly clean" is a fault report, not a measurement.
    if (normalizedVolts(pinMv, clearWaterMv) > VENDOR_ZERO_V * (1.0f + HIGH_VOLTAGE_MARGIN))
    {
      return Status::OverRange;
    }

    return Status::Ok;
  }

  // The single entry point. The steps run in this exact order so every fault and clamp has one unambiguous
  // outcome and no two of them can both claim a reading. Steps 1, 2 and 4 are classify()'s.
  inline float ntuFromPinMv(float pinMv, uint16_t clearWaterMv)
  {
    if (classify(pinMv, clearWaterMv) != Status::Ok)
    {
      return NAN;
    }

    // 3. Ratio normalization — see normalizedVolts().
    const float vEff = normalizedVolts(pinMv, clearWaterMv);

    // 5. At or above the reference, clamp to 0 and never negative (D-03). The comparison is deliberately an
    //    inequality with named slack rather than a bare >= VENDOR_ZERO_V: an exact comparison is a
    //    knife-edge that falls through to the curve whenever the float chain in step 3 lands a few ULPs
    //    low, so a unit reading its own calibrated clear water would report a small non-zero NTU. The
    //    epsilon is sized to absorb exactly that and nothing more — at about 3.7 NTU per millivolt of
    //    slack, a wider one reports clear water for a genuinely turbid pond.
    if (vEff >= VENDOR_ZERO_V - ZERO_EPSILON_V)
    {
      return 0.0f;
    }

    // 6. Muddier than the curve covers (D-02): clamp to the saturated value. Not NAN — the sensor is fine
    //    and the water really is very turbid — and not extrapolated, because below its range the quadratic
    //    turns back downward and would report *less* turbidity for muddier water.
    if (vEff < CURVE_MIN_V)
    {
      return NTU_CEILING;
    }

    // 7. Inside the curve's valid range. Subtracting CURVE_ZERO_OFFSET_NTU is what makes this branch agree
    //    with step 5 at the boundary: at vEff == VENDOR_ZERO_V it evaluates to exactly 0, so the two
    //    branches meet instead of stepping by the coefficients' residue.
    const float ntu = CURVE_A * vEff * vEff + CURVE_B * vEff + CURVE_C - CURVE_ZERO_OFFSET_NTU;
    // std::min/std::max rather than std::clamp, which is C++17: [env:native] builds with -std=gnu++17 but
    // the ESP32 Arduino core compiles at gnu++11, and this header is compiled by BOTH once Sensors.cpp and
    // Provisioning.cpp include it. The nesting is the same value for every input, NAN included. Reaching for
    // std::clamp here again would build green on the host and fail the board with "'clamp' is not a member
    // of 'std'"; raising the board's standard to fix one call would recompile the whole Arduino core,
    // WiFiManager and espMqttClient for no gain.
    return roundNtu(std::min(std::max(ntu, 0.0f), NTU_CEILING));
  }
}
