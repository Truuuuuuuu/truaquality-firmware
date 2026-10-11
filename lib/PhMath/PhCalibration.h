#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "PhMath.h"

// The pH calibration helpers the bench console (10-06) and, later, Phase 11's portal share: the buffer temperature
// table, the basic D-18 checks with a printable reason, the slope % and offset printouts, and the capture window's
// median / spread. Nothing here touches hardware or NVS. Like PhMath.h it is Arduino-free (so test_ph_math pins it
// on the host), must not include Sensors.h, and is C++11 only because the board compiles it at gnu++11: no C++17
// clamp helper, no std::size, no structured bindings, no multi-statement constexpr functions.
namespace ph
{
  namespace cal
  {
    // Same 20 x 1 s capture as turbidity: the decision is taken on the median of the window, never on one burst.
    constexpr size_t CAPTURE_WINDOW_SAMPLES = 20;

    // D-11: the buffer table covers 20..35 C (the bench room and Sorsogon pond water are both inside it). Outside
    // it the capture is refused with a reason rather than extrapolated.
    constexpr float BUFFER_TABLE_MIN_C = 20.0f;
    constexpr float BUFFER_TABLE_MAX_C = 35.0f;

    // The real pH of each buffer, in hundredths, at 20 / 25 / 30 / 35 C. Sources (MEDIUM confidence, from search
    // snippets): NIST SRM 185h for the 0.05 m phthalate (4.001 / 4.005 / 4.012 / 4.021), the standard phosphate
    // (0.025 m: 6.878 / 6.863 / 6.851 / 6.842) and borax (0.01 m: 9.227 / 9.183 / 9.143 / 9.107) tables, with the
    // 6.86 and 9.18 rows at 30 C matching Cole-Parmer's NIST table; 25 C is anchored to the printed nominal. The 4.01
    // row is 401 at 30 C because NIST 4.012 rounds to 4.01 and the ETI technical 4.01 table also gives 4.01 there (an
    // earlier 402 came from Cole-Parmer's 4.02; corrected 2026-10-12, Phase 11 D-26). If the sachet prints its own
    // table, the table recorded in 10-BENCH-RECORD.md wins and replaces these rows (D-11). The 7.00 and 10.01 rows
    // come from the ETI 8000/8100 pH meter manual's buffer table (its 4.01 / 7.00 / 10.01 technical set,
    // https://www.tester.co.uk/files/get/download/id/3825/eti-8100-plus-ph-meter-user-manual.pdf), which agrees
    // with Hamilton DuraCal snippets: MEDIUM confidence. The 4.00 row is the NIST 0.05 m phthalate curve shape
    // shifted to 4.00 at 25 C: LOW confidence, assumed, off by at most 0.02 pH, and there is no 4.00 sachet on hand
    // to check it against. A sachet-printed table recorded in 10-BENCH-RECORD.md still wins (Phase 10 D-11). The
    // six rows are exactly the Phase 11 portal picker (11-CONTEXT D-10), in ascending nominal order.
    constexpr size_t BUFFER_TABLE_TEMPS = 4;
    constexpr float BUFFER_TABLE_C[BUFFER_TABLE_TEMPS] = {20.0f, 25.0f, 30.0f, 35.0f};

    struct BufferRow
    {
      uint16_t nominalHundredths;
      uint16_t phHundredths[BUFFER_TABLE_TEMPS];
    };

    constexpr size_t BUFFER_COUNT = 6;
    constexpr BufferRow BUFFER_TABLE[BUFFER_COUNT] = {
        {400, {400, 400, 401, 402}},
        {401, {400, 401, 401, 402}},
        {686, {688, 686, 685, 684}},
        {700, {702, 700, 699, 698}},
        {918, {923, 918, 914, 911}},
        {1001, {1006, 1001, 997, 993}},
    };

    // The pH the buffer really has at `tempC`, interpolated linearly between rows. NAN for a NAN temperature, a
    // temperature outside the table, or a nominal the table does not know — refused, never extrapolated, so a
    // capture can't silently calibrate against a guessed value.
    inline float bufferPhAt(uint16_t nominalHundredths, float tempC)
    {
      if (std::isnan(tempC) || tempC < BUFFER_TABLE_MIN_C || tempC > BUFFER_TABLE_MAX_C)
      {
        return NAN;
      }
      for (size_t b = 0; b < BUFFER_COUNT; b++)
      {
        const BufferRow &row = BUFFER_TABLE[b];
        if (row.nominalHundredths != nominalHundredths)
        {
          continue;
        }
        for (size_t i = 0; i + 1 < BUFFER_TABLE_TEMPS; i++)
        {
          const float t0 = BUFFER_TABLE_C[i];
          const float t1 = BUFFER_TABLE_C[i + 1];
          if (tempC >= t0 && tempC <= t1)
          {
            const float p0 = static_cast<float>(row.phHundredths[i]) / 100.0f;
            const float p1 = static_cast<float>(row.phHundredths[i + 1]) / 100.0f;
            return p0 + (p1 - p0) * ((tempC - t0) / (t1 - t0));
          }
        }
        return NAN;
      }
      return NAN;
    }

    // Why a calibration was refused, for the bench console to print. Only the D-18 basic checks; the portal's
    // CAL-09 refusal windows (slope % and offset limits) live in PhAcceptance.h (ph::accept).
    enum class CalCheck : uint8_t
    {
      Ok,
      WrongSign,
      BuffersTooClose,
      Invalid,
    };

    // The same rules as ph::isValidCalibration, split into a reason. Order: anything structurally broken first
    // (Invalid), then the gap (BuffersTooClose — the slope sign is meaningless for equal buffers), then the sign.
    inline CalCheck checkCalibration(const Calibration &c)
    {
      if (isValidCalibration(c))
      {
        return CalCheck::Ok;
      }
      if (c.version != PH_CAL_VERSION || c.mvA == 0 || c.mvB == 0 || c.mvA > MAX_SENSOR_MV || c.mvB > MAX_SENSOR_MV ||
          c.mvA == c.mvB || c.bufA == 0 || c.bufB == 0 || c.bufA >= 1400 || c.bufB >= 1400 || c.tcalCentiC < 0 ||
          c.tcalCentiC > MAX_TCAL_CENTI_C)
      {
        return CalCheck::Invalid;
      }
      const int32_t dBuf = static_cast<int32_t>(c.bufB) - static_cast<int32_t>(c.bufA);
      if (dBuf < MIN_BUFFER_GAP_HUNDREDTHS && -dBuf < MIN_BUFFER_GAP_HUNDREDTHS)
      {
        return CalCheck::BuffersTooClose;
      }
      const int32_t dMv = static_cast<int32_t>(c.mvB) - static_cast<int32_t>(c.mvA);
      const int slopeSign = ((dMv > 0) == (dBuf > 0)) ? 1 : -1;
      if (slopeSign != EXPECTED_SLOPE_SIGN)
      {
        return CalCheck::WrongSign;
      }
      return CalCheck::Invalid;
    }

    // ASSUMED: DFRobot's nominal 3.5 pH/V (= 285.7 mV/pH sensor-side) at 25 C. This is NOT a % of the Nernst slope
    // (59.16 mV/pH at the electrode) — the board's gain is not published for this clone, so the printout is a % of
    // the nominal board slope until a bench gain measurement replaces it.
    constexpr float NOMINAL_SENSOR_SLOPE_MV_PER_PH = 285.7f;
    constexpr float NOMINAL_SLOPE_REFERENCE_K = 298.15f;

    // Measured |slope| as a percentage of the nominal slope scaled to the calibration temperature (the slope
    // scales with absolute temperature, so a 35 C calibration is compared against a 35 C nominal).
    inline float slopePercentOfNominal(const Line &l)
    {
      const float nominalAtT = NOMINAL_SENSOR_SLOPE_MV_PER_PH * ((l.tcalC + KELVIN_OFFSET) / NOMINAL_SLOPE_REFERENCE_K);
      return 100.0f * std::fabs(l.slopeMvPerPh) / nominalAtT;
    }

    // The shorted-BNC (B1) offset in pH units: how far the fitted line's pH 7 sits from the board's zero-input level.
    inline float offsetPh(const Line &l, float mvShorted)
    {
      return (mvShorted - l.mvAtIsopotential) / l.slopeMvPerPh;
    }

    // Copy of turbidity::cal::medianOf: copies into a local buffer before sorting (the caller's ring must not be
    // reordered); a count above the buffer, an empty window or any NAN has no honest median.
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

    // Copy of turbidity::cal::spreadOf: max minus min, NAN for nothing to measure or a lost sample, so a gap never
    // reads as "steady".
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

    // Every enumerator listed and no default, so a new one is a -Wswitch warning; the fallback is a fault token.
    inline const char *calCheckToken(CalCheck check)
    {
      switch (check)
      {
      case CalCheck::Ok:
        return "ok";
      case CalCheck::WrongSign:
        return "wrong_sign";
      case CalCheck::BuffersTooClose:
        return "buffers_too_close";
      case CalCheck::Invalid:
        return "invalid";
      }
      return "invalid";
    }
  }
}
