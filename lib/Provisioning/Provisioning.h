#pragma once

// Only for uint16_t in the turbidity-calibration accessors below. This header is the first include in
// Provisioning.cpp, so it has to stand on its own - without this line the fixed-width type is undeclared and
// the board build stops at "'uint16_t' does not name a type". A standard-library include is safe here in a
// way an Arduino one would not be, and unlike the Sensors library's header this one is never parsed by [env:native]
// (Provisioning is in lib_ignore and nothing on the host includes it).
#include <cstdint>

// Lets an admin set a unit's WiFi and device identity from a phone, without reflashing, through a
// captive-portal hotspot (WiFiManager). WiFi credentials are stored by WiFiManager in the ESP32's own WiFi
// NVS; DEVICE_ID/DEVICE_SECRET are stored in this module's own "unit" NVS namespace. See firmware/CLAUDE.md
// for the field procedure (hotspot name, BOOT-hold, outage fallback).
namespace provisioning
{
  struct Config
  {
    // Shared by every unit; written in the admin's handbook, not printed on the enclosure.
    const char *setupApPassword;

    // The calibration sampler needs the turbidity sensor, but this module must not include the Sensors
    // library: that would pull hardware code into the identity/NVS owner and couple the two libraries' build
    // graphs. main.cpp injects the reader instead. One call is one ~64 ms burst, returning sensor-side mV, or
    // NAN when the pin is under the fault floor (no signal). May be null, which disables sampling.
    float (*readTurbiditySensorMv)();
    // NTU of the burst readTurbiditySensorMv() just took (NAN when uncalibrated or no signal). Read back rather
    // than computed here, so the portal shows exactly the number the sensor code produced.
    float (*readTurbidityNtu)();
    // How a freshly stored clear-water reference takes effect live, without a reboot, so the admin sees the
    // new NTU at once. May be null; the value is still persisted and is loaded at the next boot.
    void (*applyTurbidityCalibration)(unsigned short clearWaterMv);
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

  // When the stored clear-water reference was saved: Unix seconds UTC from NTP at the moment of the store. 0
  // means the date is unknown - the unit was calibrated before Phase 5, or its clock was not set at the time
  // (an unprovisioned unit never syncs NTP), per D-08.
  uint32_t turbidityCalibratedAt();

  // The single door into the calibration. Persists a new clear-water reference, then stamps the date it was
  // stored (turbidityCalibratedAt), then applies it live through Config::applyTurbidityCalibration. Returns
  // false, with the reason logged, when the value is outside the plausible clear-water window - so no caller
  // can activate a calibration that would turn a healthy sensor into a confidently wrong reading - or when the
  // value could not be written to NVS.
  bool storeTurbidityClearWaterMv(uint16_t clearWaterMv);

  // A clear-water capture (D-01): 20 samples, one burst per second, starting at the request, judged by
  // turbidity::cal::evaluateCapture and stored through storeTurbidityClearWaterMv only when accepted. It is
  // asynchronous: startTurbidityCapture() returns at once and the caller polls turbidityCaptureStatus(), because
  // a 20 s blocking wait would stall WiFiManager, the MQTT keepalive and PUBACK handling. Driven by loop(), which
  // must keep being called. Never restarts the unit - a calibration is not a WiFi/identity save.
  enum class CaptureState : uint8_t
  {
    Idle,
    Capturing,
    Accepted,
    Refused,
  };

  struct CaptureStatus
  {
    CaptureState state;
    uint8_t samples;   // samples collected so far, 0..20
    uint16_t medianMv; // set once the capture has finished (0 when the outcome carries none)
    float spreadMv;    // set once the capture has finished; NAN when the signal was lost
    // nullptr unless Refused; then one of "signal_lost" | "unstable" | "implausible" (the capture rule refused
    // it), "nvs" (accepted but the write failed) or "cancelled" (the setup hotspot closed mid-capture).
    const char *reason;
  };

  // Starts a capture. Returns false, changing nothing, if one is already running (one capture in flight).
  bool startTurbidityCapture();
  CaptureStatus turbidityCaptureStatus();

  struct LiveReadout
  {
    float sensorMv; // last sampled sensor-side mV, NAN before the first sample or with no signal
    float ntu;      // NTU of that same burst, NAN when uncalibrated or no signal
    const char *state; // turbidity::cal::liveStateToken: "settling" | "steady" | "unstable" | "signal_lost"
  };

  // The latest cached sample and the live stability state over the last 20 samples. Never takes a burst
  // itself, so polling it at any rate costs nothing; the sampler runs only while the setup hotspot is open or a
  // capture is running.
  LiveReadout turbidityLiveReadout();

  // Call on every loop(): drives the captive portal while it's open, watches for a held BOOT button or a long
  // WiFi outage to reopen it, and restarts the unit only after WiFi or device identity was saved in the current
  // portal session - so BOOT-hold on an installed unit keeps the hotspot open for recalibration.
  void loop(bool wifiConnected);

  // True while the setup hotspot is up. Callers should skip anything that touches WiFi.mode()/begin() while
  // this is true - that would tear the portal's AP down.
  bool portalActive();
}
