#pragma once

// Lets a technician set a unit's WiFi and device identity from a phone, without reflashing, through a
// captive-portal hotspot (WiFiManager). WiFi credentials are stored by WiFiManager in the ESP32's own WiFi
// NVS; DEVICE_ID/DEVICE_SECRET are stored in this module's own "unit" NVS namespace. See firmware/CLAUDE.md
// for the field procedure (hotspot name, BOOT-hold, outage fallback).
namespace provisioning
{
  struct Config
  {
    // Shared by every unit; written in the technician's handbook, not printed on the enclosure.
    const char *setupApPassword;
  };

  // Loads any saved identity and, if the unit isn't fully provisioned yet, opens the setup hotspot. Call once
  // from setup(), after Serial.begin() and before anything touches WiFi.
  void begin(const Config &config);

  // True once both a WiFi network and a device identity (DEVICE_ID + DEVICE_SECRET) are saved.
  bool isProvisioned();

  // Valid only once isProvisioned() is true. The buffers live for the whole program, so they're safe to hand
  // straight to uplink::Config.
  const char *deviceId();
  const char *deviceSecret();

  // Call on every loop(): drives the captive portal while it's open, watches for a held BOOT button or a long
  // WiFi outage to reopen it, and restarts the unit once a fresh WiFi + identity pair has been saved.
  void loop(bool wifiConnected);

  // True while the setup hotspot is up. Callers should skip anything that touches WiFi.mode()/begin() while
  // this is true - that would tear the portal's AP down.
  bool portalActive();
}
