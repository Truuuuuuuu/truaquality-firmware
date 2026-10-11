#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "PhMath.h"
#include "PhCalibration.h"

// Every decision the pH setup-portal calibration takes (Phase 11): which buffer strings the picker accepts, whether
// a pair of captured points is refused and why, the probe-health verdict, the calibration age, and whether a Save
// still matches what was captured. Nothing here touches hardware or NVS, so test_ph_acceptance pins every rule on
// the host and lib/Provisioning stays thin wiring. Like PhMath.h it is Arduino-free, must not include Sensors.h
// or the turbidity headers, and is C++11 only because the board compiles it at gnu++11: no C++17 clamp helper, no
// std::size, no structured bindings, no if constexpr, no inline variables, no multi-statement constexpr functions.
//
// D-05: every bench-dependent number is a named constant marked "PROVISIONAL until 10-BENCH-RECORD §N", naming
// the bench-record section whose data replaces it. test_ph_acceptance is written against the names, so the
// follow-up swaps values here without rewriting a test.
//
// D-18 / D-23: the slope % used by the verdict is a % of the ASSUMED DFRobot board nominal slope
// (ph::cal::NOMINAL_SENSOR_SLOPE_MV_PER_PH), NOT a % of the Nernst slope — the board gain of this clone is
// unpublished. There is deliberately no upper slope bound until bench data shows what a too-steep slope means.
namespace ph
{
  namespace accept
  {
    // Mirrors Uplink/TurbidityCalibration's MIN_VALID_EPOCH rather than including them (this library must not pull
    // in turbidity or Uplink headers). A clock at or below it has never synced over NTP.
    constexpr uint32_t MIN_VALID_EPOCH = 1700000000UL;

    // D-20: a calibration older than this is shown as overdue (a reminder, never a refusal).
    constexpr uint32_t OVERDUE_AFTER_DAYS = 30;
    constexpr uint32_t SECONDS_PER_DAY = 86400UL;

    // D-10: the buffer picker offers exactly these printed nominals, in hundredths; any other string is refused.
    constexpr size_t PICKER_NOMINAL_COUNT = 6;
    constexpr uint16_t PICKER_NOMINALS[PICKER_NOMINAL_COUNT] = {400, 401, 686, 700, 918, 1001};
    // D-11: the pair the bench calibrates on is preselected.
    constexpr uint16_t DEFAULT_NOMINAL_A = 686;
    constexpr uint16_t DEFAULT_NOMINAL_B = 918;

    // D-09: the ~2 pH rule, judged on the printed nominals (so 6.86 / 7.00 and 9.18 / 10.01 are refused). It is
    // stricter than ph::MIN_BUFFER_GAP_HUNDREDTHS, which stays the storage-level divide-by-zero guard.
    constexpr uint16_t PORTAL_MIN_BUFFER_GAP_HUNDREDTHS = 200;
    static_assert(PORTAL_MIN_BUFFER_GAP_HUNDREDTHS >= ph::MIN_BUFFER_GAP_HUNDREDTHS,
                  "the portal gap must never accept what isValidCalibration refuses");

    // PROVISIONAL until 10-BENCH-RECORD §7: the largest per-buffer sensor-side noise sd seen in a steady cup.
    constexpr float NOISE_FLOOR_SENSOR_MV = 1.0f;
    // D-06: a slope so shallow that the noise alone moves the reading by more than this is useless.
    constexpr float MAX_NOISE_PH = 0.1f;
    // Derived, never typed: the noise floor expressed as a slope (10 mV/pH with today's numbers).
    constexpr float MIN_SLOPE_MV_PER_PH = NOISE_FLOOR_SENSOR_MV / MAX_NOISE_PH;

    // PROVISIONAL until 10-BENCH-RECORD §2 (BNC-short multimeter) / §6 (shorted-BNC CSV): the board's zero-input
    // level, sensor-side. D-25: measured on the bench rig only; another unit or board needs its own value.
    constexpr float BOARD_ZERO_INPUT_SENSOR_MV = 1740.0f;

    // PROVISIONAL until 10-BENCH-RECORD §13: how far the fitted pH-7 level may sit from the zero-input level.
    // Derivation: Mettler's 35 mV "worn out" electrode offset x the nominal board gain ~4.83 = ~169 mV, rounded up.
    constexpr float OFFSET_WINDOW_SENSOR_MV = 200.0f;

    // PROVISIONAL until 10-BENCH-RECORD §19 (D-19 bands): slope % of the board nominal (not Nernst) and |offset|
    // in pH. Good / aging / replace; the worse of the two decides.
    constexpr float HEALTH_SLOPE_GOOD_PCT = 95.0f;
    constexpr float HEALTH_SLOPE_AGING_PCT = 85.0f;
    constexpr float HEALTH_OFFSET_GOOD_PH = 0.30f;
    constexpr float HEALTH_OFFSET_AGING_PH = 0.50f;

    // Why a capture, pair or save was refused. The capture-level reasons are produced by plan 11-04's state
    // machine; they share this enum so the portal has one token list.
    enum class Refusal : uint8_t
    {
      None,
      NoTemperature,
      TemperatureOutOfRange,
      SignalLost,
      Unstable,
      SettleTimeout,
      BuffersTooClose,
      WrongSign,
      SlopeTooSmall,
      OffsetOutOfWindow,
      Invalid,
      Cancelled,
      Changed,
      NothingToSave,
      Nvs,
    };

    // Ordered best to worst: the verdict of a pair is the larger enumerator of its two parts.
    enum class Verdict : uint8_t
    {
      Good,
      Aging,
      Replace,
    };

    enum class Age : uint8_t
    {
      Fresh,
      Overdue,
      DateUnknown,
      NoClock,
    };

    enum class Slot : uint8_t
    {
      None,
      A,
      B,
    };

    // One captured buffer point, held in RAM only (D-14): the printed nominal, the sensor-side median mV, the
    // buffer's real pH at the cup temperature (hundredths) and that temperature.
    struct Point
    {
      bool held;
      uint16_t nominal;
      uint16_t mv;
      uint16_t bufHundredths;
      float tempC;
    };

    struct HeldPair
    {
      Point a;
      Point b;
    };

    // reasonValue carries the number a refusal is about (|slope| mV/pH, mV at pH 7), NAN when none applies.
    struct PairResult
    {
      Refusal reason;
      bool complete;
      ph::Calibration cal;
      ph::Line line;
      float slopePct;
      float offsetPh;
      Verdict verdict;
      float reasonValue;
    };

    struct AgeResult
    {
      Age age;
      uint32_t days;
    };

    inline bool isPickerNominal(uint16_t nominal)
    {
      for (size_t i = 0; i < PICKER_NOMINAL_COUNT; i++)
      {
        if (PICKER_NOMINALS[i] == nominal)
        {
          return true;
        }
      }
      return false;
    }

    // T-11-01: the page posts exact <select> values, so this is an exact-string whitelist with a length cap and no
    // numeric parser — no trimming, no signs, no leading zeros, no decimals. Anything else is 0 (refused).
    inline uint16_t parseNominal(const char *s)
    {
      if (s == nullptr || std::strlen(s) > 4)
      {
        return 0;
      }
      static const char *const TEXT[PICKER_NOMINAL_COUNT] = {"400", "401", "686", "700", "918", "1001"};
      for (size_t i = 0; i < PICKER_NOMINAL_COUNT; i++)
      {
        if (std::strcmp(s, TEXT[i]) == 0)
        {
          return PICKER_NOMINALS[i];
        }
      }
      return 0;
    }

    inline Slot parseSlot(const char *s)
    {
      if (s == nullptr)
      {
        return Slot::None;
      }
      if (std::strcmp(s, "a") == 0)
      {
        return Slot::A;
      }
      if (std::strcmp(s, "b") == 0)
      {
        return Slot::B;
      }
      return Slot::None;
    }

    // D-18 / D-19 / D-23: slope % of the board nominal (not Nernst) and |offset| in pH, each banded with the
    // accepting side inclusive; the worse band wins. A NAN in either has no honest verdict, so it is Replace.
    inline Verdict healthVerdict(float slopePct, float offsetPh)
    {
      if (std::isnan(slopePct) || std::isnan(offsetPh))
      {
        return Verdict::Replace;
      }
      const Verdict slope = slopePct >= HEALTH_SLOPE_GOOD_PCT    ? Verdict::Good
                            : slopePct >= HEALTH_SLOPE_AGING_PCT ? Verdict::Aging
                                                                 : Verdict::Replace;
      const float off = std::fabs(offsetPh);
      const Verdict offset = off <= HEALTH_OFFSET_GOOD_PH    ? Verdict::Good
                             : off <= HEALTH_OFFSET_AGING_PH ? Verdict::Aging
                                                             : Verdict::Replace;
      return static_cast<uint8_t>(slope) >= static_cast<uint8_t>(offset) ? slope : offset;
    }

    // D-20: 0 is "date unknown" (calibrated without a synced clock); an unsynced clock now cannot age anything; a
    // stamp in the future (clock stepped back) reads fresh with 0 days rather than a negative age.
    inline AgeResult calibrationAge(uint32_t calAt, long long nowEpoch)
    {
      if (calAt == 0)
      {
        return AgeResult{Age::DateUnknown, 0};
      }
      if (nowEpoch <= static_cast<long long>(MIN_VALID_EPOCH))
      {
        return AgeResult{Age::NoClock, 0};
      }
      const long long cal = static_cast<long long>(calAt);
      if (nowEpoch <= cal)
      {
        return AgeResult{Age::Fresh, 0};
      }
      const long long diff = nowEpoch - cal;
      const uint32_t days = static_cast<uint32_t>(diff / static_cast<long long>(SECONDS_PER_DAY));
      const long long limit = static_cast<long long>(OVERDUE_AFTER_DAYS) * static_cast<long long>(SECONDS_PER_DAY);
      return AgeResult{diff > limit ? Age::Overdue : Age::Fresh, days};
    }

    // The pair rules, first match wins (RESEARCH Pattern 4): gap on printed nominals, sign, useless slope, offset
    // window, then the storage-level check as a defensive last word, so nothing accepted here can be refused by
    // storePhCalibration. cal and line are filled whenever they were computed, for the page to show.
    inline PairResult evaluatePair(const Point &a, const Point &b)
    {
      PairResult r{};
      r.reason = Refusal::Invalid;
      r.complete = false;
      r.slopePct = NAN;
      r.offsetPh = NAN;
      r.verdict = Verdict::Replace;
      r.reasonValue = NAN;
      r.line = ph::Line{NAN, NAN, NAN};
      if (!a.held || !b.held)
      {
        return r;
      }

      const int32_t dNominal = static_cast<int32_t>(b.nominal) - static_cast<int32_t>(a.nominal);
      if (dNominal < static_cast<int32_t>(PORTAL_MIN_BUFFER_GAP_HUNDREDTHS) &&
          -dNominal < static_cast<int32_t>(PORTAL_MIN_BUFFER_GAP_HUNDREDTHS))
      {
        r.reason = Refusal::BuffersTooClose;
        return r;
      }

      // Same Tcal rule as the bench console's `ph save`: the mean of the two cup temperatures.
      const int16_t tcal = static_cast<int16_t>(std::lround((a.tempC + b.tempC) / 2.0f * 100.0f));
      r.cal = ph::Calibration{a.mv, a.bufHundredths, b.mv, b.bufHundredths, tcal, 0, ph::PH_CAL_VERSION};

      if (a.mv == b.mv)
      {
        r.reason = Refusal::SlopeTooSmall;
        r.reasonValue = 0.0f;
        return r;
      }
      const int32_t dMv = static_cast<int32_t>(b.mv) - static_cast<int32_t>(a.mv);
      const int32_t dBuf = static_cast<int32_t>(b.bufHundredths) - static_cast<int32_t>(a.bufHundredths);
      const int sign = ((dMv > 0) == (dBuf > 0)) ? 1 : -1;
      if (dBuf == 0 || sign != ph::EXPECTED_SLOPE_SIGN)
      {
        r.reason = dBuf == 0 ? Refusal::Invalid : Refusal::WrongSign;
        return r;
      }

      r.line = ph::fit(r.cal);
      const float absSlope = std::fabs(r.line.slopeMvPerPh);
      if (absSlope < MIN_SLOPE_MV_PER_PH)
      {
        r.reason = Refusal::SlopeTooSmall;
        r.reasonValue = absSlope;
        return r;
      }
      if (std::fabs(r.line.mvAtIsopotential - BOARD_ZERO_INPUT_SENSOR_MV) > OFFSET_WINDOW_SENSOR_MV)
      {
        r.reason = Refusal::OffsetOutOfWindow;
        r.reasonValue = r.line.mvAtIsopotential;
        return r;
      }
      if (ph::cal::checkCalibration(r.cal) != ph::cal::CalCheck::Ok)
      {
        r.reason = Refusal::Invalid;
        return r;
      }

      r.reason = Refusal::None;
      r.complete = true;
      r.slopePct = ph::cal::slopePercentOfNominal(r.line);
      r.offsetPh = ph::cal::offsetPh(r.line, BOARD_ZERO_INPUT_SENSOR_MV);
      r.verdict = healthVerdict(r.slopePct, r.offsetPh);
      return r;
    }

    inline void dropSlot(HeldPair &held, Slot slot)
    {
      const Point empty{};
      if (slot == Slot::A)
      {
        held.a = empty;
      }
      else if (slot == Slot::B)
      {
        held.b = empty;
      }
    }

    // D-14: points live in RAM only. When the pair is refused, the slot captured second is dropped and the first
    // is kept, so the user re-captures only the buffer that broke the pair.
    inline PairResult acceptIntoSlot(HeldPair &held, Slot slot, const Point &p)
    {
      PairResult r{};
      r.reason = Refusal::Invalid;
      r.complete = false;
      r.slopePct = NAN;
      r.offsetPh = NAN;
      r.verdict = Verdict::Replace;
      r.reasonValue = NAN;
      r.line = ph::Line{NAN, NAN, NAN};
      if (slot == Slot::None)
      {
        return r;
      }
      Point stored = p;
      stored.held = true;
      Point &mine = slot == Slot::A ? held.a : held.b;
      const Point &other = slot == Slot::A ? held.b : held.a;
      mine = stored;
      if (!other.held)
      {
        r.reason = Refusal::None;
        return r;
      }
      r = evaluatePair(held.a, held.b);
      if (r.reason != Refusal::None)
      {
        mine.held = false;
      }
      return r;
    }

    // D-24 / T-11-03: Save re-runs the pair rules and refuses when the nominals the page posts are not the ones
    // the held points were captured against (the picker was changed after capturing).
    inline PairResult checkSave(const HeldPair &held, uint16_t postedA, uint16_t postedB)
    {
      PairResult r{};
      r.complete = false;
      r.slopePct = NAN;
      r.offsetPh = NAN;
      r.verdict = Verdict::Replace;
      r.reasonValue = NAN;
      r.line = ph::Line{NAN, NAN, NAN};
      if (!held.a.held || !held.b.held)
      {
        r.reason = Refusal::NothingToSave;
        return r;
      }
      if (postedA != held.a.nominal || postedB != held.b.nominal)
      {
        r.reason = Refusal::Changed;
        return r;
      }
      return evaluatePair(held.a, held.b);
    }

    // Every enumerator listed and no default, so a new one is a -Wswitch warning; the fallback is a fault token.
    inline const char *refusalToken(Refusal r)
    {
      switch (r)
      {
      case Refusal::None:
        return "none";
      case Refusal::NoTemperature:
        return "no_temperature";
      case Refusal::TemperatureOutOfRange:
        return "temp_out_of_range";
      case Refusal::SignalLost:
        return "signal_lost";
      case Refusal::Unstable:
        return "unstable";
      case Refusal::SettleTimeout:
        return "settle_timeout";
      case Refusal::BuffersTooClose:
        return "buffers_too_close";
      case Refusal::WrongSign:
        return "wrong_sign";
      case Refusal::SlopeTooSmall:
        return "slope_too_small";
      case Refusal::OffsetOutOfWindow:
        return "offset_out_of_window";
      case Refusal::Invalid:
        return "invalid";
      case Refusal::Cancelled:
        return "cancelled";
      case Refusal::Changed:
        return "changed";
      case Refusal::NothingToSave:
        return "nothing_to_save";
      case Refusal::Nvs:
        return "nvs";
      }
      return "invalid";
    }

    inline const char *verdictToken(Verdict v)
    {
      switch (v)
      {
      case Verdict::Good:
        return "good";
      case Verdict::Aging:
        return "aging";
      case Verdict::Replace:
        return "replace";
      }
      return "replace";
    }

    inline const char *ageToken(Age a)
    {
      switch (a)
      {
      case Age::Fresh:
        return "fresh";
      case Age::Overdue:
        return "overdue";
      case Age::DateUnknown:
        return "date_unknown";
      case Age::NoClock:
        return "no_clock";
      }
      return "date_unknown";
    }

    inline const char *slotToken(Slot s)
    {
      switch (s)
      {
      case Slot::None:
        return "none";
      case Slot::A:
        return "a";
      case Slot::B:
        return "b";
      }
      return "none";
    }

    // ---- Capture engine (plan 11-04) ----------------------------------------------------------------------------
    // The portal sampler feeds one (sensor mV, cup temperature) pair a second; everything decided about a capture
    // lives here so Provisioning.cpp only feeds samples and reads state.

    // D-07: drift is judged as the median of the newest DRIFT_EDGE_SAMPLES minus the median of the oldest ones in
    // the rolling window. Five a side keeps one stray sample from deciding either edge while leaving a middle gap.
    constexpr size_t DRIFT_EDGE_SAMPLES = 5;
    static_assert(2 * DRIFT_EDGE_SAMPLES <= ph::cal::CAPTURE_WINDOW_SAMPLES,
                  "the two drift edges must not overlap inside the capture window");

    // PROVISIONAL until 10-BENCH-RECORD §7: the largest start-vs-end drift (sensor-side mV over the 20 s window) that
    // still counts as settled. About 3 x the sd of a median-of-5 difference at the §7 noise floor (~0.8 sigma per
    // median-of-5); noise-limited on purpose, not the Mettler 0.1 mV / 5 s textbook endpoint this board cannot see.
    constexpr float MAX_DRIFT_SENSOR_MV = 3.0f;

    // PROVISIONAL until 10-BENCH-RECORD §7: the spread backstop, max minus min of a 20-sample window. About 2 x the
    // max-min of the steady 20-row windows; catches a jumpy probe whose two edges happen to agree.
    constexpr float MAX_CAPTURE_SPREAD_SENSOR_MV = 25.0f;

    // PROVISIONAL until 10-BENCH-RECORD §7 (settle run): D-15 keeps trying for about 3 minutes of wall-clock time,
    // then refuses with the blocker that was last in the way.
    constexpr uint16_t SETTLE_TIMEOUT_S = 180;
    static_assert(SETTLE_TIMEOUT_S >= ph::cal::CAPTURE_WINDOW_SAMPLES,
                  "the timeout must leave time to fill one capture window at 1 Hz");

    // The page's live readout: whether a capture started now would pass (as turbidity's live state).
    enum class LiveState : uint8_t
    {
      Settling,
      Steady,
      SignalLost,
    };

    enum class CaptureState : uint8_t
    {
      Idle,
      Capturing,
      Settling,
      Accepted,
      Refused,
    };

    // Aggregate, value-initialise with `CaptureEngine e{};`. mvRing / tempRing are rings written at `next`; the
    // oldest sample sits at ring[next] once the window is full. elapsedS is the caller's wall-clock seconds since
    // startCapture. reasonValue is the number a refusal is about (degC, minutes), NAN when none applies.
    struct CaptureEngine
    {
      float mvRing[ph::cal::CAPTURE_WINDOW_SAMPLES];
      float tempRing[ph::cal::CAPTURE_WINDOW_SAMPLES];
      size_t filled;
      size_t next;
      uint16_t elapsedS;
      uint16_t nominal;
      CaptureState state;
      Refusal reason;
      float reasonValue;
      float lastDriftMv;
      Refusal lastNotReady;
      Point point;
    };

    inline float driftOf(const float *ring, size_t next, size_t filled)
    {
      (void)ring;
      (void)next;
      (void)filled;
      return 0.0f;
    }

    inline LiveState liveState(const float *ring, size_t next, size_t filled)
    {
      (void)ring;
      (void)next;
      (void)filled;
      return LiveState::Steady;
    }

    inline void startCapture(CaptureEngine &e, uint16_t nominal, float tempC)
    {
      (void)e;
      (void)nominal;
      (void)tempC;
    }

    inline void feedCapture(CaptureEngine &e, float sensorMv, float tempC, uint16_t elapsedS)
    {
      (void)e;
      (void)sensorMv;
      (void)tempC;
      (void)elapsedS;
    }

    inline void cancelCapture(CaptureEngine &e)
    {
      (void)e;
    }

    inline bool isCaptureRunning(const CaptureEngine &e)
    {
      (void)e;
      return false;
    }

    inline const char *liveStateToken(LiveState s)
    {
      (void)s;
      return "";
    }

    inline const char *captureStateToken(CaptureState s)
    {
      (void)s;
      return "";
    }
  }
}
