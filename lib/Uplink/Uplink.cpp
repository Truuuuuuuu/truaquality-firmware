#include "Uplink.h"

#include <Arduino.h>
#include <WiFi.h>
#include <espMqttClient.h>
#include <mbedtls/md.h>
#include <string>
#include <time.h>

#include "RootCa.h"
#include "WireFormat.h"

namespace
{
  constexpr size_t BUFFER_CAPACITY = 120;
  // Keeps each message a few KB and lets a full backlog drain in 12 messages, under the backend's per-device
  // rate limit (30 messages/minute) and its 120-samples-per-message cap.
  constexpr size_t MAX_SAMPLES_PER_MESSAGE = 10;
  constexpr unsigned long ACK_TIMEOUT_MS = 15000;
  constexpr unsigned long RECONNECT_INTERVAL_MS = 5000;
  // Any real clock reading is well past this (Nov 2023); an unsynced ESP32 starts at 1970.
  constexpr time_t MIN_VALID_EPOCH = 1700000000;

  struct BufferedSample
  {
    time_t recordedAt;
    SensorSample sample;
  };

  BufferedSample buffer[BUFFER_CAPACITY];
  size_t head = 0; // index of the oldest sample
  size_t count = 0;

  uplink::Config config;
  std::string topic;

  // Both are constructed so TLS can be chosen at runtime; only one ever connects.
  espMqttClientSecure secureClient(espMqttClientTypes::UseInternalTask::NO);
  espMqttClient plainClient(espMqttClientTypes::UseInternalTask::NO);
  MqttClient *client = nullptr;

  // The batch waiting for a PUBACK. 0 means nothing is in flight.
  uint16_t inFlightPacketId = 0;
  size_t inFlightSamples = 0;
  unsigned long inFlightSentAtMs = 0;
  unsigned long lastConnectAttemptMs = 0;

  // Lowercase hex HMAC-SHA256 of `<topic>\n<body>`, matching the backend's deviceMessages.ts. The bytes
  // being signed come from wire::, which the native suite pins against the backend's golden vectors; the
  // only thing left here is the mbedTLS call, because mbedTLS has no host build.
  bool signBody(const std::string &body, char hexOut[65])
  {
    std::string signedInput = wire::signedInput(topic, body);
    uint8_t mac[32];
    const mbedtls_md_info_t *sha256 = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    int result = mbedtls_md_hmac(
        sha256,
        reinterpret_cast<const unsigned char *>(config.deviceSecret), strlen(config.deviceSecret),
        reinterpret_cast<const unsigned char *>(signedInput.c_str()), signedInput.length(),
        mac);
    if (result != 0)
    {
      return false;
    }
    wire::toHexLower(mac, sizeof(mac), hexOut);
    return true;
  }

  void publishOldestBatch()
  {
    size_t batchSize = count < MAX_SAMPLES_PER_MESSAGE ? count : MAX_SAMPLES_PER_MESSAGE;

    wire::Stamped batch[MAX_SAMPLES_PER_MESSAGE];
    for (size_t i = 0; i < batchSize; i++)
    {
      const BufferedSample &entry = buffer[(head + i) % BUFFER_CAPACITY];
      batch[i] = wire::Stamped{entry.recordedAt, entry.sample};
    }
    // Read at publish time (per message, not per sample): the network can change between buffering a reading
    // and flushing it, and the backend only wants the last known one. The String must outlive buildBody.
    String ssid = WiFi.SSID();
    std::string body = wire::buildBody(config.firmwareVersion, ssid.c_str(), batch, batchSize);

    char signature[65];
    if (!signBody(body, signature))
    {
      Serial.println("[uplink] signing failed");
      return;
    }
    std::string payload = wire::frame(signature, body);

    uint16_t packetId = client->publish(topic.c_str(), 1, false, payload.c_str());
    if (packetId == 0)
    {
      Serial.println("[uplink] publish could not be queued, will retry");
      return;
    }
    inFlightPacketId = packetId;
    inFlightSamples = batchSize;
    inFlightSentAtMs = millis();
  }
}

namespace uplink
{
  void begin(const Config &newConfig)
  {
    config = newConfig;
    topic = wire::readingsTopic(config.deviceId);

    if (config.useTls)
    {
      secureClient.setCACert(HIVEMQ_ROOT_CA);
      secureClient.setServer(config.mqttHost, config.mqttPort);
      secureClient.setCredentials(config.mqttUsername, config.mqttPassword);
      secureClient.setClientId(config.deviceId);
      client = &secureClient;
    }
    else
    {
      plainClient.setServer(config.mqttHost, config.mqttPort);
      plainClient.setCredentials(config.mqttUsername, config.mqttPassword);
      plainClient.setClientId(config.deviceId);
      client = &plainClient;
    }

    auto onConnect = [](bool)
    { Serial.println("[mqtt] connected"); };
    auto onDisconnect = [](espMqttClientTypes::DisconnectReason reason)
    {
      Serial.printf("[mqtt] disconnected: %s\n", espMqttClientTypes::disconnectReasonToString(reason));
      // Republish the batch after reconnecting. If the broker did get it, the backend skips the duplicates.
      inFlightPacketId = 0;
    };
    auto onPublish = [](uint16_t packetId)
    {
      if (packetId != inFlightPacketId)
      {
        return;
      }
      head = (head + inFlightSamples) % BUFFER_CAPACITY;
      count -= inFlightSamples;
      inFlightPacketId = 0;
    };
    if (config.useTls)
    {
      secureClient.onConnect(onConnect).onDisconnect(onDisconnect).onPublish(onPublish);
    }
    else
    {
      plainClient.onConnect(onConnect).onDisconnect(onDisconnect).onPublish(onPublish);
    }

    // UTC; the backend and dashboard handle display time zones.
    configTime(0, 0, "pool.ntp.org", "time.google.com");
  }

  bool timeSynced()
  {
    return time(nullptr) > MIN_VALID_EPOCH;
  }

  void enqueue(const SensorSample &sample)
  {
    // A full buffer while a batch is in flight would shift which samples that PUBACK refers to, so drop the
    // incoming sample instead of the oldest one in that case.
    if (count == BUFFER_CAPACITY && inFlightPacketId != 0)
    {
      Serial.println("[uplink] buffer full, dropped new sample");
      return;
    }
    size_t tail = (head + count) % BUFFER_CAPACITY;
    buffer[tail] = BufferedSample{time(nullptr), sample};
    if (count < BUFFER_CAPACITY)
    {
      count++;
    }
    else
    {
      head = (head + 1) % BUFFER_CAPACITY;
      Serial.println("[uplink] buffer full, dropped oldest sample");
    }
  }

  void loop()
  {
    if (!client)
    {
      return;
    }
    client->loop();

    if (WiFi.status() != WL_CONNECTED)
    {
      return;
    }
    if (!client->connected())
    {
      if (client->disconnected() && millis() - lastConnectAttemptMs >= RECONNECT_INTERVAL_MS)
      {
        lastConnectAttemptMs = millis();
        Serial.printf("[mqtt] connecting to %s:%u\n", config.mqttHost, config.mqttPort);
        client->connect();
      }
      return;
    }

    if (inFlightPacketId != 0 && millis() - inFlightSentAtMs >= ACK_TIMEOUT_MS)
    {
      Serial.println("[uplink] no PUBACK, republishing");
      inFlightPacketId = 0;
    }
    if (inFlightPacketId == 0 && count > 0)
    {
      publishOldestBatch();
    }
  }
}
