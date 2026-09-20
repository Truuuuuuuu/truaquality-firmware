#include <unity.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "TurbidityMath.h"

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
  constexpr uint16_t CLEAR_MV = 4100;

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
  return UNITY_END();
}
