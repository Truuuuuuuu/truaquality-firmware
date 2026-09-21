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
  const char *FIRMWARE_VERSION = "0.5.0";

  // 2023-11-14T22:13:20Z — the fixture's first timestamp. The batch vector adds 60 s per sample.
  const std::time_t T0 = 1700000000;
}

void test_fixture_is_the_expected_one(void)
{
  // A corrupted or half-generated signing_vectors.h would otherwise let every case below pass vacuously.
  TEST_ASSERT_EQUAL_UINT(7, GOLDEN_VECTOR_COUNT);
  TEST_ASSERT_EQUAL_STRING("b1a776ca532c46a3bc2538ee73d5d18aea3c0dbd85a126fdc682bc648b3c4a80",
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
  // Every SensorSample literal in this file spells out all of its fields. An omitted second initializer is
  // not "unset": it value-initializes turbidity to 0.0f, which serializes as a real, plausible "crystal
  // clear water" reading instead of being omitted the way a missing sensor must be.
  wire::Stamped batch[] = {{T0, {27.5f, NAN}}};
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[0].body,
                           wire::buildBody(FIRMWARE_VERSION, nullptr, batch, 1).c_str());
}

void test_multi_sample_batch_keeps_distinct_timestamps(void)
{
  // Regression guard: buildBody reuses one char timestamp[25] across the loop. ArduinoJson copies a
  // non-const char[] but stores a const char* by reference, so weakening that buffer's type would give all
  // three samples the last timestamp — and this is the only case that would notice.
  wire::Stamped batch[] = {{T0, {27.5f, NAN}}, {T0 + 60, {26.25f, NAN}}, {T0 + 120, {28.0f, NAN}}};
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[1].body,
                           wire::buildBody(FIRMWARE_VERSION, nullptr, batch, 3).c_str());
}

void test_second_device_body(void)
{
  wire::Stamped batch[] = {{T0, {26.25f, NAN}}};
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[2].body,
                           wire::buildBody(FIRMWARE_VERSION, nullptr, batch, 1).c_str());
}

void test_temperature_and_turbidity_body(void)
{
  // Catches a swapped JSON key order. The keys are emitted in addValue call order, so putting turbidity
  // first would still produce valid JSON with the same numbers — and a different signature for every batch
  // the unit ever publishes, i.e. a fleet the backend rejects wholesale.
  wire::Stamped batch[] = {{T0, {27.5f, 12.3f}}};
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[3].body,
                           wire::buildBody(FIRMWARE_VERSION, nullptr, batch, 1).c_str());
}

void test_turbidity_only_body(void)
{
  // The DS18B20 unplugged on a calibrated unit. Catches a temperature NAN being emitted as null or 0 when
  // it is the only missing value: the temperature key has to be absent, not present-and-wrong.
  wire::Stamped batch[] = {{T0, {NAN, 250.5f}}};
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[4].body,
                           wire::buildBody(FIRMWARE_VERSION, nullptr, batch, 1).c_str());
}

void test_nan_turbidity_is_omitted_mid_batch(void)
{
  // The case SENS-03/SENS-04 turn on: a faulted or uncalibrated turbidity sensor must vanish from the upload
  // while temperature keeps reporting, sample by sample within one batch. A 0.0f leaking in here would be
  // indistinguishable from a genuine clear-water reading once it is in the database.
  wire::Stamped batch[] = {{T0, {27.5f, 12.3f}}, {T0 + 60, {26.25f, NAN}}};
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[5].body,
                           wire::buildBody(FIRMWARE_VERSION, nullptr, batch, 2).c_str());
}

void test_awkward_ssid_body(void)
{
  // Quote, backslash and a non-ASCII character in one SSID: ArduinoJson must escape and pass through exactly
  // what the backend's JSON.stringify does, or the signature differs by a byte nobody would think to check.
  wire::Stamped batch[] = {{T0, {27.5f, NAN}}};
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[6].body,
                           wire::buildBody(FIRMWARE_VERSION, "Bahay \"Kubo\" \\ Caf\xC3\xA9", batch, 1).c_str());
}

void test_empty_or_missing_ssid_is_omitted(void)
{
  wire::Stamped batch[] = {{T0, {27.5f, NAN}}};
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
  const uint8_t mac[32] = {0xb1, 0xa7, 0x76, 0xca, 0x53, 0x2c, 0x46, 0xa3,
                           0xbc, 0x25, 0x38, 0xee, 0x73, 0xd5, 0xd1, 0x8a,
                           0xea, 0x3c, 0x0d, 0xbd, 0x85, 0xa1, 0x26, 0xfd,
                           0xc6, 0x82, 0xbc, 0x64, 0x8b, 0x3c, 0x4a, 0x80};
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
  wire::Stamped batch[] = {{T0, {NAN, NAN}}};
  TEST_ASSERT_EQUAL_STRING(
      R"RAW({"firmwareVersion":"0.5.0","samples":[{"recordedAt":"2023-11-14T22:13:20Z","values":{}}]})RAW",
      wire::buildBody(FIRMWARE_VERSION, nullptr, batch, 1).c_str());
}

void test_infinite_parameter_is_omitted_not_null(void)
{
  // ARDUINOJSON_ENABLE_INFINITY is 0 too, so +/-infinity would serialize as null exactly like NAN.
  wire::Stamped batch[] = {{T0, {INFINITY, -INFINITY}}};
  TEST_ASSERT_EQUAL_STRING(
      R"RAW({"firmwareVersion":"0.5.0","samples":[{"recordedAt":"2023-11-14T22:13:20Z","values":{}}]})RAW",
      wire::buildBody(FIRMWARE_VERSION, nullptr, batch, 1).c_str());
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
  return UNITY_END();
}
