#include <unity.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "PhMath.h"
#include "PhCalibration.h"

// Pins every pH decision in lib/PhMath on the host: the two-point fit, the slope-only Nernst compensation that
// pivots on pH 7, the fault order (no signal, uncalibrated, over range), the shared instability backstop, the
// report window and the rounding. Every fixture is expressed relative to the header's named constants, so plan
// 10-09 can replace the PROVISIONAL numbers from the bench CSVs without rewriting these cases. A case that
// turns red after such a change is a real disagreement with a decision, not a stale literal.
namespace
{
  // DFRobot's nominal 3.5 pH/V, sensor-side, as mV per pH. Only used to build plausible fixtures; the library
  // never assumes it for conversion (the fit is measured).
  constexpr float FIXTURE_SLOPE_MV_PER_PH = 285.7f;

  // Phase 9 measured the zero-input (pH 7) level at about 1.78 V sensor-side.
  constexpr float FIXTURE_MV7 = 1780.0f;

  // A higher isopotential level so that pH 2..12 all land well above the fault floor on the pin. With the
  // measured ~1.78 V level a rising board puts pH 2 near the ADC's dead zone (RESEARCH divider table), which
  // would turn the OverRange edge cases into NoSignal ones and assert the wrong rule.
  constexpr float HIGH_MV7 = 2500.0f;

  // How far inside the plausible window an "on the edge" fixture sits. The pin -> sensor -> pH float chain
  // can land a reading aimed exactly at 2.00 a few ULPs below it; 0.001 pH is about 0.3 mV, far larger than
  // that error, and still rounds to the edge value itself.
  constexpr float EDGE_NUDGE_PH = 0.001f;

  uint16_t mvOnLine(float mv7, float slope, uint16_t bufHundredths)
  {
    const float mv = mv7 + slope * (static_cast<float>(bufHundredths) / 100.0f - 7.0f);
    return static_cast<uint16_t>(std::lround(mv));
  }

  // A calibration whose two points sit on the line mv7 + slope * (pH - 7).
  ph::Calibration calFromLine(float mv7, float slope, uint16_t bufA = 686, uint16_t bufB = 918,
                              int16_t tcalCentiC = 2500)
  {
    ph::Calibration c{};
    c.mvA = mvOnLine(mv7, slope, bufA);
    c.bufA = bufA;
    c.mvB = mvOnLine(mv7, slope, bufB);
    c.bufB = bufB;
    c.tcalCentiC = tcalCentiC;
    c.calAt = 1780000000UL;
    c.version = ph::PH_CAL_VERSION;
    return c;
  }

  float expectedSlope()
  {
    return static_cast<float>(ph::EXPECTED_SLOPE_SIGN) * FIXTURE_SLOPE_MV_PER_PH;
  }

  // A well-formed 6.86 / 9.18 calibration with the expected polarity.
  ph::Calibration goodCal()
  {
    return calFromLine(FIXTURE_MV7, expectedSlope());
  }

  ph::Calibration highCal()
  {
    return calFromLine(HIGH_MV7, expectedSlope());
  }

  // A burst whose filtered (pin-side) value is `pinMv`.
  ph::Burst burstAt(float pinMv, float rawMeanMv = 0.0f, float spreadMv = 10.0f)
  {
    return ph::Burst{rawMeanMv, pinMv, spreadMv};
  }

  // Pin millivolts that read `phValue` at Tcal on the fitted line of `c`.
  float pinMvForPh(const ph::Calibration &c, float phValue)
  {
    const ph::Line l = ph::fit(c);
    return (l.mvAtIsopotential + l.slopeMvPerPh * (phValue - ph::ISOPOTENTIAL_PH)) * ph::DIVIDER_RATIO;
  }
}

// ---- Rounding ----

void test_round_ph_keeps_two_decimals_and_nan(void)
{
  TEST_ASSERT_TRUE(ph::roundPh(7.1049f) == 7.10f);
  TEST_ASSERT_TRUE(ph::roundPh(9.456f) == 9.46f);
  TEST_ASSERT_TRUE(std::isnan(ph::roundPh(NAN)));
}

// ---- Burst summary ----

void test_trimmed_mean_rejects_one_sided_spikes(void)
{
  uint16_t samples[ph::BURST_SAMPLES];
  for (size_t i = 0; i < ph::BURST_SAMPLES; i++)
  {
    samples[i] = 1000;
  }
  for (size_t i = 0; i < ph::TRIM_EACH_END; i++)
  {
    samples[i] = 3000;
  }
  const ph::Burst b = ph::summarizeBurst(samples, ph::BURST_SAMPLES, ph::TRIM_EACH_END);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1000.0f, b.filteredMv);
  TEST_ASSERT_TRUE(b.rawMeanMv > 1000.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, b.spreadMv);
}

void test_burst_shorter_than_the_trim_is_nan_not_an_average(void)
{
  uint16_t samples[4] = {1000, 1000, 1000, 1000};
  const ph::Burst b = ph::summarizeBurst(samples, 4, 2);
  TEST_ASSERT_TRUE(std::isnan(b.filteredMv));
  TEST_ASSERT_TRUE(std::isnan(b.rawMeanMv));
  TEST_ASSERT_TRUE(std::isnan(b.spreadMv));
  TEST_ASSERT_TRUE(std::isnan(ph::summarizeBurst(nullptr, 64, 16).filteredMv));
}

// ---- Divider ----

void test_divider_ratio_includes_the_pull_down_in_parallel(void)
{
  const float bottom = (12000.0f * 1000000.0f) / (12000.0f + 1000000.0f);
  TEST_ASSERT_FLOAT_WITHIN(1e-5f, bottom / (10000.0f + bottom), ph::DIVIDER_RATIO);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.5425f, ph::DIVIDER_RATIO);
  TEST_ASSERT_FLOAT_WITHIN(0.5f, 1000.0f, ph::sensorMvFromPinMv(542.5f));
}

// ---- Fit ----

void test_fit_reproduces_both_buffers_at_tcal_rising_board(void)
{
  const ph::Calibration c = goodCal();
  const ph::Line l = ph::fit(c);
  TEST_ASSERT_FLOAT_WITHIN(0.005f, c.bufA / 100.0f, ph::phFromSensorMv(c.mvA, l, 25.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.005f, c.bufB / 100.0f, ph::phFromSensorMv(c.mvB, l, 25.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 25.0f, l.tcalC);
}

void test_fit_reproduces_both_buffers_at_tcal_falling_board(void)
{
  // The fit itself is polarity-agnostic; only isValidCalibration enforces the expected sign.
  const ph::Calibration c = calFromLine(FIXTURE_MV7, -expectedSlope(), 401, 918, 2750);
  const ph::Line l = ph::fit(c);
  TEST_ASSERT_TRUE((l.slopeMvPerPh > 0.0f) != (expectedSlope() > 0.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.005f, 4.01f, ph::phFromSensorMv(c.mvA, l, 27.5f));
  TEST_ASSERT_FLOAT_WITHIN(0.005f, 9.18f, ph::phFromSensorMv(c.mvB, l, 27.5f));
}

// ---- Compensation (D-03 / SENS-11) ----

void test_compensation_pivots_on_ph7_with_nernst_slope(void)
{
  const ph::Line l = ph::fit(goodCal());
  const float mv950 = l.mvAtIsopotential + l.slopeMvPerPh * (9.50f - 7.0f);
  const float mv401 = l.mvAtIsopotential + l.slopeMvPerPh * (4.01f - 7.0f);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 9.50f, ph::phFromSensorMv(mv950, l, 25.0f));
  TEST_ASSERT_TRUE(ph::roundPh(ph::phFromSensorMv(mv950, l, 30.0f)) == 9.46f);
  TEST_ASSERT_TRUE(ph::roundPh(ph::phFromSensorMv(mv401, l, 30.0f)) == 4.06f);
}

void test_isopotential_point_reads_seven_at_any_temperature(void)
{
  const ph::Line l = ph::fit(goodCal());
  TEST_ASSERT_TRUE(ph::roundPh(ph::phFromSensorMv(l.mvAtIsopotential, l, 10.0f)) == 7.00f);
  TEST_ASSERT_TRUE(ph::roundPh(ph::phFromSensorMv(l.mvAtIsopotential, l, 25.0f)) == 7.00f);
  TEST_ASSERT_TRUE(ph::roundPh(ph::phFromSensorMv(l.mvAtIsopotential, l, 40.0f)) == 7.00f);
}

void test_nan_temperature_falls_back_to_tcal_never_nan(void)
{
  const ph::Line l = ph::fit(goodCal());
  const float mv = l.mvAtIsopotential + l.slopeMvPerPh * 2.5f;
  const float atNan = ph::phFromSensorMv(mv, l, NAN);
  TEST_ASSERT_FALSE(std::isnan(atNan));
  TEST_ASSERT_TRUE(atNan == ph::phFromSensorMv(mv, l, l.tcalC));
}

// ---- Calibration validity (SENS-09) ----

void test_well_formed_calibration_is_valid(void)
{
  TEST_ASSERT_TRUE(ph::isValidCalibration(goodCal()));
  TEST_ASSERT_TRUE(ph::isValidCalibration(highCal()));
}

void test_zeroed_or_wrong_version_calibration_is_invalid(void)
{
  const ph::Calibration zero{};
  TEST_ASSERT_FALSE(ph::isValidCalibration(zero));

  ph::Calibration c = goodCal();
  c.version = ph::PH_CAL_VERSION + 1;
  TEST_ASSERT_FALSE(ph::isValidCalibration(c));
  c.version = 0;
  TEST_ASSERT_FALSE(ph::isValidCalibration(c));
}

void test_too_close_or_equal_mv_calibration_is_invalid(void)
{
  const uint16_t closeB = static_cast<uint16_t>(686 + ph::MIN_BUFFER_GAP_HUNDREDTHS - 1);
  TEST_ASSERT_FALSE(ph::isValidCalibration(calFromLine(FIXTURE_MV7, expectedSlope(), 686, closeB)));
  const uint16_t edgeB = static_cast<uint16_t>(686 + ph::MIN_BUFFER_GAP_HUNDREDTHS);
  TEST_ASSERT_TRUE(ph::isValidCalibration(calFromLine(FIXTURE_MV7, expectedSlope(), 686, edgeB)));

  ph::Calibration c = goodCal();
  c.mvB = c.mvA;
  TEST_ASSERT_FALSE(ph::isValidCalibration(c));
}

void test_out_of_range_fields_are_invalid(void)
{
  ph::Calibration c = goodCal();
  c.mvA = 0;
  TEST_ASSERT_FALSE(ph::isValidCalibration(c));
  c = goodCal();
  c.mvB = 0;
  TEST_ASSERT_FALSE(ph::isValidCalibration(c));
  c = goodCal();
  c.mvB = static_cast<uint16_t>(ph::MAX_SENSOR_MV + 1);
  TEST_ASSERT_FALSE(ph::isValidCalibration(c));
  c = goodCal();
  c.mvA = static_cast<uint16_t>(ph::MAX_SENSOR_MV + 1);
  TEST_ASSERT_FALSE(ph::isValidCalibration(c));

  c = goodCal();
  c.bufA = 0;
  TEST_ASSERT_FALSE(ph::isValidCalibration(c));
  c = goodCal();
  c.bufB = 1400;
  TEST_ASSERT_FALSE(ph::isValidCalibration(c));

  c = goodCal();
  c.tcalCentiC = -1;
  TEST_ASSERT_FALSE(ph::isValidCalibration(c));
  c = goodCal();
  c.tcalCentiC = static_cast<int16_t>(ph::MAX_TCAL_CENTI_C + 1);
  TEST_ASSERT_FALSE(ph::isValidCalibration(c));
}

void test_wrong_slope_sign_calibration_is_invalid(void)
{
  TEST_ASSERT_FALSE(ph::isValidCalibration(calFromLine(FIXTURE_MV7, -expectedSlope())));
  ph::Calibration swapped = goodCal();
  const uint16_t mv = swapped.mvA;
  swapped.mvA = swapped.mvB;
  swapped.mvB = mv;
  TEST_ASSERT_FALSE(ph::isValidCalibration(swapped));
}

// ---- Classify (single home of every fault decision) ----

void test_classify_no_signal_wins_over_everything(void)
{
  const ph::Calibration zero{};
  TEST_ASSERT_TRUE(ph::classify(burstAt(NAN), zero, 25.0f) == ph::Status::NoSignal);
  TEST_ASSERT_TRUE(ph::classify(burstAt(NAN), goodCal(), 25.0f) == ph::Status::NoSignal);
  TEST_ASSERT_TRUE(ph::classify(burstAt(ph::FAULT_FLOOR_PIN_MV - 0.1f), goodCal(), 25.0f) ==
                   ph::Status::NoSignal);
  TEST_ASSERT_TRUE(ph::classify(burstAt(ph::FAULT_FLOOR_PIN_MV), goodCal(), 25.0f) != ph::Status::NoSignal);
}

void test_classify_unstable_burst_is_no_signal(void)
{
  const float pin7 = pinMvForPh(goodCal(), 7.0f);
  TEST_ASSERT_TRUE(ph::classify(burstAt(pin7, 0.0f, ph::MAX_BURST_SPREAD_PIN_MV + 0.1f), goodCal(), 25.0f) ==
                   ph::Status::NoSignal);
  TEST_ASSERT_TRUE(ph::classify(burstAt(pin7, 0.0f, ph::MAX_BURST_SPREAD_PIN_MV), goodCal(), 25.0f) ==
                   ph::Status::Ok);
}

void test_classify_good_burst_with_invalid_calibration_is_uncalibrated(void)
{
  const ph::Calibration zero{};
  const float pin7 = pinMvForPh(goodCal(), 7.0f);
  TEST_ASSERT_TRUE(ph::classify(burstAt(pin7), zero, 25.0f) == ph::Status::Uncalibrated);
  TEST_ASSERT_TRUE(ph::classify(burstAt(pin7), calFromLine(FIXTURE_MV7, -expectedSlope()), 25.0f) ==
                   ph::Status::Uncalibrated);
  TEST_ASSERT_TRUE(std::isnan(ph::phFromBurst(burstAt(pin7), zero, 25.0f)));
}

void test_classify_plausible_window_edges(void)
{
  const ph::Calibration c = highCal();
  // Fixture guard: every pin value below must sit above the fault floor, or this case would be asserting the
  // NoSignal rule instead of the plausible-window one.
  TEST_ASSERT_TRUE(pinMvForPh(c, 1.99f) > ph::FAULT_FLOOR_PIN_MV);
  TEST_ASSERT_TRUE(pinMvForPh(c, 12.01f) > ph::FAULT_FLOOR_PIN_MV);

  TEST_ASSERT_TRUE(ph::classify(burstAt(pinMvForPh(c, 1.99f)), c, 25.0f) == ph::Status::OverRange);
  TEST_ASSERT_TRUE(ph::classify(burstAt(pinMvForPh(c, ph::PLAUSIBLE_MIN_PH + EDGE_NUDGE_PH)), c, 25.0f) ==
                   ph::Status::Ok);
  TEST_ASSERT_TRUE(ph::classify(burstAt(pinMvForPh(c, ph::PLAUSIBLE_MAX_PH - EDGE_NUDGE_PH)), c, 25.0f) ==
                   ph::Status::Ok);
  TEST_ASSERT_TRUE(ph::classify(burstAt(pinMvForPh(c, 12.01f)), c, 25.0f) == ph::Status::OverRange);

  TEST_ASSERT_TRUE(ph::phFromBurst(burstAt(pinMvForPh(c, ph::PLAUSIBLE_MIN_PH + EDGE_NUDGE_PH)), c, 25.0f) ==
                   2.00f);
  TEST_ASSERT_TRUE(ph::phFromBurst(burstAt(pinMvForPh(c, ph::PLAUSIBLE_MAX_PH - EDGE_NUDGE_PH)), c, 25.0f) ==
                   12.00f);
  // Never a clamped edge: out of the window is NAN, not 2.00 / 12.00 / 0 / 14.
  TEST_ASSERT_TRUE(std::isnan(ph::phFromBurst(burstAt(pinMvForPh(c, 1.99f)), c, 25.0f)));
  TEST_ASSERT_TRUE(std::isnan(ph::phFromBurst(burstAt(pinMvForPh(c, 12.01f)), c, 25.0f)));
}

void test_classify_non_finite_computed_ph_is_over_range(void)
{
  // Absolute zero collapses the compensated slope to 0, so the computed pH is infinite.
  const ph::Calibration c = highCal();
  const ph::Burst b = burstAt(pinMvForPh(c, 9.0f));
  TEST_ASSERT_TRUE(ph::classify(b, c, -ph::KELVIN_OFFSET) == ph::Status::OverRange);
  TEST_ASSERT_TRUE(std::isnan(ph::phFromBurst(b, c, -ph::KELVIN_OFFSET)));
}

void test_ph_from_burst_is_rounded_and_temperature_compensated(void)
{
  const ph::Calibration c = highCal();
  const ph::Burst b = burstAt(pinMvForPh(c, 9.50f));
  TEST_ASSERT_TRUE(ph::phFromBurst(b, c, 25.0f) == 9.50f);
  TEST_ASSERT_TRUE(ph::phFromBurst(b, c, 30.0f) == 9.46f);
  TEST_ASSERT_TRUE(ph::phFromBurst(b, c, NAN) == 9.50f);
}

void test_classify_ok_exactly_when_ph_is_finite(void)
{
  const ph::Calibration zero{};
  ph::Calibration wrongVersion = goodCal();
  wrongVersion.version = 0;
  const ph::Calibration cals[] = {goodCal(), highCal(), calFromLine(FIXTURE_MV7, -expectedSlope()), zero,
                                  wrongVersion, calFromLine(HIGH_MV7, expectedSlope(), 401, 686, 3000)};
  const float temps[] = {NAN, 25.0f, 30.0f};
  const float spreads[] = {10.0f, ph::MAX_BURST_SPREAD_PIN_MV + 1.0f};
  size_t okCount = 0;
  for (size_t ci = 0; ci < sizeof(cals) / sizeof(cals[0]); ci++)
  {
    for (size_t ti = 0; ti < sizeof(temps) / sizeof(temps[0]); ti++)
    {
      for (size_t si = 0; si < sizeof(spreads) / sizeof(spreads[0]); si++)
      {
        for (int pin = 0; pin <= 2500; pin += 5)
        {
          const ph::Burst b = burstAt(static_cast<float>(pin), 0.0f, spreads[si]);
          const bool ok = ph::classify(b, cals[ci], temps[ti]) == ph::Status::Ok;
          const bool finite = std::isfinite(ph::phFromBurst(b, cals[ci], temps[ti]));
          TEST_ASSERT_EQUAL(ok, finite);
          if (ok)
          {
            okCount++;
          }
        }
      }
    }
  }
  // The sweep must actually visit Ok readings, or the equivalence would hold vacuously.
  TEST_ASSERT_TRUE(okCount > 0);
}

// ---- Report window (D-15) and the shared backstop (D-12, Pitfall 5) ----

void test_signal_burst_test_matches_classify(void)
{
  TEST_ASSERT_FALSE(ph::isSignalBurst(burstAt(NAN)));
  TEST_ASSERT_FALSE(ph::isSignalBurst(burstAt(ph::FAULT_FLOOR_PIN_MV - 0.1f)));
  TEST_ASSERT_TRUE(ph::isSignalBurst(burstAt(ph::FAULT_FLOOR_PIN_MV)));
  TEST_ASSERT_FALSE(ph::isSignalBurst(burstAt(1000.0f, 0.0f, ph::MAX_BURST_SPREAD_PIN_MV + 0.1f)));
  TEST_ASSERT_TRUE(ph::isSignalBurst(burstAt(1000.0f, 0.0f, ph::MAX_BURST_SPREAD_PIN_MV)));

  // classify() step 1 is the same test: every burst the window rejects, classify() calls NoSignal.
  const ph::Burst unstable = burstAt(pinMvForPh(goodCal(), 7.0f), 0.0f, ph::MAX_BURST_SPREAD_PIN_MV + 0.1f);
  TEST_ASSERT_FALSE(ph::isSignalBurst(unstable));
  TEST_ASSERT_TRUE(ph::classify(unstable, goodCal(), 25.0f) == ph::Status::NoSignal);
}

void test_majority_unstable_window_reports_no_value(void)
{
  const ph::Calibration c = goodCal();
  const float pin = pinMvForPh(c, 7.5f);
  const float unstable = ph::MAX_BURST_SPREAD_PIN_MV + 50.0f;
  ph::BurstWindow w{};
  ph::pushBurst(w, burstAt(pin, 1.0f));
  ph::pushBurst(w, burstAt(pin, 2.0f, unstable));
  ph::pushBurst(w, burstAt(pin, 3.0f));
  ph::pushBurst(w, burstAt(pin, 4.0f, unstable));
  ph::pushBurst(w, burstAt(pin, 5.0f, unstable));
  const ph::Burst out = ph::reportBurst(w);
  // Filtered value is above the floor: only the shared backstop in classify() keeps it from reading Ok.
  TEST_ASSERT_TRUE(out.filteredMv >= ph::FAULT_FLOOR_PIN_MV);
  TEST_ASSERT_TRUE(ph::classify(out, c, 25.0f) == ph::Status::NoSignal);
  TEST_ASSERT_TRUE(std::isnan(ph::phFromBurst(out, c, 25.0f)));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, out.rawMeanMv);
}

void test_window_three_of_five_good_reports_the_median_good_burst(void)
{
  ph::BurstWindow w{};
  ph::pushBurst(w, burstAt(900.0f, 1.0f));
  ph::pushBurst(w, burstAt(NAN, NAN, NAN));
  ph::pushBurst(w, burstAt(1000.0f, 3.0f));
  ph::pushBurst(w, burstAt(NAN, NAN, NAN));
  ph::pushBurst(w, burstAt(950.0f, 5.0f));
  const ph::Burst out = ph::reportBurst(w);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 950.0f, out.filteredMv);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, out.rawMeanMv);
}

void test_window_warm_up_and_empty_window(void)
{
  ph::BurstWindow w{};
  TEST_ASSERT_EQUAL(ph::MIN_REPORT_BURSTS, ph::topUpNeeded(w));
  TEST_ASSERT_TRUE(std::isnan(ph::reportBurst(w).filteredMv));
  ph::pushBurst(w, burstAt(900.0f));
  TEST_ASSERT_EQUAL(ph::MIN_REPORT_BURSTS - 1, ph::topUpNeeded(w));
  for (size_t i = 0; i < ph::REPORT_BURSTS + 2; i++)
  {
    ph::pushBurst(w, burstAt(900.0f));
  }
  TEST_ASSERT_EQUAL(ph::REPORT_BURSTS, w.count);
  TEST_ASSERT_EQUAL(0, ph::topUpNeeded(w));
}

// ---- Calibration helpers (PhCalibration.h) ----

void test_buffer_table_exact_rows(void)
{
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 6.86f, ph::cal::bufferPhAt(686, 25.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 9.23f, ph::cal::bufferPhAt(918, 20.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 9.14f, ph::cal::bufferPhAt(918, 30.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.02f, ph::cal::bufferPhAt(401, 35.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.01f, ph::cal::bufferPhAt(401, 25.0f));
}

void test_buffer_table_interpolates_between_rows(void)
{
  // D-11: 9.18 at 27.5 C sits halfway between 9.18 (25 C) and 9.14 (30 C).
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 9.16f, ph::cal::bufferPhAt(918, 27.5f));
}

void test_buffer_table_refuses_outside_range_or_unknown_buffer(void)
{
  TEST_ASSERT_TRUE(std::isnan(ph::cal::bufferPhAt(918, 19.9f)));
  TEST_ASSERT_TRUE(std::isnan(ph::cal::bufferPhAt(918, 35.1f)));
  TEST_ASSERT_TRUE(std::isnan(ph::cal::bufferPhAt(918, NAN)));
  TEST_ASSERT_TRUE(std::isnan(ph::cal::bufferPhAt(700, 25.0f)));
}

void test_check_calibration_reports_the_reason(void)
{
  using ph::cal::CalCheck;
  TEST_ASSERT_TRUE(ph::cal::checkCalibration(goodCal()) == CalCheck::Ok);
  TEST_ASSERT_TRUE(ph::cal::checkCalibration(calFromLine(FIXTURE_MV7, -expectedSlope())) == CalCheck::WrongSign);
  const uint16_t closeB = static_cast<uint16_t>(686 + ph::MIN_BUFFER_GAP_HUNDREDTHS - 1);
  TEST_ASSERT_TRUE(ph::cal::checkCalibration(calFromLine(FIXTURE_MV7, expectedSlope(), 686, closeB)) ==
                   CalCheck::BuffersTooClose);
  const ph::Calibration zero{};
  TEST_ASSERT_TRUE(ph::cal::checkCalibration(zero) == CalCheck::Invalid);
  ph::Calibration hot = goodCal();
  hot.tcalCentiC = static_cast<int16_t>(ph::MAX_TCAL_CENTI_C + 1);
  TEST_ASSERT_TRUE(ph::cal::checkCalibration(hot) == CalCheck::Invalid);
  ph::Calibration equalMv = goodCal();
  equalMv.mvB = equalMv.mvA;
  TEST_ASSERT_TRUE(ph::cal::checkCalibration(equalMv) == CalCheck::Invalid);
}

void test_slope_percent_of_nominal_scales_with_tcal(void)
{
  const float nominal = ph::cal::NOMINAL_SENSOR_SLOPE_MV_PER_PH;
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 100.0f, ph::cal::slopePercentOfNominal(ph::Line{nominal, 1780.0f, 25.0f}));
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 100.0f, ph::cal::slopePercentOfNominal(ph::Line{-nominal, 1780.0f, 25.0f}));
  const float hot = nominal * (308.15f / 298.15f);
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 100.0f, ph::cal::slopePercentOfNominal(ph::Line{hot, 1780.0f, 35.0f}));
  TEST_ASSERT_FLOAT_WITHIN(0.1f, 90.0f, ph::cal::slopePercentOfNominal(ph::Line{0.9f * nominal, 1780.0f, 25.0f}));
}

void test_offset_ph_is_zero_at_isopotential_and_scaled_otherwise(void)
{
  const ph::Line l{285.7f, 1780.0f, 25.0f};
  TEST_ASSERT_FLOAT_WITHIN(1e-6f, 0.0f, ph::cal::offsetPh(l, 1780.0f));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, (1850.0f - 1780.0f) / 285.7f, ph::cal::offsetPh(l, 1850.0f));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, (1700.0f - 1780.0f) / 285.7f, ph::cal::offsetPh(l, 1700.0f));
}

void test_cal_median_and_spread_over_a_capture_window(void)
{
  float odd[ph::cal::CAPTURE_WINDOW_SAMPLES - 1];
  for (size_t i = 0; i < ph::cal::CAPTURE_WINDOW_SAMPLES - 1; i++)
  {
    odd[i] = static_cast<float>(1000 + ((i * 7) % 19));
  }
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1009.0f, ph::cal::medianOf(odd, ph::cal::CAPTURE_WINDOW_SAMPLES - 1));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 18.0f, ph::cal::spreadOf(odd, ph::cal::CAPTURE_WINDOW_SAMPLES - 1));
  // Not reordered in place.
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 1007.0f, odd[1]);

  float even[ph::cal::CAPTURE_WINDOW_SAMPLES];
  for (size_t i = 0; i < ph::cal::CAPTURE_WINDOW_SAMPLES; i++)
  {
    even[i] = static_cast<float>(2000 + i);
  }
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 2009.5f, ph::cal::medianOf(even, ph::cal::CAPTURE_WINDOW_SAMPLES));
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 19.0f, ph::cal::spreadOf(even, ph::cal::CAPTURE_WINDOW_SAMPLES));

  TEST_ASSERT_TRUE(std::isnan(ph::cal::medianOf(even, 0)));
  TEST_ASSERT_TRUE(std::isnan(ph::cal::medianOf(even, ph::cal::CAPTURE_WINDOW_SAMPLES + 1)));
  TEST_ASSERT_TRUE(std::isnan(ph::cal::spreadOf(nullptr, 3)));
  even[4] = NAN;
  TEST_ASSERT_TRUE(std::isnan(ph::cal::medianOf(even, ph::cal::CAPTURE_WINDOW_SAMPLES)));
  TEST_ASSERT_TRUE(std::isnan(ph::cal::spreadOf(even, ph::cal::CAPTURE_WINDOW_SAMPLES)));
}

void test_cal_check_tokens_cover_every_enumerator(void)
{
  using ph::cal::CalCheck;
  TEST_ASSERT_EQUAL_STRING("ok", ph::cal::calCheckToken(CalCheck::Ok));
  TEST_ASSERT_EQUAL_STRING("wrong_sign", ph::cal::calCheckToken(CalCheck::WrongSign));
  TEST_ASSERT_EQUAL_STRING("buffers_too_close", ph::cal::calCheckToken(CalCheck::BuffersTooClose));
  TEST_ASSERT_EQUAL_STRING("invalid", ph::cal::calCheckToken(CalCheck::Invalid));
  TEST_ASSERT_EQUAL_STRING("invalid", ph::cal::calCheckToken(static_cast<CalCheck>(99)));
}

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_round_ph_keeps_two_decimals_and_nan);
  RUN_TEST(test_trimmed_mean_rejects_one_sided_spikes);
  RUN_TEST(test_burst_shorter_than_the_trim_is_nan_not_an_average);
  RUN_TEST(test_divider_ratio_includes_the_pull_down_in_parallel);
  RUN_TEST(test_fit_reproduces_both_buffers_at_tcal_rising_board);
  RUN_TEST(test_fit_reproduces_both_buffers_at_tcal_falling_board);
  RUN_TEST(test_compensation_pivots_on_ph7_with_nernst_slope);
  RUN_TEST(test_isopotential_point_reads_seven_at_any_temperature);
  RUN_TEST(test_nan_temperature_falls_back_to_tcal_never_nan);
  RUN_TEST(test_well_formed_calibration_is_valid);
  RUN_TEST(test_zeroed_or_wrong_version_calibration_is_invalid);
  RUN_TEST(test_too_close_or_equal_mv_calibration_is_invalid);
  RUN_TEST(test_out_of_range_fields_are_invalid);
  RUN_TEST(test_wrong_slope_sign_calibration_is_invalid);
  RUN_TEST(test_classify_no_signal_wins_over_everything);
  RUN_TEST(test_classify_unstable_burst_is_no_signal);
  RUN_TEST(test_classify_good_burst_with_invalid_calibration_is_uncalibrated);
  RUN_TEST(test_classify_plausible_window_edges);
  RUN_TEST(test_classify_non_finite_computed_ph_is_over_range);
  RUN_TEST(test_ph_from_burst_is_rounded_and_temperature_compensated);
  RUN_TEST(test_classify_ok_exactly_when_ph_is_finite);
  RUN_TEST(test_signal_burst_test_matches_classify);
  RUN_TEST(test_majority_unstable_window_reports_no_value);
  RUN_TEST(test_window_three_of_five_good_reports_the_median_good_burst);
  RUN_TEST(test_window_warm_up_and_empty_window);
  RUN_TEST(test_buffer_table_exact_rows);
  RUN_TEST(test_buffer_table_interpolates_between_rows);
  RUN_TEST(test_buffer_table_refuses_outside_range_or_unknown_buffer);
  RUN_TEST(test_check_calibration_reports_the_reason);
  RUN_TEST(test_slope_percent_of_nominal_scales_with_tcal);
  RUN_TEST(test_offset_ph_is_zero_at_isopotential_and_scaled_otherwise);
  RUN_TEST(test_cal_median_and_spread_over_a_capture_window);
  RUN_TEST(test_cal_check_tokens_cover_every_enumerator);
  return UNITY_END();
}
