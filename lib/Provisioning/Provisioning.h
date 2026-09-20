#pragma once

// Only for uint16_t in the turbidity-calibration accessors below. This header is the first include in
// Provisioning.cpp, so it has to stand on its own - without this line the fixed-width type is undeclared and
// the board build stops at "'uint16_t' does not name a type". A standard-library include is safe here in a
// way an Arduino one would not be, and unlike Sensors.h this header is never parsed by [env:native]
// (Provisioning is in lib_ignore and nothing on the host includes it).
#include <cstdint>

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

  // The unit's own clear-water reference, in sensor-side millivolts, as read from NVS at begin(). 0 means the
  // unit was never calibrated. Deliberately NOT part of isProvisioned(): an uncalibrated unit must still
  // finish WiFi/identity setup, join its network and keep reporting temperature - it simply reports no
  // turbidity at all (D-04), and says so once at boot.
  uint16_t turbidityClearWaterMv();

  // Persists a new clear-water reference. Returns false, with the reason logged, when the value is outside
  // the plausible clear-water window - so no caller can activate a calibration that would turn a healthy
  // sensor into a confidently wrong reading. Valid values take effect for the rest of this run as well.
  bool storeTurbidityClearWaterMv(uint16_t clearWaterMv);

  // Call on every loop(): drives the captive portal while it's open, watches for a held BOOT button or a long
  // WiFi outage to reopen it, and restarts the unit once a fresh WiFi + identity pair has been saved.
  void loop(bool wifiConnected);

  // True while the setup hotspot is up. Callers should skip anything that touches WiFi.mode()/begin() while
  // this is true - that would tear the portal's AP down.
  bool portalActive();
}
