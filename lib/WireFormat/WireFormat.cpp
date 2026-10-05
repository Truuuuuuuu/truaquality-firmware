#include "WireFormat.h"

#include <ArduinoJson.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace
{
  constexpr int64_t INT32_MAX_VALUE = 2147483647;
  // The backend's per-message sample cap, which is also Uplink's ring-buffer capacity.
  constexpr int64_t MAX_QUEUED = 120;

  // Every bound is the backend's diagSchema (backend/src/schemas/ingest.ts). A value outside it doesn't get
  // one field dropped: zod rejects the whole signed message and the batch's readings are lost with it. The
  // result is cast to int32_t so ArduinoJson takes the same integer path on host and board, whatever
  // ARDUINOJSON_USE_LONG_LONG is set to. std::min/std::max, not std::clamp (C++17; the board is gnu++11).
  int32_t clampInt(int64_t value, int64_t low, int64_t high)
  {
    return static_cast<int32_t>(std::min(std::max(value, low), high));
  }

  void addValue(JsonObject values, const char *parameter, float value)
  {
    // NAN means "no reading". Omitting the key is not the same as sending null: ARDUINOJSON_ENABLE_NAN and
    // ARDUINOJSON_ENABLE_INFINITY are both 0, so a NAN or +/-infinity that got past this guard would
    // serialize as null, which the backend silently accepts and drops — the payloads would grow and nothing
    // would look broken. isfinite covers both; a future driver that divides by zero must not slip through.
    if (std::isfinite(value))
    {
      values[parameter] = value;
    }
  }
}

namespace wire
{
  std::string readingsTopic(const char *deviceId)
  {
    return std::string("truaquality/v1/devices/") + deviceId + "/readings";
  }

  void formatIso8601(std::time_t epoch, char *out, size_t size)
  {
    struct tm utc;
    gmtime_r(&epoch, &utc);
    // The format string is part of the wire format: the backend's fixture uses whole-second "Z" timestamps
    // and never ".000Z", so a switch to a millisecond form would change every signed byte.
    strftime(out, size, "%Y-%m-%dT%H:%M:%SZ", &utc);
  }

  const char *resetReasonToken(ResetReason reason)
  {
    // One case per enumerator and no default, so -Wswitch flags a new reason with no token. The strings are
    // the backend's RESET_REASONS; any other string rejects the whole message.
    switch (reason)
    {
    case ResetReason::PowerOn:
      return "power_on";
    case ResetReason::Software:
      return "software";
    case ResetReason::Panic:
      return "panic";
    case ResetReason::IntWdt:
      return "int_wdt";
    case ResetReason::TaskWdt:
      return "task_wdt";
    case ResetReason::Wdt:
      return "wdt";
    case ResetReason::Brownout:
      return "brownout";
    case ResetReason::DeepSleep:
      return "deep_sleep";
    case ResetReason::External:
      return "external";
    case ResetReason::Unknown:
      return "unknown";
    }
    return "unknown";
  }

  std::string buildBody(const char *firmwareVersion, const char *wifiSsid, const Stamped *samples, size_t count)
  {
    return buildBody(firmwareVersion, wifiSsid, nullptr, false, samples, count);
  }

  std::string buildBody(const char *firmwareVersion, const char *wifiSsid, const Diagnostics *diag,
                        bool includeSensorStatus, const Stamped *samples, size_t count)
  {
    JsonDocument doc;
    // The order of these assignments IS the JSON key order, and the JSON key order is signed bytes: it must
    // match backend/scripts/generate-signing-vectors.ts (firmwareVersion, wifiSsid, diag, sensors, samples).
    // An empty or missing SSID is omitted, never sent as "". ArduinoJson escapes quotes, backslashes and
    // control characters and leaves UTF-8 raw, exactly like the backend's JSON.stringify.
    doc["firmwareVersion"] = firmwareVersion;
    if (wifiSsid != nullptr && wifiSsid[0] != '\0')
    {
      doc["wifiSsid"] = wifiSsid;
    }
    if (diag != nullptr)
    {
      // Inner order is signed too: rssi, uptimeS, resetReason, freeHeap, queued.
      JsonObject d = doc["diag"].to<JsonObject>();
      d["rssi"] = clampInt(diag->rssi, -127, 0);
      d["uptimeS"] = clampInt(diag->uptimeS, 0, INT32_MAX_VALUE);
      d["resetReason"] = resetReasonToken(diag->resetReason);
      d["freeHeap"] = clampInt(diag->freeHeap, 0, INT32_MAX_VALUE);
      d["queued"] = clampInt(diag->queued, 0, MAX_QUEUED);
    }
    if (includeSensorStatus && count > 0)
    {
      // The newest sample's status: the backend records "status at the newest sample in the batch", so an
      // older sample's status here would report a recovered probe as still faulted (or the reverse).
      const SensorSample &newest = samples[count - 1].sample;
      JsonObject sensorsObj = doc["sensors"].to<JsonObject>();
      sensorsObj["temperature"] = sensors::statusToken(newest.temperatureStatus);
      sensorsObj["turbidity"] = sensors::statusToken(newest.turbidityStatus);
      // A build with no pH front end sends no sensors.ph at all, so its body stays the 0.6.x shape. ph comes
      // after turbidity: that is the backend fixture's key order, and key order is signed bytes.
      if (newest.phStatus != PhStatus::NotFitted)
      {
        sensorsObj["ph"] = sensors::statusToken(newest.phStatus);
      }
    }
    JsonArray array = doc["samples"].to<JsonArray>();
    // Non-const char[] on purpose: ArduinoJson copies it. A const char* would be stored by reference and
    // every sample in the batch would end up carrying the last timestamp.
    char timestamp[25];
    for (size_t i = 0; i < count; i++)
    {
      JsonObject item = array.add<JsonObject>();
      formatIso8601(samples[i].recordedAt, timestamp, sizeof(timestamp));
      item["recordedAt"] = timestamp;
      JsonObject values = item["values"].to<JsonObject>();
      // The order of these three calls IS the JSON key order, and the JSON key order is signed bytes:
      // reordering them changes every signature the backend verifies. It must match the object-literal order
      // in backend/scripts/generate-signing-vectors.ts (temperature, turbidity, ph), which is where the golden
      // vectors come from. A NotFitted pH is NAN, so addValue drops it like any other missing reading.
      addValue(values, "temperature", samples[i].sample.temperature);
      addValue(values, "turbidity", samples[i].sample.turbidity);
      addValue(values, "ph", samples[i].sample.ph);
    }
    std::string body;
    serializeJson(doc, body);
    return body;
  }

  std::string signedInput(const std::string &topic, const std::string &body)
  {
    return topic + "\n" + body;
  }

  void toHexLower(const uint8_t *mac, size_t len, char *out)
  {
    for (size_t i = 0; i < len; i++)
    {
      // Lowercase "%02x" is pinned by the golden vectors. The backend tolerates uppercase today, so a case
      // flip here would go unnoticed until some other verifier stopped being lenient.
      snprintf(out + i * 2, 3, "%02x", mac[i]);
    }
    out[len * 2] = '\0';
  }

  std::string frame(const char *signatureHex, const std::string &body)
  {
    return std::string("v1.") + signatureHex + "." + body;
  }
}
