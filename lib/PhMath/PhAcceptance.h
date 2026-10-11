#pragma once

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
      (void)nominal;
      return false;
    }

    inline uint16_t parseNominal(const char *s)
    {
      (void)s;
      return 1;
    }

    inline Slot parseSlot(const char *s)
    {
      (void)s;
      return Slot::None;
    }

    inline Verdict healthVerdict(float slopePct, float offsetPh)
    {
      (void)slopePct;
      (void)offsetPh;
      return Verdict::Good;
    }

    inline AgeResult calibrationAge(uint32_t calAt, long long nowEpoch)
    {
      (void)calAt;
      (void)nowEpoch;
      return AgeResult{Age::Fresh, 0};
    }

    inline PairResult evaluatePair(const Point &a, const Point &b)
    {
      (void)a;
      (void)b;
      PairResult r{};
      r.reason = Refusal::Invalid;
      r.reasonValue = NAN;
      return r;
    }

    inline PairResult acceptIntoSlot(HeldPair &held, Slot slot, const Point &p)
    {
      (void)held;
      (void)slot;
      (void)p;
      PairResult r{};
      r.reason = Refusal::Invalid;
      r.reasonValue = NAN;
      return r;
    }

    inline void dropSlot(HeldPair &held, Slot slot)
    {
      (void)held;
      (void)slot;
    }

    inline PairResult checkSave(const HeldPair &held, uint16_t postedA, uint16_t postedB)
    {
      (void)held;
      (void)postedA;
      (void)postedB;
      PairResult r{};
      r.reason = Refusal::Invalid;
      r.reasonValue = NAN;
      return r;
    }

    inline const char *refusalToken(Refusal r)
    {
      (void)r;
      return "";
    }

    inline const char *verdictToken(Verdict v)
    {
      (void)v;
      return "";
    }

    inline const char *ageToken(Age a)
    {
      (void)a;
      return "";
    }

    inline const char *slotToken(Slot s)
    {
      (void)s;
      return "";
    }
  }
}
