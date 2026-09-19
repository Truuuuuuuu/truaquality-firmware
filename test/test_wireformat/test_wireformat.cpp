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
  const char *FIRMWARE_VERSION = "0.3.0";

  // 2023-11-14T22:13:20Z — the fixture's first timestamp. The batch vector adds 60 s per sample.
  const std::time_t T0 = 1700000000;
}

void test_fixture_is_the_expected_one(void)
{
  // A corrupted or half-generated signing_vectors.h would otherwise let every case below pass vacuously.
  TEST_ASSERT_EQUAL_UINT(3, GOLDEN_VECTOR_COUNT);
  TEST_ASSERT_EQUAL_STRING("cbc82daad41ad84ec1896d9ec8885b42cfdade4b738922b31082f08db0cc7bf5",
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
  wire::Stamped batch[] = {{T0, {27.5f}}};
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[0].body,
                           wire::buildBody(FIRMWARE_VERSION, batch, 1).c_str());
}

void test_multi_sample_batch_keeps_distinct_timestamps(void)
{
  // Regression guard: buildBody reuses one char timestamp[25] across the loop. ArduinoJson copies a
  // non-const char[] but stores a const char* by reference, so weakening that buffer's type would give all
  // three samples the last timestamp — and this is the only case that would notice.
  wire::Stamped batch[] = {{T0, {27.5f}}, {T0 + 60, {26.25f}}, {T0 + 120, {28.0f}}};
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[1].body,
                           wire::buildBody(FIRMWARE_VERSION, batch, 3).c_str());
}

void test_second_device_body(void)
{
  wire::Stamped batch[] = {{T0, {26.25f}}};
  TEST_ASSERT_EQUAL_STRING(GOLDEN_VECTORS[2].body,
                           wire::buildBody(FIRMWARE_VERSION, batch, 1).c_str());
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
  const uint8_t mac[32] = {0xcb, 0xc8, 0x2d, 0xaa, 0xd4, 0x1a, 0xd8, 0x4e,
                           0xc1, 0x89, 0x6d, 0x9e, 0xc8, 0x88, 0x5b, 0x42,
                           0xcf, 0xda, 0xde, 0x4b, 0x73, 0x89, 0x22, 0xb3,
                           0x10, 0x82, 0xf0, 0x8d, 0xb0, 0xcc, 0x7b, 0xf5};
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
  wire::Stamped batch[] = {{T0, {NAN}}};
  TEST_ASSERT_EQUAL_STRING(
      R"RAW({"firmwareVersion":"0.3.0","samples":[{"recordedAt":"2023-11-14T22:13:20Z","values":{}}]})RAW",
      wire::buildBody(FIRMWARE_VERSION, batch, 1).c_str());
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
  RUN_TEST(test_signed_input_joins_with_one_newline);
  RUN_TEST(test_hex_is_lowercase);
  RUN_TEST(test_frame_matches_vector_payload);
  RUN_TEST(test_nan_parameter_is_omitted_not_null);
  return UNITY_END();
}
