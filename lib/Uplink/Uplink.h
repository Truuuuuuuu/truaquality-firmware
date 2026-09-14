#pragma once

#include <cstdint>

#include "Sensors.h"

// Buffers timestamped samples and publishes them over MQTT to `truaquality/v1/devices/<DEVICE_ID>/readings`.
// Every message is signed with DEVICE_SECRET, because the broker login can't prove which unit sent it. The unit
// never names its pond: the backend decides where the readings belong.
namespace uplink
{
  struct Config
  {
    const char *mqttHost;
    uint16_t mqttPort;
    bool useTls;
    const char *mqttUsername;
    const char *mqttPassword;
    const char *deviceId;
    const char *deviceSecret;
    const char *firmwareVersion;
  };

  // The strings in config must outlive the uplink (string literals from unit_config.h do).
  void begin(const Config &config);

  // Samples are only timestamped (and so only buffered) once NTP has set the clock.
  bool timeSynced();

  // Adds a sample stamped with the current time. When the buffer is full the oldest sample is dropped.
  void enqueue(const SensorSample &sample);

  // Call on every loop(): keeps the MQTT connection up and publishes buffered samples, a batch at a time. A
  // batch leaves the buffer only once the broker acknowledges it (QoS 1).
  void loop();
}
