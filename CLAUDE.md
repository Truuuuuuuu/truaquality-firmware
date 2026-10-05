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
end-to-end on a real ESP32 unit — readings land in the database and dashboard.

**Turbidity is the second parameter, bench-characterized on one rig.** The module is the DFRobot
**SEN0189** family (analog, 5 V supply, 0–4.5 V output). It is read on **GPIO34** (ADC1_CH6 — ADC2 belongs to
WiFi, and GPIO34 is input-only so no internal pull-up can pull the divider off its ratio) through a
**10 kΩ / 12 kΩ divider** with a 100 nF bypass to GND, and converted with the vendor quadratic evaluated on a
ratio-normalized voltage. The 03-06 bench session (`.planning/phases/03-turbidity-sensor-read-bench-characterization/03-BENCH-RECORD.md`)
replaced the fault floor (**250 mV** at the pin; unplugged reads 142 mV, the muddiest sample never below 357 mV)
and the plausible clear-water window (**2870–3580 mV** sensor-side) in `lib/TurbidityMath/TurbidityMath.h`; the
window is a property of that rig's USB-fed 5 V supply, so a unit with a different supply needs its own check.
`HIGH_VOLTAGE_MARGIN` (0.15) was checked against the bench and kept. **`NTU_ROUND_STEP` (0.1) and the vendor
curve are still PROVISIONAL and unvalidated** — there was no turbidimeter, so the NTU numbers are estimates,
and the clear-water signal flips between two levels about 120 mV apart (cause unconfirmed), so clear water can
read anywhere from 0 to several hundred NTU on that rig. **Calibrate from the setup portal's Calibrate screen**
(or, on the bench image, `cal capture`): both take the **median of 20 one-second samples** and refuse an
unstable signal (spread over 150 mV sensor-side), an implausible one (median outside 2870–3580 mV) or a lost one,
because a single burst can land on either of the two levels and a turbid or disconnected probe must never become
the reference. `cal set <mV>` and the portal's manual field go through the same window. Re-calibrate if the
container or probe position changes. Water muddier than the curve covers is clamped to **3000 NTU**
(`NTU_CEILING`, below the curve's 2.5 V floor): a real reading, never a fault — a fault omits the value. The
high-side fault is `over_range` (normalized voltage more than 15 % above 4.2 V). A probe out of the water reads
about 2119 mV and so reports thousands of NTU; only the portal capture refuses it. The *alert thresholds* are a separate matter: they are the backend's, never
the firmware's.

Adding a parameter means a field on `SensorSample`, a reader in `lib/Sensors/Sensors.cpp`, an `addValue` line
in `lib/WireFormat/WireFormat.cpp`, and the same id in the backend's `PARAMETER_BOUNDS` and the frontend's
`PARAMETERS`. Since 0.6.0 it also means a status enum + `sensors::statusToken()` overload in `Sensors.h`, a
status field on `SensorSample`, and a key in `wire::buildBody`'s `sensors` object — plus the backend's
`SENSOR_STATUSES` if the sensor needs a token that list doesn't have yet (an unknown token rejects the whole
message).

### Build-time configuration

Copy `include/unit_config.example.h` to `include/unit_config.h` (gitignored) and fill in — every unit gets the
same file and the same compiled image now, there's no per-unit build:
- HiveMQ: `MQTT_HOST`, `MQTT_PORT`, and one MQTT credential (`MQTT_USERNAME` / `MQTT_PASSWORD`) shared by all
  units, created in the HiveMQ console.
- `SETUP_AP_PASSWORD`: the WPA2 password for every unit's setup hotspot (see "Field provisioning" below).
  Shared by all units, kept in the administrator's handbook rather than printed on the enclosure.
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
  with `SETUP_AP_PASSWORD`. Joining it from a phone pops up a captive-portal setup page with three screens:
  "Configure WiFi" (network + password, saved by WiFiManager into the ESP32's own WiFi NVS), "Setup"
  (Device ID + Device secret, copied from the Devices page's registration/rotation dialog, saved into this
  module's own `unit` NVS namespace) and "Calibrate" (turbidity clear-water reference, below). The onboard LED
  (GPIO 2) blinks fast the whole time; the unit takes no readings until WiFi and identity are both saved, at
  which point it restarts. Calibration is not required to finish setup.
- **Provisioned:** the unit joins its saved WiFi and runs exactly as before.
- **Field order: set up WiFi and identity first, then calibrate** from a BOOT-hold portal on the provisioned
  unit. Before setup the radio state lowers clean water below 2870 mV and the capture is refused; the
  2870–3580 mV window holds only for the USB-fed 5 V rig it was measured on (D-05/D-17).
- **Calibrate screen** (`/cal`, a menu button added through WiFiManager's custom menu HTML; page in
  `lib/Provisioning/CalibrationPage.h`): shows the live sensor-side mV and NTU about once a second with a
  settling / steady / unstable / signal lost note, the stored clear-water value and its last-calibrated date, a
  **Capture** button that runs the 20 s median capture and reports the saved median or the refusal reason, and a
  manual field where **blank keeps the stored value**. It polls `GET /cal/status` (fixed key set, no identity,
  secret, SSID or MAC); `POST /cal/capture` and `POST /cal/set` are the only mutations (a manual save during a
  running capture answers 409). **Calibrating never restarts the unit and never blocks WiFi/identity setup** —
  an uncalibrated unit simply reports temperature only.
- **Restart rule:** the unit restarts about 2 s after WiFi or device identity is
  **saved in the current portal session**, never merely because the portal is open. Before the 05-01 fix, a BOOT-hold on an installed
  (provisioned) unit restarted it about 0.5 s after the hotspot opened, with nothing saved, which made
  recalibrating an installed unit impossible.
- **Re-entering setup on a working unit** (pond router replaced, secret rotated on the Devices page,
  recalibration, …): hold the BOOT button (GPIO 0) for 5 seconds. The hotspot reopens for 5 minutes and **stays
  open while a phone is joined** (`setAPClientCheck`); tap Exit or walk away when done. Sampling and the MQTT
  buffer keep running the whole time. **Press it after power-on, not during** — held down while powering up,
  GPIO 0 instead drops the chip into its ROM download mode. A waterproof button wired from GPIO 0 to GND on the
  enclosure needs no code change.
- **Web OTA is blocked.** The menu is set explicitly (`wifi`, `param`, `custom`, `info`, `exit`) with no
  Update, Erase or Restart, and WiFiManager's always-registered `/update`, `/u`, `/erase` and `/restart` URLs
  are shadowed by handlers registered first that return **404**, because anyone with the shared hotspot
  password could otherwise flash arbitrary firmware or wipe the unit.
- **Automatic fallback:** if the saved WiFi is unreachable for 10 straight minutes, the hotspot opens on its
  own with a 5-minute timeout (`handleOutage()` → `openPortal(TIMED_PORTAL_SECONDS)`). **While it is open the
  unit does not retry its WiFi** — WiFiManager turns the station interface off and `ensureWiFi()` in
  `src/main.cpp` returns early. Because `setAPClientCheck(true)` applies to every portal, the timeout keeps
  resetting **while any phone is joined**, so a joined or auto-rejoining phone keeps a provisioned unit off its
  WiFi indefinitely; with nobody joined it closes after 5 minutes, retries, and reopens 10 minutes later if the
  outage continues. Known issue CR-01 (`.planning/phases/05-field-calibration-portal/05-REVIEW.md`): the fix
  (client check limited to the BOOT-hold portal plus a maximum open time) is deferred until after the demo,
  before field install; update this bullet when it lands.
- A plain `pio run -t upload` leaves NVS (so the saved WiFi/identity) alone. Only `pio run -t erase` wipes it —
  use that for "fresh unit" testing.
- **NVS and flash are not encrypted** (no flash encryption / secure boot: both burn one-way eFuses, out of
  capstone scope). Anyone holding a unit can read its `DEVICE_SECRET`, WiFi password and the shared HiveMQ
  credential compiled from `unit_config.h`. The stolen-unit procedure (disable, rotate the device secret, rotate
  the broker credential) is in root `SECURITY_PERFORMANCE_AUDIT.md`, finding S8.

## Commands

This project uses PlatformIO. If the `pio` CLI is not on `PATH` in the shell (it isn't in this environment
by default), invoke it via the PlatformIO Core install, typically at `~/.platformio/penv/bin/pio`, or use
the PlatformIO IDE extension in VS Code.

- Build: `pio run -e nodemcu-32s`
- Upload to the connected board: `pio run -t upload`
- Serial monitor: `pio device monitor`
- Clean build artifacts: `pio run -t clean`
- Build the bench instrument (never for a deployed unit): `pio run -e nodemcu-32s-bench`
- Upload the bench instrument: `pio run -e nodemcu-32s-bench -t upload`
- Bench serial session (115200), where `cal show`, `cal capture` and `cal set <mV>` are typed:
  `pio device monitor -e nodemcu-32s-bench`. `cal capture` is the same 20 s median capture the portal runs and
  prints its result (saved median or refusal reason) when it finishes; the CSV keeps its own 1 s rows meanwhile,
  as an independent cross-check of the capture median.
- Host suites (no board needed): `pio test -e native` (expect 73/73 on fw 0.6.1) — this now runs **two** suites, `test_wireformat` and
  `test_turbidity_math`, because `[env:native]`'s `test_filter` lists both by name. A native suite missing
  from that filter is skipped silently, so the run looks green while proving nothing about it.
- On-device HMAC suite (needs a connected ESP32): `pio test -e nodemcu-32s -f test_signing`
- Compile the on-device suite without a board:
  `pio test -e nodemcu-32s -f test_signing --without-uploading --without-testing`
- Sync the golden vectors from the backend: `node scripts/sync-golden-vectors.mjs`
- Check the golden vectors for drift: `node scripts/sync-golden-vectors.mjs --check`

**Always pass `-e` to `pio run`.** `platformio.ini` sets `default_envs = nodemcu-32s`, so a bare `pio run` and
the VS Code upload arrow now target the real firmware only. Before that default existed they ran every
environment: `[env:native]` fails (it can't build the Arduino `src/`), and `[env:nodemcu-32s-bench]` would try
to flash the calibration image. Still name `native` or `nodemcu-32s-bench` explicitly with `-e` when you want them.

**The board-free compile check needs both flags.** `--without-uploading` *alone* builds and then hangs
indefinitely on serial-port detection when no board is attached. With `--without-testing` as well, it prints
`[SKIPPED]` on success — that means the build succeeded and only the *test stage* was skipped, not that
anything went wrong. A failure prints `[ERRORED]` with the compiler diagnostic. Judge that command by the
absence of `[ERRORED]`, not by the presence of `[PASSED]`.

## Architecture

- `platformio.ini` defines three environments. `[env:nodemcu-32s]` is the real firmware: `espressif32`
  platform, Arduino framework, `test_filter = test_signing`. `[env:native]` is a host-only build for the pure
  modules: `platform = native`, `test_filter = test_wireformat, test_turbidity_math`, plus `lib_ignore` and
  `-Ilib/Sensors` (the long comment in the file explains why both halves of that pair are needed — don't
  delete either). `lib_deps` for the board: **`bblanchon/ArduinoJson@7.4.3` pinned exactly, not `^7`**,
  because both environments must resolve the *same* ArduinoJson — a float-formatting change between two 7.x
  releases would make them serialize the same reading differently, and the host suite would stop proving
  anything about what the board publishes. Also espMqttClient, `tzapu/WiFiManager` (captive-portal
  provisioning), and (for the DS18B20) `paulstoffregen/OneWire` + `milesburton/DallasTemperature`.
  `[env:nodemcu-32s-bench]` is `[env:nodemcu-32s]` (`extends`) plus `-DTURBIDITY_BENCH=1`, and exists only for
  bench characterization: it compiles in the `cal show` / `cal capture` / `cal set <mV>` serial commands and
  the CSV log, both wrapped in `#if TURBIDITY_BENCH` in `src/main.cpp` so the shipping image cannot contain
  them (the check is `strings .pio/build/<env>/firmware.elf | grep -c 'turbidity-bench'` — 0 for
  `nodemcu-32s`, non-zero for `nodemcu-32s-bench`). **A unit flashed from the bench environment must not be
  installed at a pond:** anyone with a USB cable could rewrite its calibration. The CSV runs on its own
  `BENCH_SAMPLE_INTERVAL_MS` (1 s), separate from `REPORT_INTERVAL_MS`, and is emitted from above the
  provisioning gate so an unprovisioned or offline unit still produces data — a 5-minute run is about 300
  rows. The uplink cadence itself is unchanged: a bench unit still publishes on the normal schedule.
- `src/main.cpp` is the firmware entry point. `provisioning::loop()` runs on every pass regardless of
  provisioning state; sensor reads and `uplink::loop()` only start once `provisioning::isProvisioned()` is
  true. It reads the sensors on each interval (only after NTP has synced, so every sample has a real
  timestamp).
- `lib/Provisioning/`: see "Field provisioning" above. Owns the WiFiManager instance, the BOOT-button and
  WiFi-outage triggers for reopening the setup hotspot, the `unit` NVS namespace holding `device_id` /
  `device_secret` / `turb_clear_mv` / `turb_cal_at`, and the calibration capture engine (a non-blocking 1 Hz
  sampler that runs only while the portal is open or a capture is running; a portal capture is cancelled when
  the hotspot closes). Sensor access is injected through `Config` callbacks so this library never includes the
  Sensors library's header.
- `lib/Sensors/`: `sensors::readAll()` returns a `SensorSample` (temperature °C and turbidity NTU).
  - **Turbidity:** SEN0189-family analog module on **GPIO34** (ADC1), fed through a 10 kΩ / 12 kΩ divider
    because the sensor swings to 4.5 V and the ESP32's ADC tops out near 3.3 V. One burst is 64 one-shot
    samples, trimmed and averaged by `lib/TurbidityMath/`, then converted against the unit's stored
    clear-water reference. **Since firmware 0.6.1 the REPORTED reading is not one burst:** it is the median of
    the last 5 bursts (`turbidity::REPORT_BURSTS`), taken one every `REPORT_INTERVAL_MS / 5` (~6 s at 30 s) by
    `sensors::pollTurbidity()`, which `loop()` calls on every pass above the provisioning gate. Why: on
    2026-10-03 a unit in clean water reported isolated 260.1 / 408.1 NTU spikes when a single burst landed on
    the low level of the two-level signal. Rules (all in `TurbidityMath.h`, pinned by the `test_window_*`
    native cases): the median is taken only when a **strict majority** of the window has signal (one failed
    burst is outvoted), otherwise the newest failed burst is reported so a dead pin still says `no_signal`
    (recovery after a replug takes up to ~3 bursts, ~18 s); an even valid count drops the oldest valid burst;
    when the window holds fewer than 3 bursts at report time (first report after boot) `readAll()` tops it up
    with immediate bursts. **Known limit:** a low-level dwell longer than about 12 s (3 of the 5 bursts) still
    gets through — the bench saw dwells of 1-17 s — and the hardware cause is not fixed. Calibration capture,
    `cal show` and the bench CSV still take **one burst per sample** via `readTurbidityMillivolts()`, and window
    bursts never write `lastTurbidityDiagnostics()`. It is `NAN` when the pin sits under the fault floor (signal or 5 V unplugged) **and
    when the unit is uncalibrated** — either way the parameter is simply absent from the upload. The
    reference is one `uShort` of sensor-side millivolts in NVS, namespace `unit`, key `turb_clear_mv`, owned
    by `lib/Provisioning/` (0 = never calibrated, and the boot log says so once). Calibration is deliberately
    **not** part of `isProvisioned()`: an uncalibrated unit still finishes setup and keeps reporting
    temperature. Beside it, `turb_cal_at` (`uInt`) holds the Unix seconds (UTC, from NTP) when the value was
    stored; **0 = date unknown**, which is what a unit calibrated before Phase 5 or with no NTP sync yet shows.
    `provisioning::storeTurbidityClearWaterMv()` is the **only writer of both keys**: it refuses an implausible
    value and logs the reason, writes the value then the stamp, and applies the new reference live (no
    restart), so no caller can store without applying.
  - **Temperature:** real DS18B20 driver, OneWire bus on GPIO 4. The probe's data line needs a ~4.7kΩ
    pull-up to 3V3 if the module doesn't already have one built in. `sensors::begin()` logs a warning if no
    DS18B20 is found on the bus at boot (check wiring/pull-up if that happens).
  - **Status (since 0.6.0):** each read also yields a status, sent as the body's `sensors` object.
    Temperature: `ok` / `not_found` (no DS18B20 answered — the bus is re-scanned with `begin()` on *every*
    read, because `getDeviceCount()` is only a cached count, so a replugged probe recovers without a reboot) /
    `disconnected` (a -127 read) / `power_on_value` (exactly 85.0 °C, the DS18B20's power-on-reset value;
    now NAN — firmware before 0.6.0 published it as a real reading). Turbidity: `ok` / `no_signal` /
    `uncalibrated` / `over_range`, from `turbidity::classify` on the same burst the NTU came from. **A non-ok
    status always means the value is omitted**, enforced in `readAll()`. A status change logs one
    `[sensors] <parameter>: <token>` line; an unchanged one logs nothing.
  - `NAN` for any parameter means "no reading" — it's dropped from the upload rather than sent as a fake `0`.
  - **pH (wire contract only, Phase 9):** `SensorSample` carries `float ph` (directly after `turbidity`) and
    `PhStatus phStatus`, so every literal spells out **all six fields** — an old four-field literal fails to
    compile instead of sending `ph` 0. There is no pH driver until Phase 10: `readAll()` returns `NAN` +
    `PhStatus::NotFitted`, and `wire::buildBody` omits both the `ph` value and `sensors.ph` for `NotFitted`, so
    a 0.6.1 unit's bytes are unchanged. `statusToken(PhStatus)` reuses existing backend tokens only
    (`ok` / `no_signal` / `uncalibrated` / `over_range`; `NotFitted` maps to a fault token, never `ok`).
  - `Sensors.h` is deliberately Arduino-free (the rule is recorded in the header) so `[env:native]` can
    compile it; `Sensors.cpp` is free to depend on Arduino because it's never built natively.
- `lib/TurbidityMath/`: header-only and Arduino-free — the burst trim, the divider scaling, the vendor curve,
  the clear-water plausibility window and every fault decision, pinned by `test/test_turbidity_math/` on the
  host. `classify()` is the single home of the fault decisions (no signal, uncalibrated, over range) and
  `ntuFromPinMv()` starts by calling it, so the status a unit reports and whether it sends a value can't
  disagree — `test_classify_ok_exactly_when_ntu_is_finite` sweeps that equivalence. This header must **not**
  include `Sensors.h` (`lib/Provisioning` includes it, and LDF would then chase Sensors across libraries),
  which is why `turbidity::Status` is its own enum, mapped onto `TurbidityStatus` by a switch in `Sensors.cpp`.
  It is deliberately **absent from `[env:native]`'s `lib_ignore`**: a header-only library needs no
  entry there, and `lib_ignore` would strip its include path along with its (non-existent) sources. Its
  PROVISIONAL constants are the ones the bench session replaces.
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
    - The body is `{"firmwareVersion", "wifiSsid"?, "diag"?, "sensors"?, "samples": [{"recordedAt": ISO-8601
      UTC, "values": {...}}]}`, in that key order (signed bytes). `wifiSsid` (`WiFi.SSID()` at publish time,
      since 0.5.0) is omitted when empty.
    - **`diag` (since 0.6.0)**, per message: `{rssi, uptimeS, resetReason, freeHeap, queued}` in that order.
      `rssi` from `WiFi.RSSI()`; `uptimeS` from `esp_timer_get_time()`, **not `millis()`** — `millis()` wraps
      after ~49.7 days and a falling uptime is what the backend reads as a REBOOT; `resetReason` is a token
      mapped from `esp_reset_reason()` once in `begin()`; `freeHeap` from `ESP.getFreeHeap()`; `queued` is the
      buffered count (`uplink::queuedSamples()`). All integers, and **clamped in `wire::` to the backend's
      ranges** (rssi -127..0, queued 0..120, the rest 0..INT32_MAX), because one out-of-range number makes the
      backend reject the whole message and its readings with it.
    - **`sensors` (since 0.6.0)**: the status tokens of the **newest** sample in the batch — the backend
      stores "status at the newest sample".
    - JSON parameter keys (`temperature`, and `turbidity` when the unit has a reading for it) must match the
      backend's `PARAMETER_BOUNDS`.
  - **TLS:** verified against ISRG Root X1 (`lib/Uplink/RootCa.h`, Let's Encrypt, valid until 2035), the root
    of HiveMQ Cloud's certificates.
- `include/` is for project header files shared across `src/` files.
- `test/` holds the PlatformIO Unit Testing (Unity) suites:
  - `test/test_wireformat/` — native, `[env:native]`. Byte-for-byte parity with the backend's golden vectors,
    entry point `int main()`. There are **11** vectors: 8 at firmwareVersion 0.6.0, the eighth being
    `diagnostics-and-sensor-status` (the 0.6.0 `diag` + `sensors` body), then 3 pH vectors at 0.7.0
    (`temperature-turbidity-and-ph`, `ph-batch-with-omissions`, `diagnostics-and-sensor-status-with-ph`; `ph` is
    last in `values` and `sensors`). The suite keeps its **own** `FIRMWARE_VERSION` (0.6.0) and
    `FIRMWARE_VERSION_PH` (0.7.0) constants, matching the versions inside the vectors' signed bodies, so bumping `src/main.cpp` (as 0.6.1 did) does not touch them;
    only a wire-format change needs the vectors regenerated from the backend (which also moves the hand-written
    vector-0 bytes in `test_hex_is_lowercase`).
  - `test/test_turbidity_math/` — native, `[env:native]`. Pins every constant and every fault decision in
    `lib/TurbidityMath/`, entry point `int main()`.
  - `test/test_signing/` — on-device, `[env:nodemcu-32s]`. The real `mbedtls_md_hmac` against the vectors'
    signatures, entry point `setup()`/`loop()`. It never prints a secret or a full payload: serial is
    unencrypted.
  - `test/golden/` — the vectors, mirrored from `backend/src/lib/__fixtures__/signing-vectors.v1.json` and
    kept byte-identical (`signing-vectors.v1.json` is the copy, `signing_vectors.h` is what the suites
    compile against). **The mirroring is one-directional: the backend is the authority.** If a firmware test
    fails, the firmware is wrong — never regenerate the vectors to make it pass. The directory has no `test_`
    prefix on purpose, so PlatformIO doesn't collect it as a suite.
