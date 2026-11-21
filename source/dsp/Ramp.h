#pragma once

#include <algorithm>
#include <cmath>

#include "DspConstants.h"

namespace tl::dsp
{
inline constexpr double kGainRampSeconds = 0.020;   // makeup, mix, input and output gain

/** Linear ramp that reaches a new target in a fixed number of samples and then holds it
    EXACTLY (the last step assigns the target), so a settled value is bit-identical to one that
    started there. Used in dB for gains (a dB-linear ramp is an exponential gain ramp). */
class LinearRamp
{
public:
    LinearRamp() noexcept = default;
    explicit LinearRamp (float initial) noexcept : current (initial), target (initial) {}

    void prepare (double sampleRate, double seconds = kGainRampSeconds) noexcept
    {
        length = std::max (1, (int) std::lround (seconds * sampleRate));
        snap();
    }

    void setTarget (float t) noexcept
    {
        if (! isFiniteFloat (t) || sameBits (t, target))
            return;

        target = t;
        remaining = length;
        step = (target - current) / (float) length;
    }

    /** Jumps to the target (start of playback, band start). */
    void snap() noexcept
    {
        current = target;
        remaining = 0;
    }

    float tick() noexcept
    {
        if (remaining > 0)
        {
            --remaining;
            current = remaining == 0 ? target : current + step;
        }
        return current;
    }

    bool isRamping() const noexcept { return remaining > 0; }
    float value() const noexcept    { return current; }
    float targetValue() const noexcept { return target; }

private:
    float current = 0.0f, target = 0.0f, step = 0.0f;
    int remaining = 0, length = 960;
};

/** 10^(dB/20); exactly 1 for exactly 0 dB. */
inline float dbToLinear (float db) noexcept
{
    constexpr float k = 0.11512925464970228420f;   // ln(10)/20
    return isExactlyZero (db) ? 1.0f : std::exp (k * db);
}

/** A gain in dB with a click-free ramp; the linear value is cached while settled. */
class GainRamp
{
public:
    void prepare (double sampleRate) noexcept { ramp.prepare (sampleRate); linear = dbToLinear (ramp.value()); }
    void setTargetDb (float db) noexcept      { ramp.setTarget (db); }
    void snap() noexcept                      { ramp.snap(); linear = dbToLinear (ramp.value()); }

    float tick() noexcept
    {
        if (ramp.isRamping())
            linear = dbToLinear (ramp.tick());
        return linear;
    }

    /** True if the gain is exactly unity and not moving (the stage can be skipped). */
    bool isUnity() const noexcept { return ! ramp.isRamping() && isExactlyOne (linear); }
    bool isRamping() const noexcept { return ramp.isRamping(); }
    float valueDb() const noexcept { return ramp.value(); }

private:
    LinearRamp ramp;
    float linear = 1.0f;
};
} // namespace tl::dsp
