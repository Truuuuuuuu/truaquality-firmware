#include <unity.h>

#include <cmath>
#include <cstddef>
#include <cstdint>

#include "PhMath.h"
#include "PhCalibration.h"
#include "PhAcceptance.h"

// Pins every decision in lib/PhMath/PhAcceptance.h on the host: the picker and slot parsers, the pair rules (gap
// on printed nominals, sign, useless slope, offset window), the held-pair bookkeeping, the Save check, the
// probe-health verdict and the calibration age. Every fixture is expressed relative to the header's named
// constants, so the D-05 follow-up can swap the PROVISIONAL values from the bench record without rewriting these
// cases. A case that turns red after such a change is a real disagreement with a decision, not a stale literal.
namespace
{
  using ph::accept::Age;
  using ph::accept::HeldPair;
  using ph::accept::PairResult;
  using ph::accept::Point;
  using ph::accept::Refusal;
  using ph::accept::Slot;
  using ph::accept::Verdict;

  // A comfortably healthy slope for fixtures that are not about the slope (well above MIN_SLOPE_MV_PER_PH).
  constexpr float FIXTURE_SLOPE_MV_PER_PH = 50.0f;

  // A synced clock well past MIN_VALID_EPOCH.
  constexpr uint32_t FIXTURE_CAL_AT = 1780000000UL;

  Point heldPoint(uint16_t nominal, uint16_t mv, uint16_t buf, float tempC = 25.0f)
  {
    Point p{};
    p.held = true;
    p.nominal = nominal;
    p.mv = mv;
    p.bufHundredths = buf;
    p.tempC = tempC;
    return p;
  }

  uint16_t mvOnLine(float mv7, float slope, uint16_t bufHundredths)
  {
    const float mv = mv7 + slope * (static_cast<float>(bufHundredths) / 100.0f - 7.0f);
    return static_cast<uint16_t>(std::lround(mv));
  }

  // A point whose buffer pH equals its printed nominal, on the healthy fixture line through the zero-input level.
  Point onLine(uint16_t nominal)
  {
    return heldPoint(nominal, mvOnLine(ph::accept::BOARD_ZERO_INPUT_SENSOR_MV, FIXTURE_SLOPE_MV_PER_PH, nominal),
                     nominal);
  }

  // Two points with mv7 exactly at `mv7`: point A sits at pH 7.00, so the fit's pH-7 level is A's mV itself.
  PairResult pairWithMv7(float mv7)
  {
    const uint16_t mvA = static_cast<uint16_t>(std::lround(mv7));
    const Point a = heldPoint(700, mvA, 700);
    const Point b = heldPoint(918, mvOnLine(mv7, FIXTURE_SLOPE_MV_PER_PH, 918), 918);
    return ph::accept::evaluatePair(a, b);
  }
}

void setUp(void) {}
void tearDown(void) {}

void test_parse_nominal_accepts_exactly_the_picker_strings(void)
{
  TEST_ASSERT_EQUAL_UINT16(400, ph::accept::parseNominal("400"));
  TEST_ASSERT_EQUAL_UINT16(401, ph::accept::parseNominal("401"));
  TEST_ASSERT_EQUAL_UINT16(686, ph::accept::parseNominal("686"));
  TEST_ASSERT_EQUAL_UINT16(700, ph::accept::parseNominal("700"));
  TEST_ASSERT_EQUAL_UINT16(918, ph::accept::parseNominal("918"));
  TEST_ASSERT_EQUAL_UINT16(1001, ph::accept::parseNominal("1001"));
}

void test_parse_nominal_refuses_junk(void)
{
  const char *junk[] = {"", "0686", " 686", "686 ", "6.86", "500", "10010", "-686"};
  for (size_t i = 0; i < sizeof(junk) / sizeof(junk[0]); i++)
  {
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0, ph::accept::parseNominal(junk[i]), junk[i]);
  }
  TEST_ASSERT_EQUAL_UINT16(0, ph::accept::parseNominal(nullptr));
}

void test_parse_slot_accepts_only_lowercase_a_and_b(void)
{
  TEST_ASSERT_TRUE(ph::accept::parseSlot("a") == Slot::A);
  TEST_ASSERT_TRUE(ph::accept::parseSlot("b") == Slot::B);
  TEST_ASSERT_TRUE(ph::accept::parseSlot("A") == Slot::None);
  TEST_ASSERT_TRUE(ph::accept::parseSlot("ab") == Slot::None);
  TEST_ASSERT_TRUE(ph::accept::parseSlot("") == Slot::None);
  TEST_ASSERT_TRUE(ph::accept::parseSlot(nullptr) == Slot::None);
  TEST_ASSERT_TRUE(ph::accept::parseSlot("c") == Slot::None);
}

void test_picker_whitelist_and_defaults(void)
{
  const uint16_t expected[] = {400, 401, 686, 700, 918, 1001};
  TEST_ASSERT_EQUAL_UINT32(6, ph::accept::PICKER_NOMINAL_COUNT);
  for (size_t i = 0; i < ph::accept::PICKER_NOMINAL_COUNT; i++)
  {
    TEST_ASSERT_EQUAL_UINT16(expected[i], ph::accept::PICKER_NOMINALS[i]);
    TEST_ASSERT_TRUE(ph::accept::isPickerNominal(expected[i]));
  }
  TEST_ASSERT_EQUAL_UINT16(686, ph::accept::DEFAULT_NOMINAL_A);
  TEST_ASSERT_EQUAL_UINT16(918, ph::accept::DEFAULT_NOMINAL_B);
  TEST_ASSERT_FALSE(ph::accept::isPickerNominal(0));
  TEST_ASSERT_FALSE(ph::accept::isPickerNominal(500));
  TEST_ASSERT_FALSE(ph::accept::isPickerNominal(402));
  TEST_ASSERT_FALSE(ph::accept::isPickerNominal(1000));
}

void test_gap_refuses_close_printed_nominals(void)
{
  const uint16_t pairs[][2] = {{686, 700}, {400, 401}, {918, 1001}, {686, 686}};
  for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
  {
    const PairResult r = ph::accept::evaluatePair(onLine(pairs[i][0]), onLine(pairs[i][1]));
    TEST_ASSERT_TRUE(r.reason == Refusal::BuffersTooClose);
    TEST_ASSERT_FALSE(r.complete);
  }
}

void test_gap_allows_the_default_and_seven_to_nine(void)
{
  TEST_ASSERT_TRUE(ph::accept::evaluatePair(onLine(686), onLine(918)).reason == Refusal::None);
  TEST_ASSERT_TRUE(ph::accept::evaluatePair(onLine(700), onLine(918)).reason == Refusal::None);
}

void test_gap_boundary_is_inclusive_and_order_free(void)
{
  const uint16_t lo = 686;
  const uint16_t atGap = static_cast<uint16_t>(lo + ph::accept::PORTAL_MIN_BUFFER_GAP_HUNDREDTHS);
  const uint16_t underGap = static_cast<uint16_t>(atGap - 1);
  TEST_ASSERT_TRUE(ph::accept::evaluatePair(onLine(lo), onLine(atGap)).reason == Refusal::None);
  TEST_ASSERT_TRUE(ph::accept::evaluatePair(onLine(lo), onLine(underGap)).reason == Refusal::BuffersTooClose);
  // Order of A and B does not matter (a falling A/B order is still the same pair of buffers).
  TEST_ASSERT_TRUE(ph::accept::evaluatePair(onLine(atGap), onLine(lo)).reason == Refusal::None);
  TEST_ASSERT_TRUE(ph::accept::evaluatePair(onLine(underGap), onLine(lo)).reason == Refusal::BuffersTooClose);
  TEST_ASSERT_TRUE(ph::accept::evaluatePair(onLine(918), onLine(686)).reason == Refusal::None);
}

void test_wrong_sign_and_equal_mv(void)
{
  // mV falls as pH rises: the opposite of EXPECTED_SLOPE_SIGN.
  const float wrong = -static_cast<float>(ph::EXPECTED_SLOPE_SIGN) * FIXTURE_SLOPE_MV_PER_PH;
  const Point a = heldPoint(686, mvOnLine(ph::accept::BOARD_ZERO_INPUT_SENSOR_MV, wrong, 686), 686);
  const Point b = heldPoint(918, mvOnLine(ph::accept::BOARD_ZERO_INPUT_SENSOR_MV, wrong, 918), 918);
  TEST_ASSERT_TRUE(ph::accept::evaluatePair(a, b).reason == Refusal::WrongSign);

  const Point same = heldPoint(918, a.mv, 918);
  const PairResult r = ph::accept::evaluatePair(a, same);
  TEST_ASSERT_TRUE(r.reason == Refusal::SlopeTooSmall);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.0f, r.reasonValue);
}

void test_slope_floor_is_inclusive(void)
{
  const uint16_t mvA = static_cast<uint16_t>(std::lround(ph::accept::BOARD_ZERO_INPUT_SENSOR_MV));
  const uint16_t step = static_cast<uint16_t>(std::lround(ph::accept::MIN_SLOPE_MV_PER_PH * 2.0f));
  const Point a = heldPoint(700, mvA, 700);

  const PairResult atFloor = ph::accept::evaluatePair(a, heldPoint(918, static_cast<uint16_t>(mvA + step), 900));
  TEST_ASSERT_TRUE(atFloor.reason == Refusal::None);

  const PairResult under = ph::accept::evaluatePair(a, heldPoint(918, static_cast<uint16_t>(mvA + step - 1), 900));
  TEST_ASSERT_TRUE(under.reason == Refusal::SlopeTooSmall);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, (static_cast<float>(step) - 1.0f) / 2.0f, under.reasonValue);
}

void test_offset_window_plus_side_boundary(void)
{
  const float edge = ph::accept::BOARD_ZERO_INPUT_SENSOR_MV + ph::accept::OFFSET_WINDOW_SENSOR_MV;
  TEST_ASSERT_TRUE(pairWithMv7(edge).reason == Refusal::None);
  const PairResult r = pairWithMv7(edge + 1.0f);
  TEST_ASSERT_TRUE(r.reason == Refusal::OffsetOutOfWindow);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, edge + 1.0f, r.reasonValue);
}

void test_demo_slope_floor_accepts_five_and_refuses_below(void)
{
  const Point a = heldPoint(700, 1740, 700);
  const PairResult atFloor = ph::accept::evaluatePair(a, heldPoint(918, 1750, 900));
  TEST_ASSERT_TRUE(atFloor.reason == Refusal::None);
  TEST_ASSERT_TRUE(atFloor.complete);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 5.0f, atFloor.line.slopeMvPerPh);

  const PairResult below = ph::accept::evaluatePair(a, heldPoint(918, 1749, 900));
  TEST_ASSERT_TRUE(below.reason == Refusal::SlopeTooSmall);
  TEST_ASSERT_FALSE(below.complete);
  TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.5f, below.reasonValue);
}

void test_synthetic_weak_demo_pair_is_storage_valid_and_replace(void)
{
  // Synthetic regression, not a new hardware measurement: 27 mV over 5.15 pH is about 5.24 mV/pH.
  const PairResult r = ph::accept::evaluatePair(heldPoint(401, 1737, 401, 27.6f),
                                               heldPoint(918, 1764, 916, 27.6f));
  TEST_ASSERT_TRUE(r.reason == Refusal::None);
  TEST_ASSERT_TRUE(r.complete);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, 5.24f, r.line.slopeMvPerPh);
  TEST_ASSERT_TRUE(r.verdict == Verdict::Replace);
  TEST_ASSERT_TRUE(ph::isValidCalibration(r.cal));
}

void test_offset_window_minus_side_boundary(void)
{
  const float edge = ph::accept::BOARD_ZERO_INPUT_SENSOR_MV - ph::accept::OFFSET_WINDOW_SENSOR_MV;
  TEST_ASSERT_TRUE(pairWithMv7(edge).reason == Refusal::None);
  const PairResult r = pairWithMv7(edge - 1.0f);
  TEST_ASSERT_TRUE(r.reason == Refusal::OffsetOutOfWindow);
  TEST_ASSERT_FLOAT_WITHIN(0.01f, edge - 1.0f, r.reasonValue);
}

void test_bench_probe_pair_is_accepted_with_replace_verdict(void)
{
  // 10-BENCH-RECORD §2 multimeter: 6.86 -> 1740 mV, 9.18 -> 1790 mV at about 25 C.
  const PairResult r = ph::accept::evaluatePair(heldPoint(686, 1740, 686, 25.0f), heldPoint(918, 1790, 918, 25.0f));
  TEST_ASSERT_TRUE(r.reason == Refusal::None);
  TEST_ASSERT_TRUE(r.complete);
  TEST_ASSERT_FLOAT_WITHIN(0.2f, 7.5f, r.slopePct);
  TEST_ASSERT_FLOAT_WITHIN(0.02f, -0.14f, r.offsetPh);
  TEST_ASSERT_TRUE(r.verdict == Verdict::Replace);
  TEST_ASSERT_EQUAL_INT16(2500, r.cal.tcalCentiC);
  TEST_ASSERT_EQUAL_UINT8(ph::PH_CAL_VERSION, r.cal.version);
  TEST_ASSERT_TRUE(ph::cal::checkCalibration(r.cal) == ph::cal::CalCheck::Ok);
  TEST_ASSERT_TRUE(std::isnan(r.reasonValue));
}

void test_tcal_is_the_mean_of_the_point_temperatures(void)
{
  Point a = onLine(686);
  Point b = onLine(918);
  a.tempC = 24.0f;
  b.tempC = 27.0f;
  const PairResult r = ph::accept::evaluatePair(a, b);
  TEST_ASSERT_TRUE(r.reason == Refusal::None);
  TEST_ASSERT_EQUAL_INT16(2550, r.cal.tcalCentiC);
}

void test_unheld_point_is_invalid(void)
{
  Point b = onLine(918);
  b.held = false;
  TEST_ASSERT_TRUE(ph::accept::evaluatePair(onLine(686), b).reason == Refusal::Invalid);
}

void test_verdict_slope_bands(void)
{
  using namespace ph::accept;
  TEST_ASSERT_TRUE(healthVerdict(HEALTH_SLOPE_GOOD_PCT, 0.0f) == Verdict::Good);
  TEST_ASSERT_TRUE(healthVerdict(HEALTH_SLOPE_GOOD_PCT - 0.01f, 0.0f) == Verdict::Aging);
  TEST_ASSERT_TRUE(healthVerdict(HEALTH_SLOPE_AGING_PCT, 0.0f) == Verdict::Aging);
  TEST_ASSERT_TRUE(healthVerdict(HEALTH_SLOPE_AGING_PCT - 0.01f, 0.0f) == Verdict::Replace);
}

void test_verdict_offset_bands(void)
{
  using namespace ph::accept;
  TEST_ASSERT_TRUE(healthVerdict(100.0f, HEALTH_OFFSET_GOOD_PH) == Verdict::Good);
  TEST_ASSERT_TRUE(healthVerdict(100.0f, -HEALTH_OFFSET_GOOD_PH) == Verdict::Good);
  TEST_ASSERT_TRUE(healthVerdict(100.0f, HEALTH_OFFSET_GOOD_PH + 0.01f) == Verdict::Aging);
  TEST_ASSERT_TRUE(healthVerdict(100.0f, HEALTH_OFFSET_AGING_PH) == Verdict::Aging);
  TEST_ASSERT_TRUE(healthVerdict(100.0f, -HEALTH_OFFSET_AGING_PH) == Verdict::Aging);
  TEST_ASSERT_TRUE(healthVerdict(100.0f, HEALTH_OFFSET_AGING_PH + 0.01f) == Verdict::Replace);
}

void test_verdict_worse_of_two_decides_and_nan_is_replace(void)
{
  using namespace ph::accept;
  TEST_ASSERT_TRUE(healthVerdict(100.0f, HEALTH_OFFSET_AGING_PH + 0.01f) == Verdict::Replace);
  TEST_ASSERT_TRUE(healthVerdict(HEALTH_SLOPE_AGING_PCT, 0.0f) == Verdict::Aging);
  TEST_ASSERT_TRUE(healthVerdict(NAN, 0.0f) == Verdict::Replace);
  TEST_ASSERT_TRUE(healthVerdict(100.0f, NAN) == Verdict::Replace);
}

void test_verdict_has_no_upper_slope_bound(void)
{
  TEST_ASSERT_TRUE(ph::accept::healthVerdict(150.0f, 0.0f) == Verdict::Good);
}

void test_age_date_unknown_and_no_clock(void)
{
  using namespace ph::accept;
  TEST_ASSERT_TRUE(calibrationAge(0, static_cast<long long>(FIXTURE_CAL_AT)).age == Age::DateUnknown);
  TEST_ASSERT_TRUE(calibrationAge(FIXTURE_CAL_AT, static_cast<long long>(MIN_VALID_EPOCH)).age == Age::NoClock);
  TEST_ASSERT_TRUE(calibrationAge(FIXTURE_CAL_AT, 0).age == Age::NoClock);
}

void test_age_overdue_boundary(void)
{
  using namespace ph::accept;
  const long long limit = static_cast<long long>(FIXTURE_CAL_AT) +
                          static_cast<long long>(OVERDUE_AFTER_DAYS) * static_cast<long long>(SECONDS_PER_DAY);
  const ph::accept::AgeResult atLimit = calibrationAge(FIXTURE_CAL_AT, limit);
  TEST_ASSERT_TRUE(atLimit.age == Age::Fresh);
  TEST_ASSERT_EQUAL_UINT32(OVERDUE_AFTER_DAYS, atLimit.days);
  const ph::accept::AgeResult past = calibrationAge(FIXTURE_CAL_AT, limit + 1);
  TEST_ASSERT_TRUE(past.age == Age::Overdue);
  TEST_ASSERT_EQUAL_UINT32(OVERDUE_AFTER_DAYS, past.days);
}

void test_age_future_calibration_is_fresh_zero_days(void)
{
  const ph::accept::AgeResult r =
      ph::accept::calibrationAge(FIXTURE_CAL_AT + 1000UL, static_cast<long long>(FIXTURE_CAL_AT));
  TEST_ASSERT_TRUE(r.age == Age::Fresh);
  TEST_ASSERT_EQUAL_UINT32(0, r.days);
}

void test_accept_into_slot_first_then_second(void)
{
  HeldPair held{};
  const PairResult first = ph::accept::acceptIntoSlot(held, Slot::A, onLine(686));
  TEST_ASSERT_TRUE(first.reason == Refusal::None);
  TEST_ASSERT_FALSE(first.complete);
  TEST_ASSERT_TRUE(held.a.held);
  TEST_ASSERT_FALSE(held.b.held);

  const PairResult second = ph::accept::acceptIntoSlot(held, Slot::B, onLine(918));
  TEST_ASSERT_TRUE(second.reason == Refusal::None);
  TEST_ASSERT_TRUE(second.complete);
  TEST_ASSERT_TRUE(held.a.held);
  TEST_ASSERT_TRUE(held.b.held);
  TEST_ASSERT_TRUE(second.slopePct > 0.0f);
  TEST_ASSERT_TRUE(std::isfinite(second.offsetPh));
  TEST_ASSERT_TRUE(second.verdict == ph::accept::healthVerdict(second.slopePct, second.offsetPh));
}

void test_accept_into_slot_refusal_drops_the_second_capture(void)
{
  HeldPair held{};
  ph::accept::acceptIntoSlot(held, Slot::A, onLine(686));
  const PairResult r = ph::accept::acceptIntoSlot(held, Slot::B, onLine(700));
  TEST_ASSERT_TRUE(r.reason == Refusal::BuffersTooClose);
  TEST_ASSERT_FALSE(held.b.held);
  TEST_ASSERT_TRUE(held.a.held);
  TEST_ASSERT_EQUAL_UINT16(686, held.a.nominal);

  // B captured first, A second: the refusal drops A and keeps B.
  HeldPair other{};
  ph::accept::acceptIntoSlot(other, Slot::B, onLine(918));
  ph::accept::acceptIntoSlot(other, Slot::A, onLine(1001));
  TEST_ASSERT_FALSE(other.a.held);
  TEST_ASSERT_TRUE(other.b.held);

  HeldPair none{};
  TEST_ASSERT_TRUE(ph::accept::acceptIntoSlot(none, Slot::None, onLine(686)).reason == Refusal::Invalid);
  TEST_ASSERT_FALSE(none.a.held);
  TEST_ASSERT_FALSE(none.b.held);
}

void test_drop_slot_clears_only_that_slot(void)
{
  HeldPair held{};
  ph::accept::acceptIntoSlot(held, Slot::A, onLine(686));
  ph::accept::acceptIntoSlot(held, Slot::B, onLine(918));
  ph::accept::dropSlot(held, Slot::A);
  TEST_ASSERT_FALSE(held.a.held);
  TEST_ASSERT_EQUAL_UINT16(0, held.a.mv);
  TEST_ASSERT_TRUE(held.b.held);
}

void test_check_save(void)
{
  HeldPair held{};
  ph::accept::acceptIntoSlot(held, Slot::A, onLine(686));
  TEST_ASSERT_TRUE(ph::accept::checkSave(held, 686, 918).reason == Refusal::NothingToSave);

  ph::accept::acceptIntoSlot(held, Slot::B, onLine(918));
  TEST_ASSERT_TRUE(ph::accept::checkSave(held, 686, 700).reason == Refusal::Changed);
  TEST_ASSERT_TRUE(ph::accept::checkSave(held, 918, 686).reason == Refusal::Changed);

  const PairResult ok = ph::accept::checkSave(held, 686, 918);
  TEST_ASSERT_TRUE(ok.reason == Refusal::None);
  TEST_ASSERT_TRUE(ok.complete);
  TEST_ASSERT_EQUAL_UINT16(held.a.mv, ok.cal.mvA);
  TEST_ASSERT_EQUAL_UINT16(held.b.mv, ok.cal.mvB);
  TEST_ASSERT_EQUAL_UINT16(686, ok.cal.bufA);
  TEST_ASSERT_EQUAL_UINT16(918, ok.cal.bufB);
  TEST_ASSERT_TRUE(ph::cal::checkCalibration(ok.cal) == ph::cal::CalCheck::Ok);
}

void test_refusal_tokens_cover_every_enumerator(void)
{
  using ph::accept::refusalToken;
  TEST_ASSERT_EQUAL_STRING("none", refusalToken(Refusal::None));
  TEST_ASSERT_EQUAL_STRING("no_temperature", refusalToken(Refusal::NoTemperature));
  TEST_ASSERT_EQUAL_STRING("temp_out_of_range", refusalToken(Refusal::TemperatureOutOfRange));
  TEST_ASSERT_EQUAL_STRING("signal_lost", refusalToken(Refusal::SignalLost));
  TEST_ASSERT_EQUAL_STRING("unstable", refusalToken(Refusal::Unstable));
  TEST_ASSERT_EQUAL_STRING("settle_timeout", refusalToken(Refusal::SettleTimeout));
  TEST_ASSERT_EQUAL_STRING("buffers_too_close", refusalToken(Refusal::BuffersTooClose));
  TEST_ASSERT_EQUAL_STRING("wrong_sign", refusalToken(Refusal::WrongSign));
  TEST_ASSERT_EQUAL_STRING("slope_too_small", refusalToken(Refusal::SlopeTooSmall));
  TEST_ASSERT_EQUAL_STRING("offset_out_of_window", refusalToken(Refusal::OffsetOutOfWindow));
  TEST_ASSERT_EQUAL_STRING("invalid", refusalToken(Refusal::Invalid));
  TEST_ASSERT_EQUAL_STRING("cancelled", refusalToken(Refusal::Cancelled));
  TEST_ASSERT_EQUAL_STRING("changed", refusalToken(Refusal::Changed));
  TEST_ASSERT_EQUAL_STRING("nothing_to_save", refusalToken(Refusal::NothingToSave));
  TEST_ASSERT_EQUAL_STRING("nvs", refusalToken(Refusal::Nvs));
  TEST_ASSERT_EQUAL_STRING("invalid", refusalToken(static_cast<Refusal>(99)));
}

void test_verdict_age_slot_tokens_cover_every_enumerator(void)
{
  using namespace ph::accept;
  TEST_ASSERT_EQUAL_STRING("good", verdictToken(Verdict::Good));
  TEST_ASSERT_EQUAL_STRING("aging", verdictToken(Verdict::Aging));
  TEST_ASSERT_EQUAL_STRING("replace", verdictToken(Verdict::Replace));
  TEST_ASSERT_EQUAL_STRING("replace", verdictToken(static_cast<Verdict>(99)));

  TEST_ASSERT_EQUAL_STRING("fresh", ageToken(Age::Fresh));
  TEST_ASSERT_EQUAL_STRING("overdue", ageToken(Age::Overdue));
  TEST_ASSERT_EQUAL_STRING("date_unknown", ageToken(Age::DateUnknown));
  TEST_ASSERT_EQUAL_STRING("no_clock", ageToken(Age::NoClock));
  TEST_ASSERT_EQUAL_STRING("date_unknown", ageToken(static_cast<Age>(99)));

  TEST_ASSERT_EQUAL_STRING("none", slotToken(Slot::None));
  TEST_ASSERT_EQUAL_STRING("a", slotToken(Slot::A));
  TEST_ASSERT_EQUAL_STRING("b", slotToken(Slot::B));
  TEST_ASSERT_EQUAL_STRING("none", slotToken(static_cast<Slot>(99)));
}

// ---- Capture engine (plan 11-04) ---------------------------------------------------------------------------------

namespace
{
  using ph::accept::CaptureEngine;
  using ph::accept::CaptureState;
  using ph::accept::LiveState;

  constexpr size_t WINDOW = ph::cal::CAPTURE_WINDOW_SAMPLES;
  constexpr float CUP_C = 25.0f;

  // Feeds `count` identical samples with elapsed seconds counting up from `elapsedS`; returns the next second.
  uint16_t feedFlat(CaptureEngine &e, float mv, float tempC, size_t count, uint16_t elapsedS)
  {
    for (size_t i = 0; i < count; i++)
    {
      ph::accept::feedCapture(e, mv, tempC, elapsedS);
      elapsedS++;
    }
    return elapsedS;
  }

  CaptureEngine started(uint16_t nominal = 918)
  {
    CaptureEngine e{};
    ph::accept::startCapture(e, nominal, CUP_C);
    return e;
  }

  // Feeds a 20-sample window: first DRIFT_EDGE_SAMPLES at `first`, last DRIFT_EDGE_SAMPLES at `last`, the middle
  // at `middle`.
  void feedEdges(CaptureEngine &e, float first, float middle, float last)
  {
    uint16_t t = 1;
    t = feedFlat(e, first, CUP_C, ph::accept::DRIFT_EDGE_SAMPLES, t);
    t = feedFlat(e, middle, CUP_C, WINDOW - 2 * ph::accept::DRIFT_EDGE_SAMPLES, t);
    feedFlat(e, last, CUP_C, ph::accept::DRIFT_EDGE_SAMPLES, t);
  }
}

void test_drift_uses_chronological_order_on_wrapped_ring(void)
{
  using namespace ph::accept;
  // Ring written up to slot 6, so the oldest sample is slot 7: chronological 0..4 = slots 7..11, 15..19 = slots 2..6.
  float ring[WINDOW];
  for (size_t i = 0; i < WINDOW; i++)
  {
    ring[i] = 1005.0f;
  }
  for (size_t i = 7; i <= 11; i++)
  {
    ring[i] = 1000.0f;
  }
  for (size_t i = 2; i <= 6; i++)
  {
    ring[i] = 1010.0f;
  }
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 10.0f, driftOf(ring, 7, WINDOW));
  // Read naively from slot 0 the same ring would claim a negative drift; the chronological unroll must not.
  TEST_ASSERT_TRUE(driftOf(ring, 7, WINDOW) > 0.0f);

  float flat[WINDOW];
  for (size_t i = 0; i < WINDOW; i++)
  {
    const size_t slot = (7 + i) % WINDOW;
    flat[i] = ring[slot];
  }
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 10.0f, driftOf(flat, 0, WINDOW));

  TEST_ASSERT_TRUE(std::isnan(driftOf(ring, 7, WINDOW - 1)));
  TEST_ASSERT_TRUE(std::isnan(driftOf(nullptr, 0, WINDOW)));
  ring[2] = NAN;
  TEST_ASSERT_TRUE(std::isnan(driftOf(ring, 7, WINDOW)));
}

void test_live_state_settling_steady_signal_lost(void)
{
  using namespace ph::accept;
  float ring[WINDOW];
  for (size_t i = 0; i < WINDOW; i++)
  {
    ring[i] = 1500.0f;
  }
  TEST_ASSERT_EQUAL(static_cast<int>(LiveState::Settling), static_cast<int>(liveState(ring, 0, 0)));
  TEST_ASSERT_EQUAL(static_cast<int>(LiveState::Settling), static_cast<int>(liveState(ring, 10, 10)));
  TEST_ASSERT_EQUAL(static_cast<int>(LiveState::Steady), static_cast<int>(liveState(ring, 0, WINDOW)));

  float drifting[WINDOW];
  for (size_t i = 0; i < WINDOW; i++)
  {
    drifting[i] = 1500.0f + static_cast<float>(i);
  }
  TEST_ASSERT_EQUAL(static_cast<int>(LiveState::Settling), static_cast<int>(liveState(drifting, 0, WINDOW)));

  float jumpy[WINDOW];
  for (size_t i = 0; i < WINDOW; i++)
  {
    jumpy[i] = 1500.0f;
  }
  jumpy[10] = 1500.0f + MAX_CAPTURE_SPREAD_SENSOR_MV + 1.0f;
  TEST_ASSERT_EQUAL(static_cast<int>(LiveState::Settling), static_cast<int>(liveState(jumpy, 0, WINDOW)));

  ring[3] = NAN;
  TEST_ASSERT_EQUAL(static_cast<int>(LiveState::SignalLost), static_cast<int>(liveState(ring, 5, 5)));
  TEST_ASSERT_EQUAL(static_cast<int>(LiveState::SignalLost), static_cast<int>(liveState(ring, 0, WINDOW)));
}

void test_start_refuses_no_temperature(void)
{
  using namespace ph::accept;
  CaptureEngine e{};
  startCapture(e, 918, NAN);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Refused), static_cast<int>(e.state));
  TEST_ASSERT_EQUAL(static_cast<int>(Refusal::NoTemperature), static_cast<int>(e.reason));
  TEST_ASSERT_FALSE(isCaptureRunning(e));
}

void test_start_refuses_temperature_out_of_range(void)
{
  using namespace ph::accept;
  const float cold = ph::cal::BUFFER_TABLE_MIN_C - 0.1f;
  const float hot = ph::cal::BUFFER_TABLE_MAX_C + 0.1f;
  CaptureEngine e{};
  startCapture(e, 918, cold);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Refused), static_cast<int>(e.state));
  TEST_ASSERT_EQUAL(static_cast<int>(Refusal::TemperatureOutOfRange), static_cast<int>(e.reason));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, cold, e.reasonValue);

  startCapture(e, 918, hot);
  TEST_ASSERT_EQUAL(static_cast<int>(Refusal::TemperatureOutOfRange), static_cast<int>(e.reason));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, hot, e.reasonValue);

  // The table edges themselves are inside the range.
  startCapture(e, 918, ph::cal::BUFFER_TABLE_MIN_C);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Capturing), static_cast<int>(e.state));
  startCapture(e, 918, ph::cal::BUFFER_TABLE_MAX_C);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Capturing), static_cast<int>(e.state));
}

void test_start_refuses_unknown_nominal(void)
{
  using namespace ph::accept;
  CaptureEngine e{};
  startCapture(e, 500, CUP_C);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Refused), static_cast<int>(e.state));
  TEST_ASSERT_EQUAL(static_cast<int>(Refusal::Invalid), static_cast<int>(e.reason));

  startCapture(e, 918, CUP_C);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Capturing), static_cast<int>(e.state));
  TEST_ASSERT_EQUAL(static_cast<int>(Refusal::None), static_cast<int>(e.reason));
  TEST_ASSERT_EQUAL_UINT16(918, e.nominal);
  TEST_ASSERT_EQUAL(0, static_cast<int>(e.filled));
  TEST_ASSERT_EQUAL_UINT16(0, e.elapsedS);
  TEST_ASSERT_FALSE(e.point.held);
  TEST_ASSERT_TRUE(isCaptureRunning(e));
}

void test_feed_nan_mv_is_signal_lost(void)
{
  using namespace ph::accept;
  CaptureEngine e = started();
  feedFlat(e, 1790.0f, CUP_C, 5, 1);
  feedCapture(e, NAN, CUP_C, 6);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Refused), static_cast<int>(e.state));
  TEST_ASSERT_EQUAL(static_cast<int>(Refusal::SignalLost), static_cast<int>(e.reason));
}

void test_feed_nan_temperature_is_no_temperature(void)
{
  using namespace ph::accept;
  CaptureEngine e = started();
  feedFlat(e, 1790.0f, CUP_C, 5, 1);
  feedCapture(e, 1790.0f, NAN, 6);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Refused), static_cast<int>(e.state));
  TEST_ASSERT_EQUAL(static_cast<int>(Refusal::NoTemperature), static_cast<int>(e.reason));

  const float hot = ph::cal::BUFFER_TABLE_MAX_C + 0.5f;
  e = started();
  feedFlat(e, 1790.0f, CUP_C, 5, 1);
  feedCapture(e, 1790.0f, hot, 6);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Refused), static_cast<int>(e.state));
  TEST_ASSERT_EQUAL(static_cast<int>(Refusal::TemperatureOutOfRange), static_cast<int>(e.reason));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, hot, e.reasonValue);
}

void test_steady_window_accepts_with_median_and_buffer_ph(void)
{
  using namespace ph::accept;
  CaptureEngine e = started(918);
  const uint16_t t = feedFlat(e, 1790.0f, CUP_C, WINDOW - 1, 1);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Capturing), static_cast<int>(e.state));
  TEST_ASSERT_EQUAL(static_cast<int>(WINDOW - 1), static_cast<int>(e.filled));
  feedCapture(e, 1790.0f, CUP_C, t);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Accepted), static_cast<int>(e.state));
  TEST_ASSERT_EQUAL(static_cast<int>(Refusal::None), static_cast<int>(e.reason));
  TEST_ASSERT_TRUE(e.point.held);
  TEST_ASSERT_EQUAL_UINT16(918, e.point.nominal);
  TEST_ASSERT_EQUAL_UINT16(1790, e.point.mv);
  const long expectBuf = std::lround(ph::cal::bufferPhAt(918, CUP_C) * 100.0f);
  TEST_ASSERT_EQUAL(expectBuf, static_cast<long>(e.point.bufHundredths));
  TEST_ASSERT_EQUAL_UINT16(918, e.point.bufHundredths);
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, CUP_C, e.point.tempC);
  TEST_ASSERT_FALSE(isCaptureRunning(e));
}

void test_point_temperature_is_window_mean(void)
{
  using namespace ph::accept;
  CaptureEngine e = started(686);
  const uint16_t t = feedFlat(e, 1740.0f, 24.0f, WINDOW / 2, 1);
  feedFlat(e, 1740.0f, 26.0f, WINDOW / 2, t);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Accepted), static_cast<int>(e.state));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, 25.0f, e.point.tempC);
  const long expectBuf = std::lround(ph::cal::bufferPhAt(686, 25.0f) * 100.0f);
  TEST_ASSERT_EQUAL(expectBuf, static_cast<long>(e.point.bufHundredths));
}

void test_drift_boundary_is_inclusive(void)
{
  using namespace ph::accept;
  CaptureEngine e = started();
  feedEdges(e, 1000.0f, 1000.0f + MAX_DRIFT_SENSOR_MV / 2.0f, 1000.0f + MAX_DRIFT_SENSOR_MV);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Accepted), static_cast<int>(e.state));

  e = started();
  feedEdges(e, 1000.0f, 1000.0f + MAX_DRIFT_SENSOR_MV / 2.0f, 1000.0f + MAX_DRIFT_SENSOR_MV + 0.1f);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Settling), static_cast<int>(e.state));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, MAX_DRIFT_SENSOR_MV + 0.1f, e.lastDriftMv);
  TEST_ASSERT_TRUE(isCaptureRunning(e));

  // A downward drift is judged on its magnitude.
  e = started();
  feedEdges(e, 1000.0f, 1000.0f, 1000.0f - MAX_DRIFT_SENSOR_MV - 0.1f);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Settling), static_cast<int>(e.state));
  TEST_ASSERT_FLOAT_WITHIN(1e-3f, -(MAX_DRIFT_SENSOR_MV + 0.1f), e.lastDriftMv);
}

void test_spread_boundary_is_inclusive(void)
{
  using namespace ph::accept;
  CaptureEngine e = started();
  uint16_t t = feedFlat(e, 1000.0f, CUP_C, WINDOW / 2, 1);
  t = feedFlat(e, 1000.0f + MAX_CAPTURE_SPREAD_SENSOR_MV, CUP_C, 1, t);
  feedFlat(e, 1000.0f, CUP_C, WINDOW / 2 - 1, t);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Accepted), static_cast<int>(e.state));
  TEST_ASSERT_EQUAL_UINT16(1000, e.point.mv);

  e = started();
  t = feedFlat(e, 1000.0f, CUP_C, WINDOW / 2, 1);
  t = feedFlat(e, 1000.0f + MAX_CAPTURE_SPREAD_SENSOR_MV + 0.1f, CUP_C, 1, t);
  feedFlat(e, 1000.0f, CUP_C, WINDOW / 2 - 1, t);
  // Too wide, but not refused before the timeout: the engine keeps trying.
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Settling), static_cast<int>(e.state));
  TEST_ASSERT_EQUAL(static_cast<int>(Refusal::Unstable), static_cast<int>(e.lastNotReady));
}

void test_elapsed_seconds_come_from_the_caller(void)
{
  using namespace ph::accept;
  // A ramp that never settles, fed every other second: the timeout follows the supplied wall-clock seconds, so it
  // fires after SETTLE_TIMEOUT_S / 2 + 1 feeds, not after SETTLE_TIMEOUT_S feeds.
  CaptureEngine e = started();
  size_t feeds = 0;
  for (uint16_t t = 0; t < SETTLE_TIMEOUT_S; t = static_cast<uint16_t>(t + 2))
  {
    feedCapture(e, 1000.0f + static_cast<float>(feeds), CUP_C, t);
    feeds++;
    TEST_ASSERT_EQUAL_UINT16(t, e.elapsedS);
    TEST_ASSERT_TRUE(isCaptureRunning(e));
  }
  feedCapture(e, 1000.0f + static_cast<float>(feeds), CUP_C, SETTLE_TIMEOUT_S);
  feeds++;
  TEST_ASSERT_EQUAL_UINT16(SETTLE_TIMEOUT_S, e.elapsedS);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Refused), static_cast<int>(e.state));
  TEST_ASSERT_EQUAL(static_cast<int>(Refusal::SettleTimeout), static_cast<int>(e.reason));
  TEST_ASSERT_EQUAL(static_cast<int>(SETTLE_TIMEOUT_S / 2 + 1), static_cast<int>(feeds));
}

void test_long_settling_keeps_trying_then_accepts(void)
{
  using namespace ph::accept;
  CaptureEngine e = started();
  const size_t RAMP = 40;
  uint16_t t = 1;
  for (size_t i = 0; i < RAMP; i++)
  {
    feedCapture(e, 1100.0f - static_cast<float>(RAMP) + static_cast<float>(i), CUP_C, t);
    t++;
    if (i + 1 >= WINDOW)
    {
      TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Settling), static_cast<int>(e.state));
    }
  }
  while (e.state == CaptureState::Settling && t < SETTLE_TIMEOUT_S)
  {
    feedCapture(e, 1100.0f, CUP_C, t);
    t++;
  }
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Accepted), static_cast<int>(e.state));
  TEST_ASSERT_TRUE(e.elapsedS < SETTLE_TIMEOUT_S);
  TEST_ASSERT_UINT16_WITHIN(static_cast<uint16_t>(MAX_DRIFT_SENSOR_MV) + 1, 1100, e.point.mv);
}

void test_settle_timeout_refuses_with_minutes(void)
{
  using namespace ph::accept;
  // 1 mV per second for ever: the drift sits well above MAX_DRIFT_SENSOR_MV while the spread stays inside the
  // backstop, so drift is the last blocker.
  CaptureEngine e = started();
  for (uint16_t t = 1; t < SETTLE_TIMEOUT_S; t++)
  {
    feedCapture(e, 1000.0f + static_cast<float>(t), CUP_C, t);
  }
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Settling), static_cast<int>(e.state));
  TEST_ASSERT_EQUAL_UINT16(SETTLE_TIMEOUT_S - 1, e.elapsedS);
  feedCapture(e, 1000.0f + static_cast<float>(SETTLE_TIMEOUT_S), CUP_C, SETTLE_TIMEOUT_S);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Refused), static_cast<int>(e.state));
  TEST_ASSERT_EQUAL(static_cast<int>(Refusal::SettleTimeout), static_cast<int>(e.reason));
  TEST_ASSERT_FLOAT_WITHIN(1e-4f, SETTLE_TIMEOUT_S / 60.0f, e.reasonValue);
  TEST_ASSERT_FALSE(e.point.held);
}

void test_unstable_at_timeout(void)
{
  using namespace ph::accept;
  CaptureEngine e = started();
  for (uint16_t t = 1; t <= SETTLE_TIMEOUT_S; t++)
  {
    const float mv = (t % 2 == 0) ? 1000.0f : 1000.0f + 2.0f * MAX_CAPTURE_SPREAD_SENSOR_MV;
    feedCapture(e, mv, CUP_C, t);
  }
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Refused), static_cast<int>(e.state));
  TEST_ASSERT_EQUAL(static_cast<int>(Refusal::Unstable), static_cast<int>(e.reason));
  TEST_ASSERT_TRUE(std::isnan(e.reasonValue));
}

void test_feed_after_finish_is_ignored(void)
{
  using namespace ph::accept;
  CaptureEngine e = started();
  const uint16_t t = feedFlat(e, 1790.0f, CUP_C, WINDOW, 1);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Accepted), static_cast<int>(e.state));
  feedCapture(e, NAN, NAN, t);
  feedCapture(e, 5.0f, CUP_C, t + 1);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Accepted), static_cast<int>(e.state));
  TEST_ASSERT_EQUAL_UINT16(1790, e.point.mv);
  TEST_ASSERT_EQUAL_UINT16(t - 1, e.elapsedS);

  e = started();
  feedCapture(e, NAN, CUP_C, 1);
  TEST_ASSERT_EQUAL(static_cast<int>(Refusal::SignalLost), static_cast<int>(e.reason));
  feedFlat(e, 1790.0f, CUP_C, WINDOW, 2);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Refused), static_cast<int>(e.state));
  TEST_ASSERT_EQUAL(static_cast<int>(Refusal::SignalLost), static_cast<int>(e.reason));
  TEST_ASSERT_EQUAL(0, static_cast<int>(e.filled));
}

void test_cancel_refuses_cancelled(void)
{
  using namespace ph::accept;
  CaptureEngine idle{};
  TEST_ASSERT_FALSE(isCaptureRunning(idle));
  cancelCapture(idle);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Idle), static_cast<int>(idle.state));

  CaptureEngine e = started();
  feedFlat(e, 1790.0f, CUP_C, 3, 1);
  TEST_ASSERT_TRUE(isCaptureRunning(e));
  cancelCapture(e);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Refused), static_cast<int>(e.state));
  TEST_ASSERT_EQUAL(static_cast<int>(Refusal::Cancelled), static_cast<int>(e.reason));
  TEST_ASSERT_FALSE(isCaptureRunning(e));

  // Settling is running too, so it can be cancelled.
  e = started();
  feedEdges(e, 1000.0f, 1000.0f, 1000.0f + MAX_DRIFT_SENSOR_MV + 1.0f);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Settling), static_cast<int>(e.state));
  TEST_ASSERT_TRUE(isCaptureRunning(e));
  cancelCapture(e);
  TEST_ASSERT_EQUAL(static_cast<int>(Refusal::Cancelled), static_cast<int>(e.reason));

  // A finished capture is not overwritten by a late cancel.
  e = started();
  feedFlat(e, 1790.0f, CUP_C, WINDOW, 1);
  cancelCapture(e);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Accepted), static_cast<int>(e.state));
}

void test_zero_median_is_signal_lost(void)
{
  using namespace ph::accept;
  CaptureEngine e = started();
  feedFlat(e, 0.0f, CUP_C, WINDOW, 1);
  TEST_ASSERT_EQUAL(static_cast<int>(CaptureState::Refused), static_cast<int>(e.state));
  TEST_ASSERT_EQUAL(static_cast<int>(Refusal::SignalLost), static_cast<int>(e.reason));
  TEST_ASSERT_FALSE(e.point.held);
}

void test_every_picker_nominal_has_buffer_rows(void)
{
  using namespace ph::accept;
  const float temps[4] = {20.0f, 25.0f, 30.0f, 35.0f};
  for (size_t i = 0; i < PICKER_NOMINAL_COUNT; i++)
  {
    for (size_t j = 0; j < 4; j++)
    {
      TEST_ASSERT_FALSE(std::isnan(ph::cal::bufferPhAt(PICKER_NOMINALS[i], temps[j])));
    }
  }
}

void test_capture_tokens_cover_every_enumerator(void)
{
  using namespace ph::accept;
  TEST_ASSERT_EQUAL_STRING("settling", liveStateToken(LiveState::Settling));
  TEST_ASSERT_EQUAL_STRING("steady", liveStateToken(LiveState::Steady));
  TEST_ASSERT_EQUAL_STRING("signal_lost", liveStateToken(LiveState::SignalLost));
  TEST_ASSERT_EQUAL_STRING("signal_lost", liveStateToken(static_cast<LiveState>(99)));

  TEST_ASSERT_EQUAL_STRING("idle", captureStateToken(CaptureState::Idle));
  TEST_ASSERT_EQUAL_STRING("capturing", captureStateToken(CaptureState::Capturing));
  TEST_ASSERT_EQUAL_STRING("settling", captureStateToken(CaptureState::Settling));
  TEST_ASSERT_EQUAL_STRING("accepted", captureStateToken(CaptureState::Accepted));
  TEST_ASSERT_EQUAL_STRING("refused", captureStateToken(CaptureState::Refused));
  TEST_ASSERT_EQUAL_STRING("refused", captureStateToken(static_cast<CaptureState>(99)));
}

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_parse_nominal_accepts_exactly_the_picker_strings);
  RUN_TEST(test_parse_nominal_refuses_junk);
  RUN_TEST(test_parse_slot_accepts_only_lowercase_a_and_b);
  RUN_TEST(test_picker_whitelist_and_defaults);
  RUN_TEST(test_gap_refuses_close_printed_nominals);
  RUN_TEST(test_gap_allows_the_default_and_seven_to_nine);
  RUN_TEST(test_gap_boundary_is_inclusive_and_order_free);
  RUN_TEST(test_wrong_sign_and_equal_mv);
  RUN_TEST(test_slope_floor_is_inclusive);
  RUN_TEST(test_demo_slope_floor_accepts_five_and_refuses_below);
  RUN_TEST(test_synthetic_weak_demo_pair_is_storage_valid_and_replace);
  RUN_TEST(test_offset_window_plus_side_boundary);
  RUN_TEST(test_offset_window_minus_side_boundary);
  RUN_TEST(test_bench_probe_pair_is_accepted_with_replace_verdict);
  RUN_TEST(test_tcal_is_the_mean_of_the_point_temperatures);
  RUN_TEST(test_unheld_point_is_invalid);
  RUN_TEST(test_verdict_slope_bands);
  RUN_TEST(test_verdict_offset_bands);
  RUN_TEST(test_verdict_worse_of_two_decides_and_nan_is_replace);
  RUN_TEST(test_verdict_has_no_upper_slope_bound);
  RUN_TEST(test_age_date_unknown_and_no_clock);
  RUN_TEST(test_age_overdue_boundary);
  RUN_TEST(test_age_future_calibration_is_fresh_zero_days);
  RUN_TEST(test_accept_into_slot_first_then_second);
  RUN_TEST(test_accept_into_slot_refusal_drops_the_second_capture);
  RUN_TEST(test_drop_slot_clears_only_that_slot);
  RUN_TEST(test_check_save);
  RUN_TEST(test_refusal_tokens_cover_every_enumerator);
  RUN_TEST(test_verdict_age_slot_tokens_cover_every_enumerator);
  RUN_TEST(test_drift_uses_chronological_order_on_wrapped_ring);
  RUN_TEST(test_live_state_settling_steady_signal_lost);
  RUN_TEST(test_start_refuses_no_temperature);
  RUN_TEST(test_start_refuses_temperature_out_of_range);
  RUN_TEST(test_start_refuses_unknown_nominal);
  RUN_TEST(test_feed_nan_mv_is_signal_lost);
  RUN_TEST(test_feed_nan_temperature_is_no_temperature);
  RUN_TEST(test_steady_window_accepts_with_median_and_buffer_ph);
  RUN_TEST(test_point_temperature_is_window_mean);
  RUN_TEST(test_drift_boundary_is_inclusive);
  RUN_TEST(test_spread_boundary_is_inclusive);
  RUN_TEST(test_elapsed_seconds_come_from_the_caller);
  RUN_TEST(test_long_settling_keeps_trying_then_accepts);
  RUN_TEST(test_settle_timeout_refuses_with_minutes);
  RUN_TEST(test_unstable_at_timeout);
  RUN_TEST(test_feed_after_finish_is_ignored);
  RUN_TEST(test_cancel_refuses_cancelled);
  RUN_TEST(test_zero_median_is_signal_lost);
  RUN_TEST(test_every_picker_nominal_has_buffer_rows);
  RUN_TEST(test_capture_tokens_cover_every_enumerator);
  return UNITY_END();
}
