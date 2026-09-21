#include <Arduino.h>
#include <unity.h>
#include <mbedtls/md.h>

#include <cstring>
#include <string>

#include "WireFormat.h"

#include "../golden/signing_vectors.h"

// The other half of the parity proof. test_wireformat (native) pins the *bytes* the unit publishes; this
// suite pins the *signature* over them, using the same mbedTLS HMAC the shipped firmware calls. It can only
// run here: mbedTLS has no host build, which is the whole reason the crypto stayed in lib/Uplink/ when the
// byte construction moved to lib/WireFormat/.
//
// Nothing is ever printed but a signature. This output travels over an unencrypted USB serial link to
// whatever machine is running `pio test`, so no Serial.print of a secret, a device id's secret or a full
// signed payload belongs in this file — a leak here would be as bad as committing the secret, and harder to
// notice. Unity's own pass/fail lines and the assertion's expected/actual strings (signatures only) are the
// entire permitted output.

namespace
{
  // Mirrors signBody() in lib/Uplink/Uplink.cpp exactly, down to the reinterpret_cast shape and the lengths.
  // Deliberately not a second implementation: a hand-rolled SHA-256 agreeing with these vectors would say
  // nothing about the mbedtls_md_hmac call that actually signs what the backend receives.
  bool hmacHex(const char *secret, const std::string &input, char out[65])
  {
    uint8_t mac[32];
    const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (mbedtls_md_hmac(sha256,
                        reinterpret_cast<const unsigned char *>(secret), strlen(secret),
                        reinterpret_cast<const unsigned char *>(input.c_str()), input.length(),
                        mac) != 0)
    {
      return false;
    }
    wire::toHexLower(mac, sizeof(mac), out);
    out[64] = '\0';
    return true;
  }
}

void test_golden_signatures(void)
{
  for (unsigned i = 0; i < GOLDEN_VECTOR_COUNT; i++)
  {
    const GoldenVector &v = GOLDEN_VECTORS[i];
    char signature[65];
    // The input is wire::signedInput(topic, body), not a locally assembled string, so the device covers the
    // same assembly the native suite pins. If this fails while test_wireformat is green, the fault is in
    // mbedTLS or the key handling, not in the bytes.
    TEST_ASSERT_TRUE(hmacHex(v.secret, wire::signedInput(v.topic, v.body), signature));
    TEST_ASSERT_EQUAL_STRING(v.signature, signature);
  }
}

void test_fixture_is_dummy_secrets(void)
{
  // Committed-secret discipline: every secret in signing_vectors.h is a dummy literal containing "not-real"
  // (backend/scripts/generate-signing-vectors.ts enforces it at the source). If someone ever pastes a real
  // DEVICE_SECRET in to debug a mismatch, this fails loudly instead of the secret shipping quietly in git
  // history. The count check also catches a truncated or half-regenerated header, which would otherwise let
  // test_golden_signatures pass vacuously.
  TEST_ASSERT_EQUAL_UINT(7, GOLDEN_VECTOR_COUNT);
  for (unsigned i = 0; i < GOLDEN_VECTOR_COUNT; i++)
  {
    TEST_ASSERT_TRUE(strstr(GOLDEN_VECTORS[i].secret, "not-real") != nullptr);
  }
}

void setup()
{
  delay(2000); // let the host attach to the serial port before the first result line goes out
  UNITY_BEGIN();
  RUN_TEST(test_golden_signatures);
  RUN_TEST(test_fixture_is_dummy_secrets);
  UNITY_END();
}

void loop() {}
