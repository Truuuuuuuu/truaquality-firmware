# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project state

This is a PlatformIO/Arduino firmware project for the ESP32 sensor unit (`nodemcu-32s`) installed at a
fishpond. Each pond has its own unit. A unit takes a reading every `REPORT_INTERVAL_MS`, buffers it, and
publishes it over **MQTT (TLS) to HiveMQ Cloud**. The backend subscribes there; units never call the backend's
HTTP API.

**The wire format is covered by two Unity suites, and it takes both.** `test/test_wireformat/` runs under
`[env:native]` on the laptop and checks the signed *bytes* — topic, ISO-8601 timestamp, JSON body, signed
input and the framed payload — against the backend's golden vectors in about a second, so there's no excuse
to skip it. `test/test_signing/` runs on a real board and checks the *signature*, because mbedTLS has no host
build: the crypto that actually authenticates every published batch can only be exercised on hardware. If the
device suite fails while the native one is green, the fault is in mbedTLS or key handling, not in the bytes.

**The unit never knows which pond it's in.** It only holds its `DEVICE_ID` and `DEVICE_SECRET`. The backend maps
the device to a pond, so moving a unit to a different pond is an admin reassignment on the Devices page, not a
reflash.

**Every unit runs the same firmware image.** WiFi and device identity are entered per unit, in the field, once
— see "Field provisioning" below — rather than being compiled in per unit.

**Temperature is a real, verified sensor.** A DS18B20 waterproof probe on GPIO 4 (OneWire), verified
end-to-end on a real ESP32 unit — readings land in the database and dashboard. It is currently the only
parameter; **turbidity is the next sensor to be added** (module not chosen yet — don't invent a part number
or fake values). Adding a parameter means a field on `SensorSample`, a reader in `lib/Sensors/Sensors.cpp`,
an `addValue` line in `lib/Uplink/Uplink.cpp`, and the same id in the backend's `PARAMETER_BOUNDS` and the
frontend's `PARAMETERS`.

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

- Build: `pio run -e nodemcu-32s`
- Upload to the connected board: `pio run -t upload`
- Serial monitor: `pio device monitor`
- Clean build artifacts: `pio run -t clean`
- Host wire-format suite (no board needed): `pio test -e native`
- On-device HMAC suite (needs a connected ESP32): `pio test -e nodemcu-32s -f test_signing`
- Compile the on-device suite without a board:
  `pio test -e nodemcu-32s -f test_signing --without-uploading --without-testing`
- Sync the golden vectors from the backend: `node scripts/sync-golden-vectors.mjs`
- Check the golden vectors for drift: `node scripts/sync-golden-vectors.mjs --check`

**Always pass `-e` to `pio run`.** Now that `[env:native]` exists, a bare `pio run` also tries to build the
host environment from `src/` — which is Arduino code — and fails.

**The board-free compile check needs both flags.** `--without-uploading` *alone* builds and then hangs
indefinitely on serial-port detection when no board is attached. With `--without-testing` as well, it prints
`[SKIPPED]` on success — that means the build succeeded and only the *test stage* was skipped, not that
anything went wrong. A failure prints `[ERRORED]` with the compiler diagnostic. Judge that command by the
absence of `[ERRORED]`, not by the presence of `[PASSED]`.

## Architecture

- `platformio.ini` defines two environments. `[env:nodemcu-32s]` is the real firmware: `espressif32` platform,
  Arduino framework, `test_filter = test_signing`. `[env:native]` is a host-only build for the pure
  wire-format module: `platform = native`, `test_filter = test_wireformat`, plus `lib_ignore` and
  `-Ilib/Sensors` (the long comment in the file explains why both halves of that pair are needed — don't
  delete either). `lib_deps` for the board: **`bblanchon/ArduinoJson@7.4.3` pinned exactly, not `^7`**,
  because both environments must resolve the *same* ArduinoJson — a float-formatting change between two 7.x
  releases would make them serialize the same reading differently, and the host suite would stop proving
  anything about what the board publishes. Also espMqttClient, `tzapu/WiFiManager` (captive-portal
  provisioning), and (for the DS18B20) `paulstoffregen/OneWire` + `milesburton/DallasTemperature`.
- `src/main.cpp` is the firmware entry point. `provisioning::loop()` runs on every pass regardless of
  provisioning state; sensor reads and `uplink::loop()` only start once `provisioning::isProvisioned()` is
  true. It reads the sensors on each interval (only after NTP has synced, so every sample has a real
  timestamp).
- `lib/Provisioning/`: see "Field provisioning" above. Owns the WiFiManager instance, the BOOT-button and
  WiFi-outage triggers for reopening the setup hotspot, and the `unit` NVS namespace holding `device_id` /
  `device_secret`.
- `lib/Sensors/`: `sensors::readAll()` returns a `SensorSample` (temperature °C).
  - **Temperature:** real DS18B20 driver, OneWire bus on GPIO 4. The probe's data line needs a ~4.7kΩ
    pull-up to 3V3 if the module doesn't already have one built in. `sensors::begin()` logs a warning if no
    DS18B20 is found on the bus at boot (check wiring/pull-up if that happens).
  - `NAN` for any parameter means "no reading" — it's dropped from the upload rather than sent as a fake `0`.
  - `Sensors.h` is deliberately Arduino-free (the rule is recorded in the header) so `[env:native]` can
    compile it; `Sensors.cpp` is free to depend on Arduino because it's never built natively.
- `lib/WireFormat/`: the pure module that owns every byte the backend verifies — the topic string, the
  ISO-8601 timestamp, the JSON body, the signed input (`"<topic>\n<body>"`), lowercase hex, and the
  `v1.<sig>.<body>` framing. It has no `Arduino.h`, no mbedTLS and no MQTT, which is what lets it compile on
  the host and be checked against the backend's fixture without a board. It exposes **no signing function at
  all**, so the HMAC is unreachable from here by construction; the HMAC itself stays in `lib/Uplink/` because
  mbedTLS has no host build.
- `lib/Uplink/`: a 120-sample ring buffer published with espMqttClient (`UseInternalTask::NO`, so
  `uplink::loop()` drives it). Since the wire-format extraction it holds only the mbedTLS HMAC, the buffer
  and the transport — the published bytes come from `wire::buildBody()` / `wire::frame()`.
  - **Topic:** `truaquality/v1/devices/<DEVICE_ID>/readings`, QoS 1.
  - **Batching:** up to 10 samples per message, one message in flight at a time.
  - **Acknowledgement:** a batch leaves the buffer only on its PUBACK. On disconnect, or after 15 s without an
    ack, it's republished, and the backend skips the duplicates.
  - **Signed payload (must match `backend/src/lib/deviceMessages.ts` exactly):**
    `v1.<lowercase hex HMAC-SHA256(DEVICE_SECRET, "<topic>\n<body>")>.<body>`, assembled by `wire::` and
    signed by `signBody()` in `Uplink.cpp`.
    - Signed because HiveMQ's free tier can't limit an MQTT login to its own topics.
    - The body is `{"firmwareVersion", "samples": [{"recordedAt": ISO-8601 UTC, "values": {...}}]}`.
    - JSON parameter keys (currently just `temperature`) must match the backend's
      `PARAMETER_BOUNDS`.
  - **TLS:** verified against ISRG Root X1 (`lib/Uplink/RootCa.h`, Let's Encrypt, valid until 2035), the root
    of HiveMQ Cloud's certificates.
- `include/` is for project header files shared across `src/` files.
- `test/` holds the PlatformIO Unit Testing (Unity) suites:
  - `test/test_wireformat/` — native, `[env:native]`. Byte-for-byte parity with the backend's golden vectors,
    entry point `int main()`.
  - `test/test_signing/` — on-device, `[env:nodemcu-32s]`. The real `mbedtls_md_hmac` against the vectors'
    signatures, entry point `setup()`/`loop()`. It never prints a secret or a full payload: serial is
    unencrypted.
  - `test/golden/` — the vectors, mirrored from `backend/src/lib/__fixtures__/signing-vectors.v1.json` and
    kept byte-identical (`signing-vectors.v1.json` is the copy, `signing_vectors.h` is what the suites
    compile against). **The mirroring is one-directional: the backend is the authority.** If a firmware test
    fails, the firmware is wrong — never regenerate the vectors to make it pass. The directory has no `test_`
    prefix on purpose, so PlatformIO doesn't collect it as a suite.
