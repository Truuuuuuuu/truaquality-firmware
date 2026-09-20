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

  // PROVISIONAL (research estimate, D-01) — the bench measures this from an unplugged signal wire, an
  // unplugged 5 V supply and the muddiest sample, and Phase 3 does not close until the measured floor is
  // here. Below it the signal or the supply is gone: R2 pulls the pin toward GND and an unguarded curve
  // would report maximum turbidity for a dead sensor, which is exactly the false CRITICAL alert SENS-03
  // exists to prevent.
  constexpr float FAULT_FLOOR_PIN_MV = 200.0f;

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

  // PROVISIONAL plausible window for a stored clear-water reading (D-17), from DFRobot's "pure water
  // outputs 4.1 +/- 0.3 V" at 10-50 C. A value outside it, or the 0 that NVS returns when the unit was
  // never calibrated, means the calibration cannot be trusted — not that the water is turbid. The bench
  // narrows this once real clones have been read in clean water.
  constexpr uint16_t CLEAR_WATER_MIN_MV = 3800;
  constexpr uint16_t CLEAR_WATER_MAX_MV = 4400;

  // PROVISIONAL (D-03). A normalized voltage more than this fraction above the reference means the 5 V rail
  // drifted up or the stored calibration is stale, so the reading is not trustworthy and must be NAN rather
  // than a confident 0. The bench sets the real margin from the observed supply and clear-water drift.
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

  // PROVISIONAL (D-07). Rounding before serialization keeps the signed JSON bytes deterministic, which the
  // wire-format parity fixture depends on; the step is tightened or loosened to the measured noise floor in
  // plan 06 (the Arduino forum reports about +/-7 NTU of resolution on this sensor family).
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

  // The single entry point. The steps run in this exact order so every fault and clamp has one unambiguous
  // outcome and no two of them can both claim a reading.
  inline float ntuFromPinMv(float pinMv, uint16_t clearWaterMv)
  {
    // 1. Dead signal or dead 5 V supply (D-01). R2 pulls an unplugged pin toward GND, which the curve would
    //    happily read as maximum turbidity; NAN is the only honest answer and the upload omits it.
    if (std::isnan(pinMv) || pinMv < FAULT_FLOOR_PIN_MV)
    {
      return NAN;
    }

    // 2. No trustworthy reference to normalize against (D-04 uncalibrated, D-17 implausible).
    if (!isPlausibleClearWaterMv(clearWaterMv))
    {
      return NAN;
    }

    // 3. Ratio normalization (D-05): scaling by the unit's own clear-water reading cancels the divider
    //    tolerance, the unit's actual 5 V rail and the LED/phototransistor spread between clones at once,
    //    which is what makes one vendor curve usable across units.
    const float sensorVolts = sensorMvFromPinMv(pinMv) / 1000.0f;
    const float vEff = sensorVolts * (VENDOR_ZERO_V / (static_cast<float>(clearWaterMv) / 1000.0f));

    // 4. Far above the reference (D-03): the supply drifted up or the stored calibration is stale. Not 0 —
    //    "impossibly clean" is a fault report, not a measurement.
    if (vEff > VENDOR_ZERO_V * (1.0f + HIGH_VOLTAGE_MARGIN))
    {
      return NAN;
    }

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
    return roundNtu(std::clamp(ntu, 0.0f, NTU_CEILING));
  }
}
