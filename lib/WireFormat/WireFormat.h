#pragma once

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <string>

#include "Sensors.h"

// Builds the exact bytes the backend verifies. Deliberately free of Arduino.h, mbedTLS and MQTT so it
// compiles in [env:native] and can be checked against backend/src/lib/__fixtures__/signing-vectors.v1.json
// without hardware. The HMAC itself stays in Uplink.cpp: mbedTLS has no host build, and a second HMAC
// implementation written just for the tests would prove nothing about the one that ships. Every byte below
// is load-bearing — reordering the JSON keys, dropping the "\n" separator or changing the "v1." prefix
// silently breaks authentication for every deployed unit, which is why test_wireformat pins all of them.
namespace wire
{
  struct Stamped
  {
    std::time_t recordedAt;
    SensorSample sample;
  };

  // Mirrors the backend's readingsTopic(): TOPIC_PREFIX + deviceId + TOPIC_SUFFIX.
  std::string readingsTopic(const char *deviceId);

  // Whole-second UTC, no ".000Z" — the backend fixture is generated that way on purpose.
  void formatIso8601(std::time_t epoch, char *out, size_t size);

  // The signed JSON body. A NAN parameter is omitted rather than sent as a zero or a null. wifiSsid is a plain
  // C string (this library stays Arduino-free); nullptr or "" omits the key entirely.
  std::string buildBody(const char *firmwareVersion, const char *wifiSsid, const Stamped *samples, size_t count);

  // The bytes the HMAC covers: "<topic>\n<body>". Must match the backend's hmacHex().
  std::string signedInput(const std::string &topic, const std::string &body);

  // Lowercase hex. `out` must have room for len*2 + 1 bytes.
  void toHexLower(const uint8_t *mac, size_t len, char *out);

  // "v1.<signatureHex>.<body>". Takes the signature as hex so this stays testable without crypto.
  std::string frame(const char *signatureHex, const std::string &body);
}
