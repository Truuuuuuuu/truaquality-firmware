#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "TurbidityMath.h"

// Every calibration decision the field portal makes, and nothing that touches hardware. Like TurbidityMath.h
// this header is deliberately free of the Arduino core header and of Sensors.h, so [env:native] compiles it
// and test_turbidity_math pins the capture window, the stability rule, the refusal order, the manual-entry
// parsing and the timestamp rule in about a second on the laptop. The portal (lib/Provisioning) and the bench
// then share one rule instead of two copies that drift; the board code stays thin wiring around it.
//
// Symptom, if the rule is broken here: `pio test -e native` failing with a missing-Arduino-core-header error
// that names *this* file. It must not include Sensors.h either — lib/Provisioning includes it, and LDF would
// then chase Sensors across libraries. Header-only on purpose: it lives in lib/TurbidityMath, which needs no
// .cpp and must never be added to [env:native]'s lib_ignore (that would strip its include path).
//
// C++11 only: the ESP32 Arduino core compiles at gnu++11 and will compile this header once the portal
// includes it, so no std::clamp, no std::size, no structured bindings, no multi-statement constexpr functions.
namespace turbidity
{
  namespace cal
  {
    // D-01: one capture is about 20 one-second bursts, and the decision is taken on their median — never on a
    // single burst, which on the bench rig could land on either side of the ~120 mV two-level supply jump.
    constexpr size_t CAPTURE_WINDOW_SAMPLES = 20;

    // D-03: SENSOR-side millivolts (the bench CSV's sensor_mv column), measured over a 1 Hz cadence. A window
    // whose max minus min exceeds this is a moving probe or settling water. Applying the number to pin mV, or
    // to a different cadence, silently changes what it means.
    constexpr float CAPTURE_MAX_SPREAD_MV = 150.0f;

    // D-08: mirrors lib/Uplink/Uplink.cpp's MIN_VALID_EPOCH (Provisioning must not include Uplink). A clock at
    // or below this has not synced over NTP, so the calibration date is unknown rather than 1970.
    constexpr uint32_t MIN_VALID_EPOCH = 1700000000UL;

    enum class CaptureOutcome : uint8_t
    {
      Accepted,
      SignalLost,
      Unstable,
      Implausible,
    };

    struct CaptureResult
    {
      CaptureOutcome outcome;
      uint16_t medianMv;
      float spreadMv;
    };

    enum class LiveState : uint8_t
    {
      Settling,
      Steady,
      Unstable,
      SignalLost,
    };

    enum class ManualKind : uint8_t
    {
      Keep,
      Set,
      NotANumber,
      Implausible,
    };

    struct ManualResult
    {
      ManualKind kind;
      uint16_t mv;
    };

    // RED stubs: compile, give wrong answers.
    inline float medianOf(const float *values, size_t count)
    {
      (void)values;
      (void)count;
      return NAN;
    }

    inline float spreadOf(const float *values, size_t count)
    {
      (void)values;
      (void)count;
      return NAN;
    }

    inline CaptureResult evaluateCapture(const float *sensorMv, size_t count)
    {
      (void)sensorMv;
      (void)count;
      return CaptureResult{CaptureOutcome::SignalLost, 0, NAN};
    }

    inline LiveState liveState(const float *window, size_t filled)
    {
      (void)window;
      (void)filled;
      return LiveState::Settling;
    }

    inline ManualResult parseManualMv(const char *text)
    {
      (void)text;
      return ManualResult{ManualKind::Keep, 0};
    }

    inline uint32_t calibrationStamp(long long epochSeconds)
    {
      (void)epochSeconds;
      return 0;
    }

    inline const char *captureOutcomeToken(CaptureOutcome outcome)
    {
      (void)outcome;
      return "signal_lost";
    }

    inline const char *liveStateToken(LiveState state)
    {
      (void)state;
      return "signal_lost";
    }

    inline const char *manualKindToken(ManualKind kind)
    {
      (void)kind;
      return "not_a_number";
    }
  }
}
