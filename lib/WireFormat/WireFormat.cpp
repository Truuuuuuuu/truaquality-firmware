#include "WireFormat.h"

#include <ArduinoJson.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>

namespace
{
  void addValue(JsonObject values, const char *parameter, float value)
  {
    // NAN means "no reading". Omitting the key is not the same as sending null: ARDUINOJSON_ENABLE_NAN is 0,
    // so a NAN that got past this guard would serialize as null, which the backend silently accepts and
    // drops — the payloads would grow and nothing would look broken.
    if (!std::isnan(value))
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

  std::string buildBody(const char *firmwareVersion, const Stamped *samples, size_t count)
  {
    JsonDocument doc;
    doc["firmwareVersion"] = firmwareVersion;
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
      addValue(values, "temperature", samples[i].sample.temperature);
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
