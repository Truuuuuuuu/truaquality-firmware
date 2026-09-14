// Copy this file to include/unit_config.h (gitignored) and fill it in. Unlike before, every unit gets the
// same unit_config.h and the same compiled firmware image - WiFi and DEVICE_ID/DEVICE_SECRET are no longer
// compile-time settings. A technician sets those per unit, in the field, through the setup hotspot the unit
// opens on first boot (see "Field provisioning" in firmware/CLAUDE.md).
// (Not named config.h: on case-insensitive filesystems that collides with espMqttClient's Config.h.)
#pragma once

// HiveMQ Cloud cluster: console > your cluster > Overview (host) and Access Management (credentials).
// All units can share one MQTT credential — the broker login isn't what identifies a unit (DEVICE_SECRET,
// entered per unit through the setup hotspot, is).
#define MQTT_HOST "your-cluster-id.s1.eu.hivemq.cloud"
#define MQTT_PORT 8883
#define MQTT_USERNAME "your-device-mqtt-username"
#define MQTT_PASSWORD "your-device-mqtt-password"
// 1 for HiveMQ Cloud (TLS, verified against ISRG Root X1). 0 only for a plaintext broker on a local network.
#define MQTT_USE_TLS 1

// WPA2 password for the "TruAquality-XXXX" setup hotspot every unit opens until it's provisioned (or while a
// technician holds its BOOT button). Shared by every unit - write it in the technician's handbook, not on the
// enclosure. Must be at least 8 characters (WPA2's minimum) or the hotspot won't start.
#define SETUP_AP_PASSWORD "change-me-setup-password"

// How often to take a reading. The dashboard marks a pond stale after 5 minutes of silence.
#define REPORT_INTERVAL_MS 30000UL
