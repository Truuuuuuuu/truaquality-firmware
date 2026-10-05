#include <unity.h>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>

#include "WireFormat.h"

#include "../golden/signing_vectors.h"

// Byte-for-byte parity proof against the backend's golden vectors, run on the host in about a second. The
// backend (src/lib/deviceMessages.ts) is the authority: if a case here fails, the firmware is wrong — never
// regenerate the vectors to make this suite green. The HMAC itself is not exercised here because mbedTLS has
// no host build; feeding a vector's own signature into wire::frame() still proves the full published payload
// byte-for-byte, which is what test_frame_matches_vector_payload does.
namespace
{
  const char *FIRMWARE_VERSION = "0.6.0";
  // The pH vectors (8-10) are signed at 0.7.0, the first firmware that will emit pH. Like FIRMWARE_VERSION it
  // matches the vectors' bodies, not src/main.cpp, which stays at 0.6.1 until the Phase 10 driver lands.
  const char *FIRMWARE_VERSION_PH = "0.7.0";

  // 2023-11-14T22:13:20Z — the fixture's first timestamp. The batch vector adds 60 s per sample.
  const std::time_t T0 = 1700000000;
}

void test_fixture_is_the_expected_one(void)
{
  // A corrupted or half-generated signing_vectors.h would otherwise let every case below pass vacuously.
  TEST_ASSERT_EQUAL_UINT(11, GOLDEN_VECTOR_COUNT);
  // The pH vectors are appended to the backend fixture, never inserted, so the indices below are stable.
  TEST_ASSERT_EQUAL_STRING("temperature-turbidity-and-ph", GOLDEN_VECTORS[8].name);
  TEST_ASSERT_EQUAL_STRING("ph-batch-with-omissions", GOLDEN_VECTORS[9].name);
  TEST_ASSERT_EQUAL_STRING("diagnostics-and-sensor-status-with-ph", GOLDEN_VECTORS[10].name);
  TEST_ASSERT_EQUAL_STRING("b537560d7b8e2fd2f7ff0f52b9b86ce9ee0f413f9db798ade49d7fedd395b491",
                           GOLDEN_VECTORS[0].signature);
}

void test_topic_matches_backend(void)
{
  for (unsigned i = 0; i < GOLDEN_VECTOR_COUNT; i++)
  {
    TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[i].topic,
                             wire::readingsTopic(GOLDEN_VECTORS[i].deviceId).c_str());
  }
}

void test_single_sample_body(void)
{
  // Every SensorSample literal in this file spells out all six fields. An omitted second initializer is
  // not "unset": it value-initializes turbidity to 0.0f, which serializes as a real, plausible "crystal
  // clear water" reading instead of being omitted the way a missing sensor must be — and an omitted status
  // value-initializes to Ok. The pre-pH vectors carry ph NAN + PhStatus::NotFitted, which is exactly what
  // today's readAll() produces, so these cases also prove a pH-less unit's bytes did not move. The statuses don't reach these bodies (the 4-arg overload emits none) but are
  // kept honest anyway: NotFound/NoSignal wherever the value is missing.
  wire::Stamped batch[] = {{T0, {27.5f, NAN, NAN, TemperatureStatus::Ok, TurbidityStatus::NoSignal, PhStatus::NotFitted}}};
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[0].body,
                           wire::buildBody(FIRMWARE_VERSION, nullptr, batch, 1).c_str());
}

void test_multi_sample_batch_keeps_distinct_timestamps(void)
{
  // Regression guard: buildBody reuses one char timestamp[25] across the loop. ArduinoJson copies a
  // non-const char[] but stores a const char* by reference, so weakening that buffer's type would give all
  // three samples the last timestamp — and this is the only case that would notice.
  wire::Stamped batch[] = {
      {T0, {27.5f, NAN, NAN, TemperatureStatus::Ok, TurbidityStatus::NoSignal, PhStatus::NotFitted}},
      {T0 + 60, {26.25f, NAN, NAN, TemperatureStatus::Ok, TurbidityStatus::NoSignal, PhStatus::NotFitted}},
      {T0 + 120, {28.0f, NAN, NAN, TemperatureStatus::Ok, TurbidityStatus::NoSignal, PhStatus::NotFitted}},
  };
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[1].body,
                           wire::buildBody(FIRMWARE_VERSION, nullptr, batch, 3).c_str());
}

void test_second_device_body(void)
{
  wire::Stamped batch[] = {{T0, {26.25f, NAN, NAN, TemperatureStatus::Ok, TurbidityStatus::NoSignal, PhStatus::NotFitted}}};
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[2].body,
                           wire::buildBody(FIRMWARE_VERSION, nullptr, batch, 1).c_str());
}

void test_temperature_and_turbidity_body(void)
{
  // Catches a swapped JSON key order. The keys are emitted in addValue call order, so putting turbidity
  // first would still produce valid JSON with the same numbers — and a different signature for every batch
  // the unit ever publishes, i.e. a fleet the backend rejects wholesale.
  wire::Stamped batch[] = {{T0, {27.5f, 12.3f, NAN, TemperatureStatus::Ok, TurbidityStatus::Ok, PhStatus::NotFitted}}};
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[3].body,
                           wire::buildBody(FIRMWARE_VERSION, nullptr, batch, 1).c_str());
}

void test_turbidity_only_body(void)
{
  // The DS18B20 unplugged on a calibrated unit. Catches a temperature NAN being emitted as null or 0 when
  // it is the only missing value: the temperature key has to be absent, not present-and-wrong.
  wire::Stamped batch[] = {{T0, {NAN, 250.5f, NAN, TemperatureStatus::NotFound, TurbidityStatus::Ok, PhStatus::NotFitted}}};
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[4].body,
                           wire::buildBody(FIRMWARE_VERSION, nullptr, batch, 1).c_str());
}

void test_nan_turbidity_is_omitted_mid_batch(void)
{
  // The case SENS-03/SENS-04 turn on: a faulted or uncalibrated turbidity sensor must vanish from the upload
  // while temperature keeps reporting, sample by sample within one batch. A 0.0f leaking in here would be
  // indistinguishable from a genuine clear-water reading once it is in the database.
  wire::Stamped batch[] = {
      {T0, {27.5f, 12.3f, NAN, TemperatureStatus::Ok, TurbidityStatus::Ok, PhStatus::NotFitted}},
      {T0 + 60, {26.25f, NAN, NAN, TemperatureStatus::Ok, TurbidityStatus::NoSignal, PhStatus::NotFitted}},
  };
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[5].body,
                           wire::buildBody(FIRMWARE_VERSION, nullptr, batch, 2).c_str());
}

void test_awkward_ssid_body(void)
{
  // Quote, backslash and a non-ASCII character in one SSID: ArduinoJson must escape and pass through exactly
  // what the backend's JSON.stringify does, or the signature differs by a byte nobody would think to check.
  wire::Stamped batch[] = {{T0, {27.5f, NAN, NAN, TemperatureStatus::Ok, TurbidityStatus::NoSignal, PhStatus::NotFitted}}};
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[6].body,
                           wire::buildBody(FIRMWARE_VERSION, "Bahay \"Kubo\" \\ Caf\xC3\xA9", batch, 1).c_str());
}

void test_empty_or_missing_ssid_is_omitted(void)
{
  wire::Stamped batch[] = {{T0, {27.5f, NAN, NAN, TemperatureStatus::Ok, TurbidityStatus::NoSignal, PhStatus::NotFitted}}};
  std::string missing = wire::buildBody(FIRMWARE_VERSION, nullptr, batch, 1);
  std::string empty = wire::buildBody(FIRMWARE_VERSION, "", batch, 1);
  TEST_ASSERT_NULL(strstr(missing.c_str(), "wifiSsid"));
  TEST_ASSERT_EQUAL_STRING(missing.c_str(), empty.c_str());
}

void test_signed_input_joins_with_one_newline(void)
{
  for (unsigned i = 0; i < GOLDEN_VECTOR_COUNT; i++)
  {
    const GoldenVector &v = GOLDEN_VECTORS[i];
    std::string expected = std::string(v.topic) + "\n" + v.body;
    TEST_ASSERT_EQUAL_STRING(expected.c_str(), wire::signedInput(v.topic, v.body).c_str());
  }
}

void test_hex_is_lowercase(void)
{
  // Characterization of the backend, not an endorsement: Phase 1 pinned that it accepts uppercase hex too
  // (Buffer.from is case-insensitive). The firmware must still emit lowercase, because the vectors are
  // lowercase and a case flip here would be invisible until some other verifier stopped tolerating it.
  // These are GOLDEN_VECTORS[0].signature's own bytes, written out by hand so the assertion compares an
  // independent byte array against the fixture's hex rather than re-deriving one from the other. They move
  // with every regeneration that changes vector 0 — which a FIRMWARE_VERSION bump always does, because the
  // version string is inside the signed body.
  const uint8_t mac[32] = {0xb5, 0x37, 0x56, 0x0d, 0x7b, 0x8e, 0x2f, 0xd2,
                           0xf7, 0xff, 0x0f, 0x52, 0xb9, 0xb8, 0x6c, 0xe9,
                           0xee, 0x0f, 0x41, 0x3f, 0x9d, 0xb7, 0x98, 0xad,
                           0xe4, 0x9d, 0x7f, 0xed, 0xd3, 0x95, 0xb4, 0x91};
  char hex[65];
  wire::toHexLower(mac, sizeof(mac), hex);
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[0].signature, hex);
  TEST_ASSERT_EQUAL_UINT(64, strlen(hex));
}

void test_frame_matches_vector_payload(void)
{
  for (unsigned i = 0; i < GOLDEN_VECTOR_COUNT; i++)
  {
    const GoldenVector &v = GOLDEN_VECTORS[i];
    TEST_ASSERT_EQUAL_STRING(v.payload, wire::frame(v.signature, v.body).c_str());
  }
}

void test_nan_parameter_is_omitted_not_null(void)
{
  // ARDUINOJSON_ENABLE_NAN is 0, so a NAN that got past the guard would serialize as null — which the
  // backend accepts and silently drops. The failure would be invisible in production; it has to be caught
  // here instead.
  wire::Stamped batch[] = {{T0, {NAN, NAN, NAN, TemperatureStatus::NotFound, TurbidityStatus::NoSignal, PhStatus::NotFitted}}};
  TEST_ASSERT_EQUAL_STRING(
      R"RAW({"firmwareVersion":"0.6.0","samples":[{"recordedAt":"2023-11-14T22:13:20Z","values":{}}]})RAW",
      wire::buildBody(FIRMWARE_VERSION, nullptr, batch, 1).c_str());
}

void test_infinite_parameter_is_omitted_not_null(void)
{
  // ARDUINOJSON_ENABLE_INFINITY is 0 too, so +/-infinity would serialize as null exactly like NAN.
  wire::Stamped batch[] = {{T0, {INFINITY, -INFINITY, NAN, TemperatureStatus::NotFound, TurbidityStatus::NoSignal, PhStatus::NotFitted}}};
  TEST_ASSERT_EQUAL_STRING(
      R"RAW({"firmwareVersion":"0.6.0","samples":[{"recordedAt":"2023-11-14T22:13:20Z","values":{}}]})RAW",
      wire::buildBody(FIRMWARE_VERSION, nullptr, batch, 1).c_str());
}

void test_diagnostics_and_sensor_status_body(void)
{
  // The 0.6.0 shape, byte for byte: diag after wifiSsid, sensors after diag, and turbidity's value omitted
  // because its status isn't ok.
  const wire::Diagnostics diag{-67, 86400, wire::ResetReason::PowerOn, 201344, 3};
  wire::Stamped batch[] = {{T0, {27.5f, NAN, NAN, TemperatureStatus::Ok, TurbidityStatus::NoSignal, PhStatus::NotFitted}}};
  TEST_ASSERT_EQUAL_STRING("diagnostics-and-sensor-status", GOLDEN_VECTORS[7].name);
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[7].body,
                           wire::buildBody(FIRMWARE_VERSION, "BFAR-Pond-1", &diag, true, batch, 1).c_str());
}

void test_sensor_status_comes_from_the_newest_sample(void)
{
  // The backend stores the status "at the newest sample in the batch". Taking the first sample would report
  // a probe that was re-plugged mid-batch as still missing, and SENSOR_RECOVERED would fire a batch late.
  wire::Stamped batch[] = {
      {T0, {NAN, NAN, NAN, TemperatureStatus::NotFound, TurbidityStatus::Uncalibrated, PhStatus::NotFitted}},
      {T0 + 60, {27.5f, NAN, NAN, TemperatureStatus::Ok, TurbidityStatus::OverRange, PhStatus::NotFitted}},
  };
  std::string body = wire::buildBody(FIRMWARE_VERSION, nullptr, nullptr, true, batch, 2);
  TEST_ASSERT_NOT_NULL(strstr(body.c_str(), R"RAW("sensors":{"temperature":"ok","turbidity":"over_range"})RAW"));
  TEST_ASSERT_NULL(strstr(body.c_str(), "not_found"));
  TEST_ASSERT_NULL(strstr(body.c_str(), "uncalibrated"));
}

void test_diag_and_sensors_are_independently_optional(void)
{
  const wire::Diagnostics diag{-50, 10, wire::ResetReason::Software, 1000, 1};
  wire::Stamped batch[] = {{T0, {27.5f, NAN, NAN, TemperatureStatus::Ok, TurbidityStatus::NoSignal, PhStatus::NotFitted}}};

  std::string diagOnly = wire::buildBody(FIRMWARE_VERSION, nullptr, &diag, false, batch, 1);
  TEST_ASSERT_NOT_NULL(strstr(diagOnly.c_str(), "\"diag\":"));
  TEST_ASSERT_NULL(strstr(diagOnly.c_str(), "\"sensors\":"));

  std::string sensorsOnly = wire::buildBody(FIRMWARE_VERSION, nullptr, nullptr, true, batch, 1);
  TEST_ASSERT_NULL(strstr(sensorsOnly.c_str(), "\"diag\":"));
  TEST_ASSERT_NOT_NULL(strstr(sensorsOnly.c_str(), "\"sensors\":"));

  // No samples means no newest sample to take a status from, so no sensors object at all.
  std::string empty = wire::buildBody(FIRMWARE_VERSION, nullptr, nullptr, true, batch, 0);
  TEST_ASSERT_NULL(strstr(empty.c_str(), "\"sensors\":"));

  // Key order is signed bytes: wifiSsid < diag < sensors < samples.
  std::string both = wire::buildBody(FIRMWARE_VERSION, "Pond", &diag, true, batch, 1);
  const char *ssid = strstr(both.c_str(), "\"wifiSsid\":");
  const char *d = strstr(both.c_str(), "\"diag\":");
  const char *sn = strstr(both.c_str(), "\"sensors\":");
  const char *sm = strstr(both.c_str(), "\"samples\":");
  TEST_ASSERT_NOT_NULL(ssid);
  TEST_ASSERT_NOT_NULL(d);
  TEST_ASSERT_NOT_NULL(sn);
  TEST_ASSERT_NOT_NULL(sm);
  TEST_ASSERT_TRUE(ssid < d);
  TEST_ASSERT_TRUE(d < sn);
  TEST_ASSERT_TRUE(sn < sm);
}

namespace
{
  std::string diagBody(int64_t rssi, int64_t uptimeS, int64_t freeHeap, int64_t queued)
  {
    const wire::Diagnostics diag{rssi, uptimeS, wire::ResetReason::Unknown, freeHeap, queued};
    wire::Stamped batch[] = {{T0, {27.5f, NAN, NAN, TemperatureStatus::Ok, TurbidityStatus::NoSignal, PhStatus::NotFitted}}};
    return wire::buildBody(FIRMWARE_VERSION, nullptr, &diag, false, batch, 1);
  }
}

void test_diag_numbers_are_clamped_to_the_backend_ranges(void)
{
  // The backend rejects the WHOLE message for one out-of-range diag number, and the batch's readings go with
  // it — so a wild RSSI or a queue count past the cap must be clamped here, never sent as-is.
  TEST_ASSERT_NOT_NULL(strstr(diagBody(5, 1, 1, 1).c_str(), "\"rssi\":0,"));
  TEST_ASSERT_NOT_NULL(strstr(diagBody(-200, 1, 1, 1).c_str(), "\"rssi\":-127,"));
  TEST_ASSERT_NOT_NULL(strstr(diagBody(-60, 1, 1, 500).c_str(), "\"queued\":120}"));
  TEST_ASSERT_NOT_NULL(strstr(diagBody(-60, 1, 1, -1).c_str(), "\"queued\":0}"));
  TEST_ASSERT_NOT_NULL(strstr(diagBody(-60, -5, 1, 1).c_str(), "\"uptimeS\":0,"));
  TEST_ASSERT_NOT_NULL(strstr(diagBody(-60, 5000000000LL, 1, 1).c_str(), "\"uptimeS\":2147483647,"));
  TEST_ASSERT_NOT_NULL(strstr(diagBody(-60, 1, -5, 1).c_str(), "\"freeHeap\":0,"));
  TEST_ASSERT_NOT_NULL(strstr(diagBody(-60, 1, 5000000000LL, 1).c_str(), "\"freeHeap\":2147483647,"));
  // In-range values pass through untouched.
  TEST_ASSERT_NOT_NULL(
      strstr(diagBody(-60, 42, 123456, 7).c_str(),
             R"RAW("diag":{"rssi":-60,"uptimeS":42,"resetReason":"unknown","freeHeap":123456,"queued":7})RAW"));
}

void test_reset_reason_tokens_match_backend(void)
{
  // Expected strings are the backend's RESET_REASONS (backend/src/schemas/ingest.ts), written out by hand so
  // a typo on either side shows up here instead of as a rejected message.
  TEST_ASSERT_EQUAL_STRING("power_on", wire::resetReasonToken(wire::ResetReason::PowerOn));
  TEST_ASSERT_EQUAL_STRING("software", wire::resetReasonToken(wire::ResetReason::Software));
  TEST_ASSERT_EQUAL_STRING("panic", wire::resetReasonToken(wire::ResetReason::Panic));
  TEST_ASSERT_EQUAL_STRING("int_wdt", wire::resetReasonToken(wire::ResetReason::IntWdt));
  TEST_ASSERT_EQUAL_STRING("task_wdt", wire::resetReasonToken(wire::ResetReason::TaskWdt));
  TEST_ASSERT_EQUAL_STRING("wdt", wire::resetReasonToken(wire::ResetReason::Wdt));
  TEST_ASSERT_EQUAL_STRING("brownout", wire::resetReasonToken(wire::ResetReason::Brownout));
  TEST_ASSERT_EQUAL_STRING("deep_sleep", wire::resetReasonToken(wire::ResetReason::DeepSleep));
  TEST_ASSERT_EQUAL_STRING("external", wire::resetReasonToken(wire::ResetReason::External));
  TEST_ASSERT_EQUAL_STRING("unknown", wire::resetReasonToken(wire::ResetReason::Unknown));
}

void test_sensor_status_tokens_match_backend(void)
{
  // The backend's SENSOR_STATUSES (backend/src/schemas/ingest.ts).
  TEST_ASSERT_EQUAL_STRING("ok", sensors::statusToken(TemperatureStatus::Ok));
  TEST_ASSERT_EQUAL_STRING("not_found", sensors::statusToken(TemperatureStatus::NotFound));
  TEST_ASSERT_EQUAL_STRING("disconnected", sensors::statusToken(TemperatureStatus::Disconnected));
  TEST_ASSERT_EQUAL_STRING("power_on_value", sensors::statusToken(TemperatureStatus::PowerOnValue));
  TEST_ASSERT_EQUAL_STRING("ok", sensors::statusToken(TurbidityStatus::Ok));
  TEST_ASSERT_EQUAL_STRING("no_signal", sensors::statusToken(TurbidityStatus::NoSignal));
  TEST_ASSERT_EQUAL_STRING("uncalibrated", sensors::statusToken(TurbidityStatus::Uncalibrated));
  TEST_ASSERT_EQUAL_STRING("over_range", sensors::statusToken(TurbidityStatus::OverRange));
}

void test_temperature_turbidity_and_ph_body(void)
{
  // Key order temperature, turbidity, ph is signed bytes: a ph emitted anywhere else is valid JSON with the
  // same numbers and a signature the backend rejects.
  wire::Stamped batch[] = {{T0, {27.5f, 12.3f, 7.1f, TemperatureStatus::Ok, TurbidityStatus::Ok, PhStatus::Ok}}};
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[8].body,
                           wire::buildBody(FIRMWARE_VERSION_PH, nullptr, batch, 1).c_str());
}

void test_ph_batch_with_omissions_body(void)
{
  // pH near both alert edges, then turbidity dropping out while pH stays, then pH dropping out as well:
  // each missing value must vanish per sample, never leak as 0 or null into its neighbours.
  wire::Stamped batch[] = {
      {T0, {27.5f, 12.3f, 6.49f, TemperatureStatus::Ok, TurbidityStatus::Ok, PhStatus::Ok}},
      {T0 + 60, {27.4f, NAN, 9.51f, TemperatureStatus::Ok, TurbidityStatus::NoSignal, PhStatus::Ok}},
      {T0 + 120, {27.3f, NAN, NAN, TemperatureStatus::Ok, TurbidityStatus::NoSignal, PhStatus::NoSignal}},
  };
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[9].body,
                           wire::buildBody(FIRMWARE_VERSION_PH, nullptr, batch, 3).c_str());
}

void test_diagnostics_and_sensor_status_with_ph_body(void)
{
  // The vector-7 shape with a fitted but faulted pH: sensors.ph comes after turbidity and the pH value is
  // omitted because its status isn't ok.
  const wire::Diagnostics diag{-67, 86400, wire::ResetReason::PowerOn, 201344, 3};
  wire::Stamped batch[] = {
      {T0, {27.5f, 12.3f, NAN, TemperatureStatus::Ok, TurbidityStatus::Ok, PhStatus::NoSignal}}};
  TEST_ASSERT_EQUAL_STRING("diagnostics-and-sensor-status-with-ph", GOLDEN_VECTORS[10].name);
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[10].body,
                           wire::buildBody(FIRMWARE_VERSION_PH, "BFAR-Pond-1", &diag, true, batch, 1).c_str());
}

void test_ph_status_tokens(void)
{
  // Only tokens already in the backend's SENSOR_STATUSES: a new one would reject the whole message on any
  // backend that predates it.
  TEST_ASSERT_EQUAL_STRING("ok", sensors::statusToken(PhStatus::Ok));
  TEST_ASSERT_EQUAL_STRING("no_signal", sensors::statusToken(PhStatus::NoSignal));
  TEST_ASSERT_EQUAL_STRING("uncalibrated", sensors::statusToken(PhStatus::Uncalibrated));
  TEST_ASSERT_EQUAL_STRING("over_range", sensors::statusToken(PhStatus::OverRange));
  // NotFitted is never sent, but if it ever were it must not claim a healthy sensor.
  TEST_ASSERT_TRUE(strcmp("ok", sensors::statusToken(PhStatus::NotFitted)) != 0);
}

void test_not_fitted_ph_never_appears(void)
{
  // What every Phase 9 unit produces: no pH front end, so neither a ph value nor a sensors.ph key — a pH-less
  // unit sending "ph":0 would chart a strongly acidic pond that does not exist.
  const wire::Diagnostics diag{-67, 86400, wire::ResetReason::PowerOn, 201344, 3};
  wire::Stamped batch[] = {
      {T0, {27.5f, 12.3f, NAN, TemperatureStatus::Ok, TurbidityStatus::Ok, PhStatus::NotFitted}}};
  std::string plain = wire::buildBody(FIRMWARE_VERSION, nullptr, batch, 1);
  std::string full = wire::buildBody(FIRMWARE_VERSION, "BFAR-Pond-1", &diag, true, batch, 1);
  TEST_ASSERT_NULL(strstr(plain.c_str(), "\"ph\""));
  TEST_ASSERT_NULL(strstr(full.c_str(), "\"ph\""));
  TEST_ASSERT_NOT_NULL(strstr(full.c_str(), R"RAW("sensors":{"temperature":"ok","turbidity":"ok"})RAW"));
}

void test_non_ok_ph_status_never_sends_a_finite_ph(void)
{
  // WR-03: the ph value is gated on PhStatus::Ok, not only on isfinite. A finite ph left behind with any other
  // status (a Phase 10 driver slip, a partial init) must not be signed: the sensors map would say the probe is
  // missing or faulted while the body carried a pH the backend would store and alert on.
  const wire::Diagnostics diag{-67, 86400, wire::ResetReason::PowerOn, 201344, 3};
  const PhStatus statuses[] = {PhStatus::NotFitted, PhStatus::NoSignal, PhStatus::Uncalibrated, PhStatus::OverRange};
  for (PhStatus status : statuses)
  {
    wire::Stamped batch[] = {{T0, {27.5f, 12.3f, 7.1f, TemperatureStatus::Ok, TurbidityStatus::Ok, status}}};
    std::string plain = wire::buildBody(FIRMWARE_VERSION_PH, nullptr, batch, 1);
    std::string full = wire::buildBody(FIRMWARE_VERSION_PH, "BFAR-Pond-1", &diag, true, batch, 1);
    TEST_ASSERT_NULL_MESSAGE(strstr(plain.c_str(), "\"ph\":7"), sensors::statusToken(status));
    TEST_ASSERT_NULL_MESSAGE(strstr(full.c_str(), "\"ph\":7"), sensors::statusToken(status));
    TEST_ASSERT_NOT_NULL(strstr(plain.c_str(), R"RAW("values":{"temperature":27.5,"turbidity":12.3}})RAW"));
    if (status == PhStatus::NotFitted)
    {
      TEST_ASSERT_NULL(strstr(full.c_str(), "\"ph\""));
    }
    else
    {
      // A fault keeps its sensors.ph token; only the value goes.
      TEST_ASSERT_NOT_NULL(strstr(full.c_str(), "\"ph\":\""));
    }
  }
}

int main(int argc, char **argv)
{
  (void)argc;
  (void)argv;
  UNITY_BEGIN();
  RUN_TEST(test_fixture_is_the_expected_one);
  RUN_TEST(test_topic_matches_backend);
  RUN_TEST(test_single_sample_body);
  RUN_TEST(test_multi_sample_batch_keeps_distinct_timestamps);
  RUN_TEST(test_second_device_body);
  RUN_TEST(test_temperature_and_turbidity_body);
  RUN_TEST(test_turbidity_only_body);
  RUN_TEST(test_nan_turbidity_is_omitted_mid_batch);
  RUN_TEST(test_awkward_ssid_body);
  RUN_TEST(test_empty_or_missing_ssid_is_omitted);
  RUN_TEST(test_signed_input_joins_with_one_newline);
  RUN_TEST(test_hex_is_lowercase);
  RUN_TEST(test_frame_matches_vector_payload);
  RUN_TEST(test_nan_parameter_is_omitted_not_null);
  RUN_TEST(test_infinite_parameter_is_omitted_not_null);
  RUN_TEST(test_diagnostics_and_sensor_status_body);
  RUN_TEST(test_sensor_status_comes_from_the_newest_sample);
  RUN_TEST(test_diag_and_sensors_are_independently_optional);
  RUN_TEST(test_diag_numbers_are_clamped_to_the_backend_ranges);
  RUN_TEST(test_reset_reason_tokens_match_backend);
  RUN_TEST(test_sensor_status_tokens_match_backend);
  RUN_TEST(test_temperature_turbidity_and_ph_body);
  RUN_TEST(test_ph_batch_with_omissions_body);
  RUN_TEST(test_diagnostics_and_sensor_status_with_ph_body);
  RUN_TEST(test_ph_status_tokens);
  RUN_TEST(test_not_fitted_ph_never_appears);
  RUN_TEST(test_non_ok_ph_status_never_sends_a_finite_ph);
  return UNITY_END();
}
