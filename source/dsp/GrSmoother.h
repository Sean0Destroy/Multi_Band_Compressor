#pragma once

#include <algorithm>
#include <cmath>

#include "DspConstants.h"

namespace tl::dsp
{
/** Decoupled peak smoother in the gain-reduction domain. Works on c = -target (dB of reduction, >= 0):
        s = aA s + (1-aA) c   if c > s   (attack)
        s = aR s + (1-aR) c   otherwise  (release)
    a = exp(-1 / (tau fs)). The linear band gain is 10^(-s/20).
    Evaluated in the algebraically identical form s += (1-a)(c - s) with a double state: in
    float, (1-a) would be quantised (long time constants off by up to a few percent at 192 kHz)
    and the state would stall once the step is below half an ulp, e.g. 0.06 dB short of the
    target for a 5 s release at 48 kHz, 0.1 dB for a 300 ms attack to 48 dB GR at 192 kHz. */
class GrSmoother
{
public:
    void setTimes (double attackMs, double releaseMs, double sampleRate) noexcept
    {
        const double aA = onePoleCoefficient (attackMs * 1.0e-3, sampleRate);
        const double aR = onePoleCoefficient (releaseMs * 1.0e-3, sampleRate);
        attackB = 1.0 - aA;
        releaseB = 1.0 - aR;
    }

    /** target: gain reduction in dB (<= 0). Returns the smoothed reduction amount s (>= 0). */
    float tick (float targetDb) noexcept
    {
        const double c = -(double) targetDb;
        s += (c > s ? attackB : releaseB) * (c - s);
        return (float) s;
    }

    float amountDb() const noexcept { return (float) s; }
    void reset() noexcept { s = 0.0; }
    bool isFinite() const noexcept { return std::isfinite (s); }

    /** 10^(-s/20); exactly 1 when s is exactly 0. */
    static float gainFromAmount (float amountDb) noexcept
    {
        constexpr float k = -0.11512925464970228420f;   // -ln(10)/20
        return isExactlyZero (amountDb) ? 1.0f : std::exp (k * amountDb);
    }

private:
    double attackB = 1.0, releaseB = 1.0;   // 1 - a
    double s = 0.0;
};

/** Linear 10 ms activity ramp a in [0, 1]; gEff = 1 + a (gComp - 1). */
class ActivityRamp
{
public:
    void prepare (double sampleRate) noexcept { step = (float) (1.0 / (kActivityRampSeconds * sampleRate)); }
    void setTarget (bool on) noexcept         { target = on ? 1.0f : 0.0f; }
    void setValue (float v) noexcept          { a = v; }
    float value() const noexcept              { return a; }
    bool isSilentTarget() const noexcept      { return target < 0.5f; }
    bool isSettledOff() const noexcept        { return target < 0.5f && isExactlyZero (a); }

    float tick() noexcept
    {
        if (a < target)      a = std::min (target, a + step);
        else if (a > target) a = std::max (target, a - step);
        return a;
    }

private:
    float a = 0.0f, target = 0.0f, step = 1.0f / 480.0f;
};
} // namespace tl::dsp
