#include <unity.h>
#include "PhMath.h"

void setUp() {}
void tearDown() {}

static const ph::Calibration CAL = {1727, 401, 1763, 916, 2500, 0, ph::PH_CAL_VERSION};

static ph::Burst burst(float sensorMv, float spread = 1.0f)
{
  const float pinMv = sensorMv * ph::DIVIDER_RATIO;
  return ph::Burst{pinMv, pinMv, spread};
}

static void test_anchor_at_all_temperatures()
{
  for (float temp : {0.0f, 25.0f, 35.0f, 60.0f, NAN, INFINITY})
  {
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.5f, ph::phFromSensorMv(1819.0f, ph::fit(CAL), temp));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.5f, ph::phFromBurst(burst(1819.0f), CAL, temp));
  }
}

static void test_one_saved_slope_changes_demo_ph_by_one_tenth()
{
  const ph::Line line = ph::fit(CAL);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.6f, ph::phFromSensorMv(1819.0f + line.slopeMvPerPh, line, 25.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.4f, ph::phFromSensorMv(1819.0f - line.slopeMvPerPh, line, 25.0f));
}

static void test_ten_saved_slopes_change_demo_ph_by_one()
{
  const ph::Line line = ph::fit(CAL);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 8.5f, ph::phFromSensorMv(1819.0f + 10 * line.slopeMvPerPh, line, 25.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 6.5f, ph::phFromSensorMv(1819.0f - 10 * line.slopeMvPerPh, line, 25.0f));
}

static void test_temperature_compensation_and_fallback_keep_reduced_sensitivity()
{
  const ph::Line line = ph::fit(CAL);
  const float warmStep = line.slopeMvPerPh * (308.15f / 298.15f);
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.6f, ph::phFromSensorMv(1819.0f + warmStep, line, 35.0f));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.6f, ph::phFromSensorMv(1819.0f + line.slopeMvPerPh, line, NAN));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.4f, ph::phFromSensorMv(1819.0f - line.slopeMvPerPh, line, INFINITY));
}

static void test_179x_sensor_mv_demo_values()
{
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.09f, ph::phFromBurst(burst(1790.0f), CAL, 25));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.21f, ph::phFromBurst(burst(1799.0f), CAL, 25));
}

static void test_faults_still_suppress_values()
{
  ph::Calibration invalid = CAL;
  invalid.version = 0;
  TEST_ASSERT_TRUE(std::isnan(ph::phFromBurst(burst(1819), invalid, 25)));
  TEST_ASSERT_EQUAL(static_cast<int>(ph::Status::Uncalibrated), static_cast<int>(ph::classify(burst(1819), invalid, 25)));
  TEST_ASSERT_TRUE(std::isnan(ph::phFromBurst(burst(0), CAL, 25)));
  TEST_ASSERT_TRUE(std::isnan(ph::phFromBurst(burst(1819, ph::MAX_BURST_SPREAD_PIN_MV + 1), CAL, 25)));
  TEST_ASSERT_TRUE(std::isnan(ph::phFromBurst(burst(NAN), CAL, 25)));
}

static void test_range_rejection_and_rounding()
{
  const float slope = 10 * ph::fit(CAL).slopeMvPerPh;
  // An exactly representable slope keeps float reconstruction of the boundary from landing just outside it.
  const ph::Calibration boundaryCal = {1700, 400, 1750, 900, 2500, 0, ph::PH_CAL_VERSION};
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 2.0f, ph::phFromBurst(burst(1269), boundaryCal, 25));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 12.0f, ph::phFromBurst(burst(2269), boundaryCal, 25));
  TEST_ASSERT_TRUE(std::isnan(ph::phFromBurst(burst(1819 + 5 * slope), CAL, 25)));
  TEST_ASSERT_TRUE(std::isnan(ph::phFromBurst(burst(1819 - 6 * slope), CAL, 25)));
  TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.62f, ph::phFromBurst(burst(1819 + 0.123f * slope), CAL, 25));
}

int main()
{
  UNITY_BEGIN();
  RUN_TEST(test_anchor_at_all_temperatures);
  RUN_TEST(test_one_saved_slope_changes_demo_ph_by_one_tenth);
  RUN_TEST(test_ten_saved_slopes_change_demo_ph_by_one);
  RUN_TEST(test_temperature_compensation_and_fallback_keep_reduced_sensitivity);
  RUN_TEST(test_179x_sensor_mv_demo_values);
  RUN_TEST(test_faults_still_suppress_values);
  RUN_TEST(test_range_rejection_and_rounding);
  return UNITY_END();
}
