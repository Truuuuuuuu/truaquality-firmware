#include <unity.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "TurbidityMath.h"
#include "TurbidityCalibration.h"

// Pins the shape of the turbidity math, not its exact coefficient outputs: what an unplugged sensor
// reports, what water muddier than the curve covers reports, what a unit sitting in its own calibrated
// clear water reports, and that the trimmed mean actually rejects the one-sided spikes WiFi TX bursts put
// on ADC reads. Every one of those is a decision the bench (plan 06) cannot re-open by editing a constant,
// which is why they are asserted here, on the host, in about a second, rather than discovered on a pond.
namespace
{
  // Shape tolerance only (D-05): the cases below assert the curve's behaviour, so a harmless coefficient
  // refit in plan 06 must not turn the suite red. It must NEVER be widened to absorb a boundary
  // disagreement — if a case sitting on a decision boundary fails, move the fixture off the boundary or fix
  // the constant in the header. A wider tolerance here would hide exactly the errors this file exists for.
  constexpr float NTU_TOLERANCE = 0.05f;

  // The calibrated clear-water reference most cases run against, sensor-side, inside the plausible window.
  constexpr uint16_t CLEAR_MV = 3200;

  // How far off a decision boundary every fixture is placed. ntuFromPinMv divides by DIVIDER_RATIO, divides
  // by the stored millivolts and multiplies by VENDOR_ZERO_V, so a fixture aimed *exactly* at a boundary
  // can land on either side of it after float rounding and the case would flake rather than assert
  // anything. This margin is about fifty thousand times the float error in that chain (roughly 1e-6 V), and
  // still far too small to carry a fixture across a *different* boundary. It is deliberately much wider
  // than the header's ZERO_EPSILON_V (0.0001 V), so a fixture placed a margin below the reference lands in
  // the curve branch rather than in the zero clamp. The only two cases allowed inside the epsilon are the
  // ones that exist to pin the epsilon itself and to prove it opens no dead zone.
  constexpr float FIXTURE_MARGIN_V = 0.05f;

  // Inverts the production chain: given the normalized voltage a case is actually about, what pin reading
  // produces it. Every fixture is expressed this way so no assertion depends on a hand-computed millivolt
  // literal that silently stops meaning what it meant when a constant changes.
  float pinMvForNormalized(float vEff)
  {
    return vEff * (static_cast<float>(CLEAR_MV) / 1000.0f) / turbidity::VENDOR_ZERO_V * 1000.0f *
           turbidity::DIVIDER_RATIO;
  }

  // Bench-derived capture windows (D-01/D-03/D-04): 20 consecutive 1 Hz rows of the sensor_mv column from the
  // 03-06 bench session, .planning/phases/03-turbidity-sensor-read-bench-characterization/bench/. Real data, so
  // the stability and plausibility guards are pinned against what the rig actually produced, not a guess.

  // 01-clear-water.csv, file lines 2-21: clear water flipping between the two supply levels (~120 mV apart).
  constexpr float RUN01_CLEAR[20] = {3261.2f, 3383.9f, 3252.4f, 3256.5f, 3332.1f, 3254.6f, 3253.2f,
                                     3259.3f, 3254.0f, 3384.7f, 3251.7f, 3253.2f, 3379.8f, 3319.9f,
                                     3382.8f, 3251.5f, 3383.8f, 3252.0f, 3254.4f, 3383.6f};

  // 12-clear-water-repeat-newglass.csv, file lines 2-21: the probe still settling (a ~860 mV dip).
  constexpr float RUN12_SETTLING[20] = {2376.7f, 2386.7f, 2386.1f, 2093.0f, 1983.3f, 1800.2f, 1536.8f,
                                        2381.7f, 2304.4f, 2307.3f, 2394.3f, 2390.0f, 2306.2f, 2389.3f,
                                        2372.4f, 2309.8f, 2395.4f, 2303.8f, 2302.4f, 2390.1f};

  // 12-clear-water-repeat-newglass.csv, file lines 9-28: steady, but below the plausible clear-water window.
  constexpr float RUN12_STEADY_LOW[20] = {2381.7f, 2304.4f, 2307.3f, 2394.3f, 2390.0f, 2306.2f, 2389.3f,
                                          2372.4f, 2309.8f, 2395.4f, 2303.8f, 2302.4f, 2390.1f, 2390.2f,
                                          2393.1f, 2308.8f, 2308.3f, 2387.9f, 2310.1f, 2324.6f};

  // 07-dose1cap-newglass.csv, file lines 2-21: one cap of cornstarch stock — slightly turbid water.
  constexpr float RUN07_ONE_CAP[20] = {2863.6f, 2972.1f, 2972.5f, 2968.6f, 2859.9f, 2984.2f, 2982.9f,
                                       2979.3f, 2943.4f, 2954.4f, 2979.8f, 2971.6f, 2973.5f, 2978.3f,
                                       2900.5f, 2983.0f, 2994.8f, 2998.4f, 2992.8f, 2987.8f};

  // A constant capture window, for the synthetic boundary cases.
  void fillWindow(float *window, float value)
  {
    for (size_t i = 0; i < turbidity::cal::CAPTURE_WINDOW_SAMPLES; i++)
    {
      window[i] = value;
    }
  }

  bool isOutcome(const float *window, size_t count, turbidity::cal::CaptureOutcome expected)
  {
    return turbidity::cal::evaluateCapture(window, count).outcome == expected;
  }
}

void test_trimmed_mean_rejects_one_sided_spikes(void)
{
  // A plain average would let a WiFi TX burst move the reported NTU: the spikes here are one-sided, so they
  // drag the mean up and nothing drags it back. The trim is what makes the reported value survive them.
  uint16_t samples[turbidity::BURST_SAMPLES];
  for (size_t i = 0; i < turbidity::BURST_SAMPLES; i++)
  {
    samples[i] = 1500;
  }
  for (size_t i = 0; i < 12; i++)
  {
    samples[i] = 1900;
  }

  const turbidity::Burst burst =
      turbidity::summarizeBurst(samples, turbidity::BURST_SAMPLES, turbidity::TRIM_EACH_END);

  TEST_ASSERT_FLOAT_WITHIN(NTU_TOLERANCE, 1500.0f, burst.filteredMv);
  TEST_ASSERT_FLOAT_WITHIN(NTU_TOLERANCE, 0.0f, burst.spreadMv);
  // The raw mean is what a naive implementation would have reported: 12 spikes of +400 mV over 64 samples.
  TEST_ASSERT_FLOAT_WITHIN(NTU_TOLERANCE, 1575.0f, burst.rawMeanMv);
  TEST_ASSERT_TRUE(burst.rawMeanMv - burst.filteredMv > 50.0f);
}

void test_trimmed_mean_reports_spread_of_the_kept_window(void)
{
  // spreadMv is read by the bench CSV (D-13) and by Phase 5's capture-stability check, so it has to be the
  // spread of the window that was actually averaged — not of the raw burst, whose outliers were discarded.
  uint16_t samples[turbidity::BURST_SAMPLES];
  for (size_t i = 0; i < turbidity::BURST_SAMPLES; i++)
  {
    samples[i] = static_cast<uint16_t>(1000 + i);
  }

  const turbidity::Burst burst =
      turbidity::summarizeBurst(samples, turbidity::BURST_SAMPLES, turbidity::TRIM_EACH_END);

  // Kept window is indices 16..47 of the sorted ramp: 1016..1047.
  const float keptCount =
      static_cast<float>(turbidity::BURST_SAMPLES - 2 * turbidity::TRIM_EACH_END);
  TEST_ASSERT_FLOAT_WITHIN(NTU_TOLERANCE, keptCount - 1.0f, burst.spreadMv);
  TEST_ASSERT_FLOAT_WITHIN(NTU_TOLERANCE, 1031.5f, burst.filteredMv);
  // The raw burst spans the whole 64-entry ramp, which is strictly wider than the kept window.
  TEST_ASSERT_TRUE(burst.spreadMv < static_cast<float>(turbidity::BURST_SAMPLES) - 1.0f);
}

void test_burst_shorter_than_the_trim_is_nan_not_an_average(void)
{
  // T-03-03: a caller that shrinks the burst below the trim must get a refusal. Without the guard this
  // divides by zero or reads past the array and reports whatever it found as a real turbidity value.
  uint16_t samples[32];
  for (size_t i = 0; i < 32; i++)
  {
    samples[i] = 1500;
  }

  const turbidity::Burst burst = turbidity::summarizeBurst(samples, 32, turbidity::TRIM_EACH_END);

  TEST_ASSERT_FLOAT_IS_NAN(burst.rawMeanMv);
  TEST_ASSERT_FLOAT_IS_NAN(burst.filteredMv);
  TEST_ASSERT_FLOAT_IS_NAN(burst.spreadMv);
}

void test_divider_scaling_recovers_sensor_voltage(void)
{
  // If DIVIDER_RATIO is ever written upside down (22/12 instead of 12/22) every NTU reading is wrong by a
  // factor of 3.4 and still looks like a plausible number, so the ratio is asserted in both directions.
  TEST_ASSERT_FLOAT_WITHIN(NTU_TOLERANCE, 4500.0f,
                           turbidity::sensorMvFromPinMv(4500.0f * turbidity::DIVIDER_RATIO));
  // The wiring fact behind the ratio: the sensor's 4.5 V full output lands at ~2450 mV on the pin, the top
  // of the ADC's 11 dB range. A ratio that put it above 3300 mV would clip every high reading.
  TEST_ASSERT_TRUE(4500.0f * turbidity::DIVIDER_RATIO > 2440.0f);
  TEST_ASSERT_TRUE(4500.0f * turbidity::DIVIDER_RATIO < 2460.0f);
}

void test_pin_below_fault_floor_is_nan_not_zero_and_not_ceiling(void)
{
  // T-03-01 / D-01 / SENS-03. R2 pulls an unplugged or unpowered pin toward GND, and the two values an
  // unguarded curve would produce there are exactly the two asserted against: the ceiling (a false CRITICAL
  // alert for a dead sensor) or 0 (a false "perfectly clean" reading). The 10 mV step needs no fixture
  // margin — this is a plain comparison against a constant with no arithmetic chain in front of it.
  const float justBelow = turbidity::ntuFromPinMv(turbidity::FAULT_FLOOR_PIN_MV - 10.0f, CLEAR_MV);
  TEST_ASSERT_FLOAT_IS_NAN(justBelow);
  TEST_ASSERT_FALSE(justBelow == 0.0f);
  TEST_ASSERT_FALSE(justBelow == turbidity::NTU_CEILING);

  const float grounded = turbidity::ntuFromPinMv(0.0f, CLEAR_MV);
  TEST_ASSERT_FLOAT_IS_NAN(grounded);
  TEST_ASSERT_FALSE(grounded == 0.0f);
  TEST_ASSERT_FALSE(grounded == turbidity::NTU_CEILING);
}

void test_zero_calibration_is_uncalibrated_and_yields_nan(void)
{
  // D-04 / D-17: 0 is what NVS returns for a unit that was never calibrated. A healthy pin voltage plus a
  // missing reference must report nothing rather than a confidently wrong number the dashboard would show.
  TEST_ASSERT_FLOAT_IS_NAN(turbidity::ntuFromPinMv(pinMvForNormalized(3.5f), 0));
}

void test_calibration_outside_the_plausible_window_yields_nan(void)
{
  // T-03-02 / D-17: a corrupted or hostile stored value scales every reading by whatever it says. These are
  // integer comparisons against the window bounds, so the 50 mV step is only for readability.
  TEST_ASSERT_FLOAT_IS_NAN(turbidity::ntuFromPinMv(
      pinMvForNormalized(3.5f), static_cast<uint16_t>(turbidity::CLEAR_WATER_MIN_MV - 50)));
  TEST_ASSERT_FLOAT_IS_NAN(turbidity::ntuFromPinMv(
      pinMvForNormalized(3.5f), static_cast<uint16_t>(turbidity::CLEAR_WATER_MAX_MV + 50)));
}

void test_clear_water_reference_reads_zero_after_normalization(void)
{
  // D-03: clear water reads exactly 0, never a negative number the backend's bounds would reject. The
  // fixture deliberately sits a margin *above* the reference rather than exactly on it, so this case
  // asserts the clamp branch itself and not a boundary the float chain can flip — that boundary is the next
  // case's job. Asserted exactly, not within a tolerance: "about zero" is not the contract.
  TEST_ASSERT_EQUAL_FLOAT(
      0.0f, turbidity::ntuFromPinMv(
                pinMvForNormalized(turbidity::VENDOR_ZERO_V + FIXTURE_MARGIN_V), CLEAR_MV));
}

void test_at_reference_within_epsilon_still_reads_zero(void)
{
  // This is the case that pins ZERO_EPSILON_V. A unit sitting in its own calibrated clear water lands a
  // hair low after the divide-by-ratio, divide-by-stored-mV, multiply-by-4.2 round trip, so a bare
  // `>= VENDOR_ZERO_V` would fall through to the curve and report a small non-zero turbidity for water it
  // was itself calibrated against. Deleting the epsilon from the header must turn this case red. The
  // fixture is derived from the constant so it follows the epsilon if it ever changes — which is precisely
  // what the next case caps.
  TEST_ASSERT_EQUAL_FLOAT(
      0.0f, turbidity::ntuFromPinMv(
                pinMvForNormalized(turbidity::VENDOR_ZERO_V - turbidity::ZERO_EPSILON_V * 0.5f),
                CLEAR_MV));
}

void test_ten_millivolts_below_reference_is_not_zero(void)
{
  // The anti-dead-zone half. The curve's slope at the zero point is about 3.7 NTU per millivolt, so a point
  // 10 mV below the reference is a real reading of roughly 37 NTU — squarely inside the fishpond range a
  // BFAR staffer is meant to see. A centivolt-scale ZERO_EPSILON_V would clamp it to 0 and hide it
  // silently. The floor is a loose 10 NTU rather than the exact figure so a coefficient refit in plan 06
  // cannot turn this red, while it still fails if the epsilon grows past about 3 mV. A deliberate dead
  // band, if the bench ever justifies one, belongs in NTU_ROUND_STEP or in a named bench-derived constant —
  // never in the zero-clamp epsilon.
  const float ntu = turbidity::ntuFromPinMv(
      pinMvForNormalized(turbidity::VENDOR_ZERO_V - 0.010f), CLEAR_MV);

  TEST_ASSERT_FALSE(std::isnan(ntu));
  TEST_ASSERT_FALSE(ntu == 0.0f);
  TEST_ASSERT_TRUE(ntu > 10.0f);
  TEST_ASSERT_TRUE(ntu < turbidity::NTU_CEILING);
}

void test_above_clear_water_reference_clamps_to_zero_never_negative(void)
{
  // D-03: above the reference the raw quadratic goes negative, and a negative NTU is a number the backend's
  // bounds check would reject and the dashboard could never explain. The fixture sits squarely inside the
  // high-voltage margin rather than just above the reference, so it cannot drift into the NAN branch.
  const float ntu = turbidity::ntuFromPinMv(
      pinMvForNormalized(turbidity::VENDOR_ZERO_V * (1.0f + turbidity::HIGH_VOLTAGE_MARGIN * 0.5f)),
      CLEAR_MV);

  TEST_ASSERT_EQUAL_FLOAT(0.0f, ntu);
  TEST_ASSERT_FALSE(ntu < 0.0f);
}

void test_far_above_clear_water_reference_is_nan(void)
{
  // D-03: well past the margin the unit is not reading impossibly clean water — its 5 V rail drifted up or
  // its stored calibration is stale. That is a fault report, not a measurement, so it must not clamp to 0.
  TEST_ASSERT_FLOAT_IS_NAN(turbidity::ntuFromPinMv(
      pinMvForNormalized(turbidity::VENDOR_ZERO_V * (1.0f + turbidity::HIGH_VOLTAGE_MARGIN * 2.0f)),
      CLEAR_MV));
}

void test_below_curve_range_clamps_to_ceiling_not_nan(void)
{
  // D-02: muddier than the curve covers is a real reading of very turbid water, so it saturates rather than
  // disappearing as NAN or being extrapolated (below its range the quadratic turns back downward and would
  // report *less* turbidity for muddier water). Phase 4's bounds must accept this ceiling. The boundary
  // itself is benign either way — the zero-referenced curve evaluates to within a fraction of an NTU of
  // NTU_CEILING at CURVE_MIN_V and step 7 clamps it there — so the margin here is for clarity, not
  // correctness.
  const float ntu =
      turbidity::ntuFromPinMv(pinMvForNormalized(turbidity::CURVE_MIN_V - FIXTURE_MARGIN_V), CLEAR_MV);

  TEST_ASSERT_FALSE(std::isnan(ntu));
  TEST_ASSERT_EQUAL_FLOAT(turbidity::NTU_CEILING, ntu);
}

void test_curve_is_monotonic_decreasing_across_the_valid_range(void)
{
  // The shape assertion D-05 asks for: more light through the water means a higher voltage means less
  // turbidity, with no fold-back anywhere inside the valid range. A coefficient refit that accidentally put
  // the parabola's vertex inside the range would report the same NTU for two different waters, and nothing
  // else in this suite would notice.
  const int steps = 12;
  const float lowV = turbidity::CURVE_MIN_V + FIXTURE_MARGIN_V;
  float previous = turbidity::NTU_CEILING;

  for (int i = 0; i <= steps; i++)
  {
    const float vEff = lowV + (turbidity::VENDOR_ZERO_V - lowV) * static_cast<float>(i) /
                                  static_cast<float>(steps);
    const float ntu = turbidity::ntuFromPinMv(pinMvForNormalized(vEff), CLEAR_MV);

    TEST_ASSERT_FALSE(std::isnan(ntu));
    TEST_ASSERT_TRUE(ntu >= 0.0f);
    TEST_ASSERT_TRUE(ntu <= turbidity::NTU_CEILING);
    // The top steps fall inside ZERO_EPSILON_V and return 0, which still satisfies "no higher than before".
    TEST_ASSERT_TRUE(ntu <= previous);
    previous = ntu;
  }
}

void test_reported_ntu_is_rounded_to_the_declared_step(void)
{
  // D-07: the signed JSON bytes must be deterministic for the wire-format parity fixture, so what
  // ntuFromPinMv returns is already quantized — rounding it again must change nothing. A value that moved
  // on a second pass would mean the serialized digits depend on float noise rather than on the step.
  const float ntu = turbidity::ntuFromPinMv(pinMvForNormalized(3.5f), CLEAR_MV);
  TEST_ASSERT_FALSE(std::isnan(ntu));
  TEST_ASSERT_EQUAL_FLOAT(ntu, turbidity::roundNtu(ntu));

  // NAN must survive rounding as NAN: it is the "no reading" marker the upload omits, and a rounded NAN
  // that became a number would be published as a fake measurement.
  TEST_ASSERT_FLOAT_IS_NAN(turbidity::roundNtu(NAN));
}

void test_classify_dead_pin_is_no_signal(void)
{
  // The unit reports this as "no_signal": the signal wire or the 5 V supply is unplugged.
  TEST_ASSERT_TRUE(turbidity::classify(NAN, CLEAR_MV) == turbidity::Status::NoSignal);
  TEST_ASSERT_TRUE(turbidity::classify(0.0f, CLEAR_MV) == turbidity::Status::NoSignal);
  TEST_ASSERT_TRUE(turbidity::classify(turbidity::FAULT_FLOOR_PIN_MV - 10.0f, CLEAR_MV) ==
                   turbidity::Status::NoSignal);
  // A dead pin is a dead pin even on an uncalibrated unit: step 1 runs first.
  TEST_ASSERT_TRUE(turbidity::classify(0.0f, 0) == turbidity::Status::NoSignal);
}

void test_classify_missing_or_implausible_calibration_is_uncalibrated(void)
{
  TEST_ASSERT_TRUE(turbidity::classify(pinMvForNormalized(3.5f), 0) == turbidity::Status::Uncalibrated);
  TEST_ASSERT_TRUE(turbidity::classify(pinMvForNormalized(3.5f),
                                       static_cast<uint16_t>(turbidity::CLEAR_WATER_MIN_MV - 50)) ==
                   turbidity::Status::Uncalibrated);
  TEST_ASSERT_TRUE(turbidity::classify(pinMvForNormalized(3.5f),
                                       static_cast<uint16_t>(turbidity::CLEAR_WATER_MAX_MV + 50)) ==
                   turbidity::Status::Uncalibrated);
  // Checked before over-range: a pin that would be far above the reference is still "uncalibrated" when
  // there is no reference to be above — telling an admin "over range" there would send them the wrong way.
  TEST_ASSERT_TRUE(
      turbidity::classify(pinMvForNormalized(turbidity::VENDOR_ZERO_V * (1.0f + turbidity::HIGH_VOLTAGE_MARGIN * 2.0f)),
                          0) == turbidity::Status::Uncalibrated);
}

void test_classify_far_above_reference_is_over_range(void)
{
  TEST_ASSERT_TRUE(
      turbidity::classify(pinMvForNormalized(turbidity::VENDOR_ZERO_V * (1.0f + turbidity::HIGH_VOLTAGE_MARGIN * 2.0f)),
                          CLEAR_MV) == turbidity::Status::OverRange);
}

void test_classify_usable_readings_are_ok(void)
{
  // Clear water (the zero clamp), mid-curve, and muddier than the curve covers (the ceiling) are all real
  // readings — the last one especially: saturated is not a fault.
  TEST_ASSERT_TRUE(turbidity::classify(pinMvForNormalized(turbidity::VENDOR_ZERO_V + FIXTURE_MARGIN_V), CLEAR_MV) ==
                   turbidity::Status::Ok);
  TEST_ASSERT_TRUE(turbidity::classify(pinMvForNormalized(3.5f), CLEAR_MV) == turbidity::Status::Ok);
  TEST_ASSERT_TRUE(turbidity::classify(pinMvForNormalized(turbidity::CURVE_MIN_V - FIXTURE_MARGIN_V), CLEAR_MV) ==
                   turbidity::Status::Ok);
}

void test_classify_ok_exactly_when_ntu_is_finite(void)
{
  // The status the unit reports and whether it sends a value must never disagree: "ok" with no value, or a
  // value under a fault status, would contradict itself on the dashboard. Swept across every branch and
  // across calibrations at and around the window's edges.
  const uint16_t calibrations[] = {0, 2869, 2870, 3200, 3580, 3581};
  for (uint16_t clear : calibrations)
  {
    for (int pin = 0; pin <= 3000; pin += 5)
    {
      const float pinMv = static_cast<float>(pin);
      const bool ok = turbidity::classify(pinMv, clear) == turbidity::Status::Ok;
      const bool finite = std::isfinite(turbidity::ntuFromPinMv(pinMv, clear));
      TEST_ASSERT_EQUAL(ok, finite);
    }
  }
}

void test_cal_median_of_even_window_is_mean_of_middle_pair(void)
{
  // D-01: the capture decision is the median of the 20-sample window. 20 is even, so the median is the mean
  // of the 10th and 11th sorted values; an odd count takes the middle one.
  const float ramp[20] = {19, 3, 11, 0, 7, 15, 1, 18, 5, 13, 9, 17, 2, 14, 6, 10, 4, 16, 8, 12};
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 9.5f, turbidity::cal::medianOf(ramp, 20));
  const float odd[5] = {50, 10, 40, 20, 30};
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 30.0f, turbidity::cal::medianOf(odd, 5));
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 3257.9f, turbidity::cal::medianOf(RUN01_CLEAR, 20));
}

void test_cal_median_refuses_empty_oversized_or_nan_input(void)
{
  // D-01 / T-05-07: the median copies into a 20-entry local buffer, so a larger count must be refused rather
  // than written past it; an empty or NAN-bearing window has no honest median.
  float big[21];
  for (size_t i = 0; i < 21; i++)
  {
    big[i] = 3200.0f;
  }
  TEST_ASSERT_FLOAT_IS_NAN(turbidity::cal::medianOf(nullptr, 20));
  TEST_ASSERT_FLOAT_IS_NAN(turbidity::cal::medianOf(big, 0));
  TEST_ASSERT_FLOAT_IS_NAN(turbidity::cal::medianOf(big, 21));
  float withNan[3] = {3200.0f, NAN, 3210.0f};
  TEST_ASSERT_FLOAT_IS_NAN(turbidity::cal::medianOf(withNan, 3));
}

void test_cal_median_does_not_reorder_the_callers_window(void)
{
  // T-05-07: the portal passes its live ring buffer; sorting it in place would scramble the order the next
  // sample is written into and the live state would be judged on a shuffled window.
  float window[20];
  for (size_t i = 0; i < 20; i++)
  {
    window[i] = RUN01_CLEAR[i];
  }
  (void)turbidity::cal::medianOf(window, 20);
  for (size_t i = 0; i < 20; i++)
  {
    TEST_ASSERT_EQUAL_FLOAT(RUN01_CLEAR[i], window[i]);
  }
}

void test_cal_spread_is_max_minus_min(void)
{
  // D-03: the stability measure is max minus min of the window, sensor-side mV.
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 133.2f, turbidity::cal::spreadOf(RUN01_CLEAR, 20));
  const float two[2] = {3200.0f, 3350.0f};
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 150.0f, turbidity::cal::spreadOf(two, 2));
  TEST_ASSERT_FLOAT_IS_NAN(turbidity::cal::spreadOf(nullptr, 20));
  TEST_ASSERT_FLOAT_IS_NAN(turbidity::cal::spreadOf(two, 0));
  const float withNan[2] = {3200.0f, NAN};
  TEST_ASSERT_FLOAT_IS_NAN(turbidity::cal::spreadOf(withNan, 2));
}

void test_cal_capture_wrong_count_or_nan_is_signal_lost(void)
{
  // D-01 / T-05-06: fail closed. A window that is not exactly 20 samples is not a capture, and a NAN means
  // the signal is gone — which wins even over a 900 mV swing that would otherwise read as Unstable.
  float window[20];
  fillWindow(window, 3200.0f);
  TEST_ASSERT_TRUE(isOutcome(nullptr, 20, turbidity::cal::CaptureOutcome::SignalLost));
  TEST_ASSERT_TRUE(isOutcome(window, 19, turbidity::cal::CaptureOutcome::SignalLost));
  TEST_ASSERT_EQUAL_UINT16(0, turbidity::cal::evaluateCapture(window, 19).medianMv);

  window[0] = NAN;
  window[5] = 3200.0f - 450.0f;
  window[6] = 3200.0f + 450.0f;
  TEST_ASSERT_TRUE(isOutcome(window, 20, turbidity::cal::CaptureOutcome::SignalLost));
  TEST_ASSERT_EQUAL_UINT16(0, turbidity::cal::evaluateCapture(window, 20).medianMv);
}

void test_cal_capture_spread_boundary_is_inclusive_at_150(void)
{
  // D-03: 150.0 mV exactly is accepted, 150.1 is refused as unstable.
  float window[20];
  fillWindow(window, 3200.0f);
  window[19] = 3350.0f;
  const turbidity::cal::CaptureResult atLimit = turbidity::cal::evaluateCapture(window, 20);
  TEST_ASSERT_TRUE(atLimit.outcome == turbidity::cal::CaptureOutcome::Accepted);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 150.0f, atLimit.spreadMv);

  window[19] = 3350.1f;
  const turbidity::cal::CaptureResult over = turbidity::cal::evaluateCapture(window, 20);
  TEST_ASSERT_TRUE(over.outcome == turbidity::cal::CaptureOutcome::Unstable);
  TEST_ASSERT_TRUE(over.spreadMv > 150.0f);
}

void test_cal_capture_plausibility_window_boundaries(void)
{
  // D-04: the median must sit inside turbidity::isPlausibleClearWaterMv's window (2870-3580 sensor mV).
  float window[20];
  fillWindow(window, 3200.0f);
  const turbidity::cal::CaptureResult flat = turbidity::cal::evaluateCapture(window, 20);
  TEST_ASSERT_TRUE(flat.outcome == turbidity::cal::CaptureOutcome::Accepted);
  TEST_ASSERT_EQUAL_UINT16(3200, flat.medianMv);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, flat.spreadMv);

  fillWindow(window, static_cast<float>(turbidity::CLEAR_WATER_MIN_MV));
  TEST_ASSERT_TRUE(isOutcome(window, 20, turbidity::cal::CaptureOutcome::Accepted));
  TEST_ASSERT_EQUAL_UINT16(turbidity::CLEAR_WATER_MIN_MV, turbidity::cal::evaluateCapture(window, 20).medianMv);
  fillWindow(window, static_cast<float>(turbidity::CLEAR_WATER_MAX_MV));
  TEST_ASSERT_TRUE(isOutcome(window, 20, turbidity::cal::CaptureOutcome::Accepted));

  fillWindow(window, static_cast<float>(turbidity::CLEAR_WATER_MIN_MV - 1));
  const turbidity::cal::CaptureResult low = turbidity::cal::evaluateCapture(window, 20);
  TEST_ASSERT_TRUE(low.outcome == turbidity::cal::CaptureOutcome::Implausible);
  TEST_ASSERT_EQUAL_UINT16(turbidity::CLEAR_WATER_MIN_MV - 1, low.medianMv);
  fillWindow(window, static_cast<float>(turbidity::CLEAR_WATER_MAX_MV + 1));
  TEST_ASSERT_TRUE(isOutcome(window, 20, turbidity::cal::CaptureOutcome::Implausible));

  // Probe in air: the bench read about 2716 mV sensor-side, steady — refused as implausible, not stored.
  fillWindow(window, 2716.0f);
  TEST_ASSERT_TRUE(isOutcome(window, 20, turbidity::cal::CaptureOutcome::Implausible));
}

void test_cal_capture_bench_run01_clear_water_is_accepted(void)
{
  // D-01/D-03/D-04: real clear water with the two-level supply jump is still a good capture.
  const turbidity::cal::CaptureResult r = turbidity::cal::evaluateCapture(RUN01_CLEAR, 20);
  TEST_ASSERT_TRUE(r.outcome == turbidity::cal::CaptureOutcome::Accepted);
  TEST_ASSERT_EQUAL_UINT16(3258, r.medianMv);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 133.2f, r.spreadMv);
}

void test_cal_capture_bench_run12_settling_is_unstable_before_implausible(void)
{
  // D-03 before D-04: this window is also below the plausible window, but a median of a moving window means
  // nothing, so Unstable must be the answer the admin sees ("wait"), not Implausible.
  const turbidity::cal::CaptureResult r = turbidity::cal::evaluateCapture(RUN12_SETTLING, 20);
  TEST_ASSERT_TRUE(r.outcome == turbidity::cal::CaptureOutcome::Unstable);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 858.6f, r.spreadMv);
}

void test_cal_capture_bench_run12_steady_low_is_implausible(void)
{
  // D-04: steady (spread 93 mV) but far below the window — refused, and the median is reported so the portal
  // can tell the admin what it saw.
  const turbidity::cal::CaptureResult r = turbidity::cal::evaluateCapture(RUN12_STEADY_LOW, 20);
  TEST_ASSERT_TRUE(r.outcome == turbidity::cal::CaptureOutcome::Implausible);
  TEST_ASSERT_EQUAL_UINT16(2349, r.medianMv);
  TEST_ASSERT_FLOAT_WITHIN(0.05f, 93.0f, r.spreadMv);
}

void test_cal_capture_bench_run07_one_cap_turbid_is_accepted_known_limit(void)
{
  // KNOWN LIMIT (T-05-09, accepted): one cap of cornstarch stock is steady and inside the plausible window,
  // so it passes both guards and would be stored as "clear water". Pinned so the limit is disclosed, not
  // hidden; the guards cannot tell slightly turbid water from clear water on this rig.
  const turbidity::cal::CaptureResult r = turbidity::cal::evaluateCapture(RUN07_ONE_CAP, 20);
  TEST_ASSERT_TRUE(r.outcome == turbidity::cal::CaptureOutcome::Accepted);
  TEST_ASSERT_EQUAL_UINT16(2976, r.medianMv);
}

void test_cal_live_state_uses_the_capture_spread_rule(void)
{
  // D-06: the live indicator and the capture decision share one rule, so "steady" on screen means a capture
  // taken now would pass the stability check.
  float window[20];
  fillWindow(window, 3200.0f);
  TEST_ASSERT_TRUE(turbidity::cal::liveState(nullptr, 0) == turbidity::cal::LiveState::Settling);
  TEST_ASSERT_TRUE(turbidity::cal::liveState(window, 0) == turbidity::cal::LiveState::Settling);
  TEST_ASSERT_TRUE(turbidity::cal::liveState(window, 10) == turbidity::cal::LiveState::Settling);
  TEST_ASSERT_TRUE(turbidity::cal::liveState(window, 20) == turbidity::cal::LiveState::Steady);
  TEST_ASSERT_TRUE(turbidity::cal::liveState(RUN01_CLEAR, 20) == turbidity::cal::LiveState::Steady);

  // Unstable is reported as soon as the filled part is too wide, even before 20 samples.
  window[3] = 3350.1f;
  TEST_ASSERT_TRUE(turbidity::cal::liveState(window, 5) == turbidity::cal::LiveState::Unstable);
  TEST_ASSERT_TRUE(turbidity::cal::liveState(RUN12_SETTLING, 20) == turbidity::cal::LiveState::Unstable);

  // A NAN in the filled part is signal lost; one outside the filled part is not looked at.
  fillWindow(window, 3200.0f);
  window[2] = NAN;
  TEST_ASSERT_TRUE(turbidity::cal::liveState(window, 3) == turbidity::cal::LiveState::SignalLost);
  TEST_ASSERT_TRUE(turbidity::cal::liveState(window, 2) == turbidity::cal::LiveState::Settling);
}

void test_cal_manual_blank_means_keep_never_zero(void)
{
  // D-09 / T-05-05: a submitted empty field must never store 0 (which reads as "never calibrated").
  const char *blanks[] = {nullptr, "", "   ", "\t", " \t "};
  for (const char *text : blanks)
  {
    const turbidity::cal::ManualResult r = turbidity::cal::parseManualMv(text);
    TEST_ASSERT_TRUE(r.kind == turbidity::cal::ManualKind::Keep);
    TEST_ASSERT_EQUAL_UINT16(0, r.mv);
  }
}

void test_cal_manual_digits_set_after_trim(void)
{
  // D-09: typed digits, with surrounding spaces or tabs trimmed, set the value when inside the window.
  turbidity::cal::ManualResult r = turbidity::cal::parseManualMv("3100");
  TEST_ASSERT_TRUE(r.kind == turbidity::cal::ManualKind::Set);
  TEST_ASSERT_EQUAL_UINT16(3100, r.mv);
  r = turbidity::cal::parseManualMv(" 3100 ");
  TEST_ASSERT_TRUE(r.kind == turbidity::cal::ManualKind::Set);
  TEST_ASSERT_EQUAL_UINT16(3100, r.mv);
  r = turbidity::cal::parseManualMv("\t3100\t");
  TEST_ASSERT_TRUE(r.kind == turbidity::cal::ManualKind::Set);
  TEST_ASSERT_EQUAL_UINT16(3100, r.mv);
  r = turbidity::cal::parseManualMv("2870");
  TEST_ASSERT_TRUE(r.kind == turbidity::cal::ManualKind::Set);
  TEST_ASSERT_EQUAL_UINT16(2870, r.mv);
  r = turbidity::cal::parseManualMv("3580");
  TEST_ASSERT_TRUE(r.kind == turbidity::cal::ManualKind::Set);
  TEST_ASSERT_EQUAL_UINT16(3580, r.mv);
}

void test_cal_manual_rejects_non_digits_signs_decimals_and_long_input(void)
{
  // D-09 / T-05-05: digits only — no sign, no decimal point, no inner space, at most 5 digits.
  const char *bad[] = {"31a", "-5", "+3100", "3.1", "999999", "3 100"};
  for (const char *text : bad)
  {
    TEST_ASSERT_TRUE(turbidity::cal::parseManualMv(text).kind == turbidity::cal::ManualKind::NotANumber);
  }
}

void test_cal_manual_out_of_window_is_implausible(void)
{
  // D-04 applied to D-09: a typed number outside the plausible window is refused, and a 5-digit value above
  // 65535 cannot fit the stored uShort, so it is implausible with mv 0 rather than wrapped.
  const turbidity::cal::ManualResult huge = turbidity::cal::parseManualMv("70000");
  TEST_ASSERT_TRUE(huge.kind == turbidity::cal::ManualKind::Implausible);
  TEST_ASSERT_EQUAL_UINT16(0, huge.mv);
  const char *outside[] = {"0", "2000", "2869", "3581"};
  for (const char *text : outside)
  {
    TEST_ASSERT_TRUE(turbidity::cal::parseManualMv(text).kind == turbidity::cal::ManualKind::Implausible);
  }
}

void test_cal_stamp_unsynced_clock_is_date_unknown(void)
{
  // D-08 / T-05-08: an unsynced clock gives 0 ("date unknown"), never a fake 1970 date; anything that does
  // not fit the stored uint32 is also 0.
  TEST_ASSERT_EQUAL_UINT32(0, turbidity::cal::calibrationStamp(0));
  TEST_ASSERT_EQUAL_UINT32(0, turbidity::cal::calibrationStamp(1700000000LL));
  TEST_ASSERT_EQUAL_UINT32(0, turbidity::cal::calibrationStamp(-5LL));
  TEST_ASSERT_EQUAL_UINT32(0, turbidity::cal::calibrationStamp(4294967296LL));
  TEST_ASSERT_EQUAL_UINT32(1700000001UL, turbidity::cal::calibrationStamp(1700000001LL));
  TEST_ASSERT_EQUAL_UINT32(1759300000UL, turbidity::cal::calibrationStamp(1759300000LL));
  TEST_ASSERT_EQUAL_UINT32(4294967295UL, turbidity::cal::calibrationStamp(4294967295LL));
}

void test_cal_tokens_cover_every_enumerator(void)
{
  // The portal's JSON speaks these tokens; each enumerator has its own, and an out-of-range value falls to a
  // fault token, never to a success one.
  using turbidity::cal::CaptureOutcome;
  using turbidity::cal::LiveState;
  using turbidity::cal::ManualKind;
  TEST_ASSERT_EQUAL_STRING("accepted", turbidity::cal::captureOutcomeToken(CaptureOutcome::Accepted));
  TEST_ASSERT_EQUAL_STRING("signal_lost", turbidity::cal::captureOutcomeToken(CaptureOutcome::SignalLost));
  TEST_ASSERT_EQUAL_STRING("unstable", turbidity::cal::captureOutcomeToken(CaptureOutcome::Unstable));
  TEST_ASSERT_EQUAL_STRING("implausible", turbidity::cal::captureOutcomeToken(CaptureOutcome::Implausible));
  TEST_ASSERT_EQUAL_STRING("signal_lost",
                           turbidity::cal::captureOutcomeToken(static_cast<CaptureOutcome>(99)));

  TEST_ASSERT_EQUAL_STRING("settling", turbidity::cal::liveStateToken(LiveState::Settling));
  TEST_ASSERT_EQUAL_STRING("steady", turbidity::cal::liveStateToken(LiveState::Steady));
  TEST_ASSERT_EQUAL_STRING("unstable", turbidity::cal::liveStateToken(LiveState::Unstable));
  TEST_ASSERT_EQUAL_STRING("signal_lost", turbidity::cal::liveStateToken(LiveState::SignalLost));
  TEST_ASSERT_EQUAL_STRING("signal_lost", turbidity::cal::liveStateToken(static_cast<LiveState>(99)));

  TEST_ASSERT_EQUAL_STRING("keep", turbidity::cal::manualKindToken(ManualKind::Keep));
  TEST_ASSERT_EQUAL_STRING("set", turbidity::cal::manualKindToken(ManualKind::Set));
  TEST_ASSERT_EQUAL_STRING("not_a_number", turbidity::cal::manualKindToken(ManualKind::NotANumber));
  TEST_ASSERT_EQUAL_STRING("implausible", turbidity::cal::manualKindToken(ManualKind::Implausible));
  TEST_ASSERT_EQUAL_STRING("not_a_number", turbidity::cal::manualKindToken(static_cast<ManualKind>(99)));
}

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_trimmed_mean_rejects_one_sided_spikes);
  RUN_TEST(test_trimmed_mean_reports_spread_of_the_kept_window);
  RUN_TEST(test_burst_shorter_than_the_trim_is_nan_not_an_average);
  RUN_TEST(test_divider_scaling_recovers_sensor_voltage);
  RUN_TEST(test_pin_below_fault_floor_is_nan_not_zero_and_not_ceiling);
  RUN_TEST(test_zero_calibration_is_uncalibrated_and_yields_nan);
  RUN_TEST(test_calibration_outside_the_plausible_window_yields_nan);
  RUN_TEST(test_clear_water_reference_reads_zero_after_normalization);
  RUN_TEST(test_at_reference_within_epsilon_still_reads_zero);
  RUN_TEST(test_ten_millivolts_below_reference_is_not_zero);
  RUN_TEST(test_above_clear_water_reference_clamps_to_zero_never_negative);
  RUN_TEST(test_far_above_clear_water_reference_is_nan);
  RUN_TEST(test_below_curve_range_clamps_to_ceiling_not_nan);
  RUN_TEST(test_curve_is_monotonic_decreasing_across_the_valid_range);
  RUN_TEST(test_reported_ntu_is_rounded_to_the_declared_step);
  RUN_TEST(test_classify_dead_pin_is_no_signal);
  RUN_TEST(test_classify_missing_or_implausible_calibration_is_uncalibrated);
  RUN_TEST(test_classify_far_above_reference_is_over_range);
  RUN_TEST(test_classify_usable_readings_are_ok);
  RUN_TEST(test_classify_ok_exactly_when_ntu_is_finite);
  RUN_TEST(test_cal_median_of_even_window_is_mean_of_middle_pair);
  RUN_TEST(test_cal_median_refuses_empty_oversized_or_nan_input);
  RUN_TEST(test_cal_median_does_not_reorder_the_callers_window);
  RUN_TEST(test_cal_spread_is_max_minus_min);
  RUN_TEST(test_cal_capture_wrong_count_or_nan_is_signal_lost);
  RUN_TEST(test_cal_capture_spread_boundary_is_inclusive_at_150);
  RUN_TEST(test_cal_capture_plausibility_window_boundaries);
  RUN_TEST(test_cal_capture_bench_run01_clear_water_is_accepted);
  RUN_TEST(test_cal_capture_bench_run12_settling_is_unstable_before_implausible);
  RUN_TEST(test_cal_capture_bench_run12_steady_low_is_implausible);
  RUN_TEST(test_cal_capture_bench_run07_one_cap_turbid_is_accepted_known_limit);
  RUN_TEST(test_cal_live_state_uses_the_capture_spread_rule);
  RUN_TEST(test_cal_manual_blank_means_keep_never_zero);
  RUN_TEST(test_cal_manual_digits_set_after_trim);
  RUN_TEST(test_cal_manual_rejects_non_digits_signs_decimals_and_long_input);
  RUN_TEST(test_cal_manual_out_of_window_is_implausible);
  RUN_TEST(test_cal_stamp_unsynced_clock_is_date_unknown);
  RUN_TEST(test_cal_tokens_cover_every_enumerator);
  return UNITY_END();
}
