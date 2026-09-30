#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "TurbidityMath.h"

// Every calibration decision the field portal makes, and nothing that touches hardware. Like TurbidityMath.h
// this header is deliberately free of the Arduino core header and of the Sensors library, so [env:native]
// compiles it and test_turbidity_math pins the capture window, the stability rule, the refusal order, the
// manual-entry parsing and the timestamp rule in about a second on the laptop. The portal (lib/Provisioning) and the bench
// then share one rule instead of two copies that drift; the board code stays thin wiring around it.
//
// Symptom, if the rule is broken here: `pio test -e native` failing with a missing-Arduino-core-header error
// that names *this* file. It must not include the Sensors library's header either — lib/Provisioning
// includes this one, and LDF would then chase Sensors across libraries. Header-only on purpose: it lives in
// lib/TurbidityMath, which needs no .cpp and must never be added to [env:native]'s lib_ignore (that would
// strip its include path).
//
// C++11 only: the ESP32 Arduino core compiles at gnu++11 and will compile this header once the portal
// includes it, so no C++17 clamp helper, no std::size, no structured bindings, no multi-statement constexpr
// functions. std::min/std::max and std::sort are the C++11 tools used instead.
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

    // D-01. Copies into a local buffer before sorting: the portal passes its live ring, and sorting it in place
    // would scramble the order new samples are written in (T-05-07). A count above the buffer is refused rather
    // than written past it; an empty window or any NAN has no honest median.
    inline float medianOf(const float *values, size_t count)
    {
      if (values == nullptr || count == 0 || count > CAPTURE_WINDOW_SAMPLES)
      {
        return NAN;
      }
      float copy[CAPTURE_WINDOW_SAMPLES];
      for (size_t i = 0; i < count; i++)
      {
        if (std::isnan(values[i]))
        {
          return NAN;
        }
        copy[i] = values[i];
      }
      std::sort(copy, copy + count);
      if (count % 2 == 0)
      {
        return (copy[count / 2 - 1] + copy[count / 2]) / 2.0f;
      }
      return copy[count / 2];
    }

    // D-03: max minus min. NAN for nothing to measure or a lost sample, so a gap never reads as "steady".
    inline float spreadOf(const float *values, size_t count)
    {
      if (values == nullptr || count == 0)
      {
        return NAN;
      }
      float lo = values[0];
      float hi = values[0];
      for (size_t i = 0; i < count; i++)
      {
        if (std::isnan(values[i]))
        {
          return NAN;
        }
        lo = std::min(lo, values[i]);
        hi = std::max(hi, values[i]);
      }
      return hi - lo;
    }

    // The capture decision. Order matters and the first match wins (T-05-06):
    //   1. SignalLost — a window that is not exactly CAPTURE_WINDOW_SAMPLES long, or holds a NAN, means the
    //      signal is gone or the capture was cut short, so nothing else is judged (fail closed).
    //   2. Unstable (D-03) — the median of a moving window means nothing, so stability is judged before the
    //      plausibility window; an admin told "implausible" for a probe that is still settling would go looking
    //      for the wrong fault.
    //   3. Implausible (D-04) — the rounded median must pass turbidity::isPlausibleClearWaterMv, the same window
    //      the stored value is later checked against, so the portal can never store what classify() refuses.
    //   4. Accepted.
    inline CaptureResult evaluateCapture(const float *sensorMv, size_t count)
    {
      if (sensorMv == nullptr || count != CAPTURE_WINDOW_SAMPLES)
      {
        return CaptureResult{CaptureOutcome::SignalLost, 0, NAN};
      }
      const float spread = spreadOf(sensorMv, count);
      if (std::isnan(spread))
      {
        return CaptureResult{CaptureOutcome::SignalLost, 0, NAN};
      }
      if (spread > CAPTURE_MAX_SPREAD_MV)
      {
        return CaptureResult{CaptureOutcome::Unstable, 0, spread};
      }
      const long rounded = lroundf(medianOf(sensorMv, count));
      // Range-guarded before the cast: a float outside uint16_t would otherwise wrap into the window.
      if (rounded < 0 || rounded > 65535L)
      {
        return CaptureResult{CaptureOutcome::Implausible, 0, spread};
      }
      const uint16_t medianMv = static_cast<uint16_t>(rounded);
      if (!isPlausibleClearWaterMv(medianMv))
      {
        return CaptureResult{CaptureOutcome::Implausible, medianMv, spread};
      }
      return CaptureResult{CaptureOutcome::Accepted, medianMv, spread};
    }

    // D-06: the live indicator uses the capture's own spread rule on whatever part of the window is filled, so
    // "steady" on screen means a capture taken now would pass the stability check. Unstable is reported as soon
    // as the filled part is too wide — waiting for 20 samples to say so would only delay the admin.
    inline LiveState liveState(const float *window, size_t filled)
    {
      if (window == nullptr || filled == 0)
      {
        return LiveState::Settling;
      }
      const size_t n = std::min(filled, CAPTURE_WINDOW_SAMPLES);
      const float spread = spreadOf(window, n);
      if (std::isnan(spread))
      {
        return LiveState::SignalLost;
      }
      if (spread > CAPTURE_MAX_SPREAD_MV)
      {
        return LiveState::Unstable;
      }
      if (n < CAPTURE_WINDOW_SAMPLES)
      {
        return LiveState::Settling;
      }
      return LiveState::Steady;
    }

    // D-09 / T-05-05: the portal's manual-entry field. Blank (after trimming spaces and tabs) means Keep, never
    // 0 — a submitted empty field must not wipe the calibration to "never calibrated". Digits only, checked one
    // character at a time: the C library's long parser would accept a sign, leading spaces inside and trailing junk. At
    // most 5 digits, accumulated in an unsigned long so nothing can overflow; above 65535 cannot fit the stored
    // uShort, so it is Implausible with mv 0 rather than wrapped. Then the same D-04 window as a capture.
    inline ManualResult parseManualMv(const char *text)
    {
      if (text == nullptr)
      {
        return ManualResult{ManualKind::Keep, 0};
      }
      size_t begin = 0;
      size_t end = std::strlen(text);
      while (begin < end && (text[begin] == ' ' || text[begin] == '\t'))
      {
        begin++;
      }
      while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
      {
        end--;
      }
      if (begin == end)
      {
        return ManualResult{ManualKind::Keep, 0};
      }
      if (end - begin > 5)
      {
        return ManualResult{ManualKind::NotANumber, 0};
      }
      unsigned long value = 0;
      for (size_t i = begin; i < end; i++)
      {
        if (!std::isdigit(static_cast<unsigned char>(text[i])))
        {
          return ManualResult{ManualKind::NotANumber, 0};
        }
        value = value * 10UL + static_cast<unsigned long>(text[i] - '0');
      }
      if (value > 65535UL)
      {
        return ManualResult{ManualKind::Implausible, 0};
      }
      const uint16_t mv = static_cast<uint16_t>(value);
      if (!isPlausibleClearWaterMv(mv))
      {
        return ManualResult{ManualKind::Implausible, mv};
      }
      return ManualResult{ManualKind::Set, mv};
    }

    // D-08 / T-05-08: the stored calibration date. An unsynced clock (at or below MIN_VALID_EPOCH), a negative
    // value or one that does not fit the stored uint32 all become 0, "date unknown" — never a plausible-looking
    // 1970 date. One expression, so it could become a C++11 constexpr function without a rewrite.
    inline uint32_t calibrationStamp(long long epochSeconds)
    {
      return (epochSeconds > static_cast<long long>(MIN_VALID_EPOCH) && epochSeconds <= 4294967295LL)
                 ? static_cast<uint32_t>(epochSeconds)
                 : 0;
    }

    // Tokens the portal's JSON speaks. Every enumerator is listed and there is no default, so a new enumerator
    // is a -Wswitch warning; the trailing return is a fault token, never a success one.
    inline const char *captureOutcomeToken(CaptureOutcome outcome)
    {
      switch (outcome)
      {
      case CaptureOutcome::Accepted:
        return "accepted";
      case CaptureOutcome::SignalLost:
        return "signal_lost";
      case CaptureOutcome::Unstable:
        return "unstable";
      case CaptureOutcome::Implausible:
        return "implausible";
      }
      return "signal_lost";
    }

    inline const char *liveStateToken(LiveState state)
    {
      switch (state)
      {
      case LiveState::Settling:
        return "settling";
      case LiveState::Steady:
        return "steady";
      case LiveState::Unstable:
        return "unstable";
      case LiveState::SignalLost:
        return "signal_lost";
      }
      return "signal_lost";
    }

    inline const char *manualKindToken(ManualKind kind)
    {
      switch (kind)
      {
      case ManualKind::Keep:
        return "keep";
      case ManualKind::Set:
        return "set";
      case ManualKind::NotANumber:
        return "not_a_number";
      case ManualKind::Implausible:
        return "implausible";
      }
      return "not_a_number";
    }
  }
}
