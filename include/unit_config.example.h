// Copy this file to include/unit_config.h (gitignored) and fill it in for each unit.
// (Not named config.h: on case-insensitive filesystems that collides with espMqttClient's Config.h.)
#pragma once

#define WIFI_SSID "your-wifi-ssid"
#define WIFI_PASS "your-wifi-password"

// HiveMQ Cloud cluster: console > your cluster > Overview (host) and Access Management (credentials).
// All units can share one MQTT credential — the broker login isn't what identifies a unit (see DEVICE_SECRET).
#define MQTT_HOST "your-cluster-id.s1.eu.hivemq.cloud"
#define MQTT_PORT 8883
#define MQTT_USERNAME "your-device-mqtt-username"
#define MQTT_PASSWORD "your-device-mqtt-password"
// 1 for HiveMQ Cloud (TLS, verified against ISRG Root X1). 0 only for a plaintext broker on a local network.
#define MQTT_USE_TLS 1

// Per unit. Shown once when an admin registers the device (or rotates its secret) on the Devices page.
#define DEVICE_ID "00000000-0000-0000-0000-000000000000"
#define DEVICE_SECRET "paste-the-device-secret"

// How often to take a reading. The dashboard marks a pond stale after 5 minutes of silence.
#define REPORT_INTERVAL_MS 30000UL
