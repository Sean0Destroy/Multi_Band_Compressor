#pragma once

#include <algorithm>

#include "DspConstants.h"

namespace tl::dsp
{
/** Static feed-forward gain computer, soft knee after Giannoulis, Massberg and Reiss (JAES 2012),
    followed by the per-band Range limit. All levels in dB; the result is gain reduction <= 0.

        2(x-T) <  -W : y = x
       |2(x-T)| <= W : y = x + (1/R - 1)(x - T + W/2)^2 / (2W)
        2(x-T) >   W : y = T + (x - T)/R
    gr = max(y - x, -range). Below the knee gr is exactly 0. W == 0 is a hard knee. */
struct GainComputer
{
    float thresholdDb = -18.0f;
    float invRatio = 0.5f;        // 1/R, 0 for R >= 100 ("infinity")
    float kneeDb = 6.0f;
    float rangeDb = 24.0f;

    void set (float threshDb, float ratio, float knee, float range) noexcept
    {
        thresholdDb = threshDb;
        invRatio = ratio >= kInfiniteRatio ? 0.0f : 1.0f / std::max (1.0f, ratio);
        kneeDb = std::max (0.0f, knee);
        rangeDb = std::max (0.0f, range);
    }

    /** Level (dB) below which gr is exactly zero: T - W/2. */
    float kneeStartDb() const noexcept { return thresholdDb - 0.5f * kneeDb; }

    /** Static curve without the range limit (y - x). */
    float staticGr (float x) const noexcept
    {
        const float over2 = 2.0f * (x - thresholdDb);

        if (over2 < -kneeDb)
            return 0.0f;

        if (kneeDb > 0.0f && over2 <= kneeDb)
        {
            const float u = x - thresholdDb + 0.5f * kneeDb;
            return (invRatio - 1.0f) * u * u / (2.0f * kneeDb);
        }

        return (invRatio - 1.0f) * (x - thresholdDb);   // T + (x-T)/R - x
    }

    float gainReductionDb (float x) const noexcept
    {
        return std::max (staticGr (x), -rangeDb);
    }
};
} // namespace tl::dsp
