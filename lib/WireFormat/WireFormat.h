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

  // esp_reset_reason() reduced to the backend's RESET_REASONS (backend/src/schemas/ingest.ts). Its own enum so
  // this library stays free of the ESP-IDF headers; Uplink.cpp does the mapping on the board.
  enum class ResetReason : uint8_t
  {
    PowerOn,
    Software,
    Panic,
    IntWdt,
    TaskWdt,
    Wdt,
    Brownout,
    DeepSleep,
    External,
    Unknown,
  };

  const char *resetReasonToken(ResetReason reason);

  // The unit's self-report, sent once per message as the body's "diag" object (firmware >= 0.6.0). Wide
  // int64 fields so callers pass raw values straight in: buildBody clamps each into the backend's accepted
  // range, because one out-of-range number makes the backend reject the whole message, readings included.
  struct Diagnostics
  {
    int64_t rssi;     // dBm, sent within -127..0
    int64_t uptimeS;  // seconds since boot, sent within 0..INT32_MAX
    ResetReason resetReason;
    int64_t freeHeap; // bytes, sent within 0..INT32_MAX
    int64_t queued;   // samples buffered, sent within 0..120
  };

  // Mirrors the backend's readingsTopic(): TOPIC_PREFIX + deviceId + TOPIC_SUFFIX.
  std::string readingsTopic(const char *deviceId);

  // Whole-second UTC, no ".000Z" — the backend fixture is generated that way on purpose.
  void formatIso8601(std::time_t epoch, char *out, size_t size);

  // The signed JSON body. A NAN parameter is omitted rather than sent as a zero or a null. wifiSsid is a plain
  // C string (this library stays Arduino-free); nullptr or "" omits the key entirely.
  // This overload never emits "diag" or "sensors"; it is the pre-0.6.0 body shape the first seven golden
  // vectors pin.
  std::string buildBody(const char *firmwareVersion, const char *wifiSsid, const Stamped *samples, size_t count);

  // The 0.6.0 body: firmwareVersion, wifiSsid?, diag? (when `diag` is non-null), sensors? (when
  // includeSensorStatus and count > 0, taken from the NEWEST sample in the batch — the backend stores the
  // status "at the newest sample"), samples. That key order is signed bytes.
  std::string buildBody(const char *firmwareVersion, const char *wifiSsid, const Diagnostics *diag,
                        bool includeSensorStatus, const Stamped *samples, size_t count);

  // The bytes the HMAC covers: "<topic>\n<body>". Must match the backend's hmacHex().
  std::string signedInput(const std::string &topic, const std::string &body);

  // Lowercase hex. `out` must have room for len*2 + 1 bytes.
  void toHexLower(const uint8_t *mac, size_t len, char *out);

  // "v1.<signatureHex>.<body>". Takes the signature as hex so this stays testable without crypto.
  std::string frame(const char *signatureHex, const std::string &body);
}
