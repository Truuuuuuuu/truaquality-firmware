# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project state

This is a PlatformIO/Arduino firmware project for the ESP32 sensor unit (`nodemcu-32s`) installed at a
fishpond. Each pond has its own unit. A unit takes a reading every `REPORT_INTERVAL_MS`, buffers it, and
publishes it over **MQTT (TLS) to HiveMQ Cloud**. The backend subscribes there; units never call the backend's
HTTP API. There is no test suite yet.

**The unit never knows which pond it's in.** It only holds its `DEVICE_ID` and `DEVICE_SECRET`. The backend maps
the device to a pond, so moving a unit to a different pond is an admin reassignment on the Devices page, not a
reflash.

**Every unit runs the same firmware image.** WiFi and device identity are entered per unit, in the field, once
— see "Field provisioning" below — rather than being compiled in per unit.

**Temperature is a real, verified sensor.** A DS18B20 waterproof probe on GPIO 4 (OneWire), verified
end-to-end on a real ESP32 unit — readings land in the database and dashboard. **Dissolved oxygen and
salinity are still stubs**: those modules haven't been chosen yet, so their readers in
`lib/Sensors/Sensors.cpp` return `NAN` (nothing is uploaded for that parameter) until real drivers replace
the TODOs. Don't invent part numbers or fake values for those two.

### Build-time configuration

Copy `include/unit_config.example.h` to `include/unit_config.h` (gitignored) and fill in — every unit gets the
same file and the same compiled image now, there's no per-unit build:
- HiveMQ: `MQTT_HOST`, `MQTT_PORT`, and one MQTT credential (`MQTT_USERNAME` / `MQTT_PASSWORD`) shared by all
  units, created in the HiveMQ console.
- `SETUP_AP_PASSWORD`: the WPA2 password for every unit's setup hotspot (see "Field provisioning" below).
  Shared by all units, kept in the technician's handbook rather than printed on the enclosure.
- `REPORT_INTERVAL_MS`.

`src/main.cpp` `#error`s if `unit_config.h` is missing. **Don't rename it to `config.h`:** on macOS's
case-insensitive filesystem, `#include "config.h"` silently resolves to espMqttClient's `Config.h`, and every
setting then shows up as undeclared. `MQTT_USE_TLS 0` exists only for a plaintext broker on a
local network.

### Field provisioning

**WiFi and each unit's `DEVICE_ID`/`DEVICE_SECRET` are no longer compiled in.** A sealed, deployed unit can't
be plugged into USB for a reflash, so those are entered from a phone instead and kept in flash (NVS).
`lib/Provisioning/` wraps [WiFiManager](https://github.com/tzapu/WiFiManager) to do this.

- **Unprovisioned** (no saved WiFi, or no saved device identity): the unit opens its own WiFi hotspot,
  `TruAquality-XXXX` (`XXXX` = the last two bytes of its MAC, also printed in the serial log at boot), secured
  with `SETUP_AP_PASSWORD`. Joining it from a phone pops up a captive-portal setup page with two screens:
  "Configure WiFi" (network + password, saved by WiFiManager into the ESP32's own WiFi NVS) and "Setup"
  (Device ID + Device secret, copied from the Devices page's registration/rotation dialog, saved into this
  module's own `unit` NVS namespace). The onboard LED (GPIO 2) blinks fast the whole time; the unit takes no
  readings until both are saved, at which point it restarts.
- **Provisioned:** the unit joins its saved WiFi and runs exactly as before.
- **Re-entering setup on a working unit** (pond router replaced, secret rotated on the Devices page, …): hold
  the BOOT button (GPIO 0) for 5 seconds. The hotspot reopens for 5 minutes; sampling and the MQTT buffer keep
  running the whole time. **Press it after power-on, not during** — held down while powering up, GPIO 0 instead
  drops the chip into its ROM download mode. A waterproof button wired from GPIO 0 to GND on the enclosure
  needs no code change.
- **Automatic fallback:** if the saved WiFi is unreachable for 10 straight minutes, the hotspot opens on its
  own for 5 minutes, then closes and goes back to retrying the saved network — repeating for as long as the
  outage lasts, so a unit nobody can reach in person still has a way back online.
- A plain `pio run -t upload` leaves NVS (so the saved WiFi/identity) alone. Only `pio run -t erase` wipes it —
  use that for "fresh unit" testing.

## Commands

This project uses PlatformIO. If the `pio` CLI is not on `PATH` in the shell (it isn't in this environment
by default), invoke it via the PlatformIO Core install, typically at `~/.platformio/penv/bin/pio`, or use
the PlatformIO IDE extension in VS Code.

- Build: `pio run`
- Build for a specific environment: `pio run -e nodemcu-32s`
- Upload to the connected board: `pio run -t upload`
- Serial monitor: `pio device monitor`
- Clean build artifacts: `pio run -t clean`
- Run unit tests (once tests exist under `test/`): `pio test`
- Run a single test file: `pio test -f <test_name>`

## Architecture

- `platformio.ini` defines a single build environment, `[env:nodemcu-32s]`, targeting the `espressif32`
  platform with the Arduino framework. Additional environments (e.g. for a different board or a
  native/test environment) would be added here as new `[env:...]` sections. `lib_deps`: ArduinoJson v7,
  espMqttClient, `tzapu/WiFiManager` (captive-portal provisioning), and (for the DS18B20)
  `paulstoffregen/OneWire` + `milesburton/DallasTemperature`.
- `src/main.cpp` is the firmware entry point. `provisioning::loop()` runs on every pass regardless of
  provisioning state; sensor reads and `uplink::loop()` only start once `provisioning::isProvisioned()` is
  true. It reads the sensors on each interval (only after NTP has synced, so every sample has a real
  timestamp).
- `lib/Provisioning/`: see "Field provisioning" above. Owns the WiFiManager instance, the BOOT-button and
  WiFi-outage triggers for reopening the setup hotspot, and the `unit` NVS namespace holding `device_id` /
  `device_secret`.
- `lib/Sensors/`: `sensors::readAll()` returns a `SensorSample` (temperature °C, dissolved oxygen mg/L,
  salinity ppt).
  - **Temperature:** real DS18B20 driver, OneWire bus on GPIO 4. The probe's data line needs a ~4.7kΩ
    pull-up to 3V3 if the module doesn't already have one built in. `sensors::begin()` logs a warning if no
    DS18B20 is found on the bus at boot (check wiring/pull-up if that happens).
  - **Dissolved oxygen, salinity:** still stubbed, return `NAN`.
  - `NAN` for any parameter means "no reading" — it's dropped from the upload rather than sent as a fake `0`.
- `lib/Uplink/`: a 120-sample ring buffer published with espMqttClient (`UseInternalTask::NO`, so
  `uplink::loop()` drives it).
  - **Topic:** `truaquality/v1/devices/<DEVICE_ID>/readings`, QoS 1.
  - **Batching:** up to 10 samples per message, one message in flight at a time.
  - **Acknowledgement:** a batch leaves the buffer only on its PUBACK. On disconnect, or after 15 s without an
    ack, it's republished, and the backend skips the duplicates.
  - **Signed payload (must match `backend/src/lib/deviceMessages.ts` exactly):**
    `v1.<lowercase hex HMAC-SHA256(DEVICE_SECRET, "<topic>\n<body>")>.<body>`.
    - Signed because HiveMQ's free tier can't limit an MQTT login to its own topics.
    - The body is `{"firmwareVersion", "samples": [{"recordedAt": ISO-8601 UTC, "values": {...}}]}`.
    - JSON parameter keys (`temperature`, `dissolvedOxygen`, `salinity`) must match the backend's
      `PARAMETER_BOUNDS`.
  - **TLS:** verified against ISRG Root X1 (`lib/Uplink/RootCa.h`, Let's Encrypt, valid until 2035), the root
    of HiveMQ Cloud's certificates.
- `include/` is for project header files shared across `src/` files.
- `test/` is for PlatformIO Unit Testing (Unity-based on-device or native tests).
