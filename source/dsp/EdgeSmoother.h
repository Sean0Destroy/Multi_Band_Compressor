#pragma once

#include <algorithm>
#include <cmath>

#include "DspConstants.h"

namespace tl::dsp
{
/** Smooths a band's lower/upper edge in the log2 (octave) domain with a per-sample one-pole
    (tau 20 ms). Parameter ordering is resolved here (targets are sorted); the runtime clamp
    (fLo >= 10 Hz, fHi <= 0.45 fs, fHi >= fLo * 2^(1/24)) is applied by clampedEdgesHz() and
    is never written back to parameters.
    The state is double: a float one-pole with a = exp(-1/(tau fs)) stalls once the per-sample
    step is below half an ulp, i.e. up to 0.5/(1-a) ulps away from the target (480 ulps = 4.6e-4
    octave at 48 kHz), which visibly detunes deep bells. Within 1e-9 octave it snaps to the
    target, so a settled band has exactly the coefficients of a band that started there. */
class EdgeSmoother
{
public:
    void prepare (double sampleRate) noexcept
    {
        fs = sampleRate;
        b = 1.0 - onePoleCoefficient (kEdgeTauSeconds, sampleRate);
        maxLog2 = (float) std::log2 (kMaxEdgeFraction * sampleRate);
    }

    void setTargetsHz (float fLoHz, float fHiHz) noexcept
    {
        // Non-positive / non-finite values cannot come from the schema; keep the DSP safe anyway.
        const float lo = std::isfinite (fLoHz) && fLoHz > 0.0f ? std::log2 (fLoHz) : (float) std::log2 (kMinEdgeHz);
        const float hi = std::isfinite (fHiHz) && fHiHz > 0.0f ? std::log2 (fHiHz) : (float) std::log2 (kMinEdgeHz);
        tLo = (double) std::min (lo, hi);
        tHi = (double) std::max (lo, hi);
    }

    void snapToTargets() noexcept { sLo = tLo; sHi = tHi; }

    void tick() noexcept
    {
        sLo = step (sLo, tLo);
        sHi = step (sHi, tHi);
    }

    float smoothedLoLog2() const noexcept { return (float) sLo; }
    float smoothedHiLog2() const noexcept { return (float) sHi; }

    /** Applies the runtime clamp to the current smoothed edges (log2 Hz). */
    void clampedLog2 (float& lo, float& hi) const noexcept
    {
        constexpr float minRatio = (float) kMinEdgeRatioOct;
        const float minLog2 = (float) 3.32192809488736234787;   // log2 (10 Hz)
        lo = std::clamp ((float) sLo, minLog2, maxLog2 - minRatio);
        hi = std::clamp ((float) sHi, lo + minRatio, maxLog2);
    }

    double sampleRate() const noexcept { return fs; }

private:
    double step (double s, double t) const noexcept
    {
        const double next = s + b * (t - s);   // == t + a (s - t)
        return std::abs (t - next) < 1.0e-9 ? t : next;
    }

    double fs = 48000.0, b = 1.0;
    float maxLog2 = 14.4f;
    double tLo = 6.0, tHi = 10.0, sLo = 6.0, sHi = 10.0;
};
} // namespace tl::dsp
