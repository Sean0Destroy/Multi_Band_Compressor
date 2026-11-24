#pragma once

#include <algorithm>
#include <cmath>

#include "DspConstants.h"
#include "KWeighting.h"

// The last stages of the signal path: automatic gain (slow loudness match of the processed
// signal to the dry signal) and the output clipper.
namespace tl::dsp
{
// ---- Clipper ----------------------------------------------------------------------------------
enum class ClipMode { Off = 0, Soft = 1, Hard = 2 };

inline constexpr float kSoftClipKnee = 0.5f;   // linear below knee * ceiling

/** Hard clip at +-ceiling (linear). Samples inside the ceiling are returned unchanged. */
inline float hardClip (float x, float ceiling) noexcept
{
    return std::clamp (x, -ceiling, ceiling);
}

/** Soft clip: exactly linear up to half the ceiling, then a tanh section that approaches the
    ceiling and never exceeds it. Value and slope are continuous at the knee:
        |x| <= k c :  y = x
        |x| >  k c :  y = sgn(x) c (k + (1 - k) tanh ((|x|/c - k) / (1 - k)))            */
inline float softClip (float x, float ceiling) noexcept
{
    const float a = std::abs (x);
    const float knee = kSoftClipKnee * ceiling;
    if (a <= knee)
        return x;

    const float span = (1.0f - kSoftClipKnee) * ceiling;
    const float y = knee + span * std::tanh ((a - knee) / span);
    return std::copysign (std::min (y, ceiling), x);
}

// ---- Automatic gain -----------------------------------------------------------------------------
inline constexpr double kAutoGainIntegrationSeconds = 1.0;   // loudness integration (one-pole)
inline constexpr double kAutoGainSmoothingSeconds = 0.25;    // smoothing of the applied gain
inline constexpr double kAutoGainLimitDb = 12.0;             // the correction never exceeds +-12 dB
inline constexpr double kAutoGainGateMeanSquare = 1.0e-7;    // -70 dB K-weighted: below this the gain is held

/** Follows the K-weighted loudness of a reference (the dry signal) and of the processed signal
    and returns the gain that makes the processed signal as loud as the reference. Slow by
    design (1 s integration): it restores the average level, it does not undo the compression.
    While the reference is below the gate the correction is held, so silence cannot drive it up.
    Per-sample, state in double; the target is recomputed on the absolute coefficient grid so
    that the result does not depend on the host's block layout. */
class AutoGain
{
public:
    void prepare (double sampleRate) noexcept
    {
        shelf = kWeightingShelf (sampleRate);
        highPass = kWeightingHighPass (sampleRate);
        integrate = 1.0 - onePoleCoefficient (kAutoGainIntegrationSeconds, sampleRate);
        smooth = 1.0 - onePoleCoefficient (kAutoGainSmoothingSeconds, sampleRate);
        reset();
    }

    void reset() noexcept
    {
        for (auto& f : refFilter) f.reset();
        for (auto& f : wetFilter) f.reset();
        refMs = wetMs = 0.0;
        target = gain = 1.0;
    }

    /** One sample frame (1 or 2 channels). `onGrid`: recompute the target now. Returns the
        linear gain to apply to this frame. */
    float tick (const float* reference, const float* processed, int numChannels, bool onGrid) noexcept
    {
        double r = 0.0, w = 0.0;
        for (int c = 0; c < numChannels; ++c)
        {
            const double kr = refFilter[c].tick (shelf, highPass, (double) reference[c]);
            const double kw = wetFilter[c].tick (shelf, highPass, (double) processed[c]);
            r += kr * kr;
            w += kw * kw;
        }
        refMs += integrate * (r - refMs);
        wetMs += integrate * (w - wetMs);

        if (windowOpen)
        {
            windowEnergy += w;
            ++windowSamples;
        }

        if (onGrid && refMs > kAutoGainGateMeanSquare && wetMs > kAutoGainGateMeanSquare * 1.0e-3)
        {
            const double limit = std::pow (10.0, kAutoGainLimitDb / 20.0);
            target = std::clamp (std::sqrt (refMs / wetMs), 1.0 / limit, limit);
        }

        gain += smooth * (target - gain);
        return (float) gain;
    }

    double gainDb() const noexcept { return 20.0 * std::log10 (std::max (1.0e-6, gain)); }

    // ---- Changeover between two engines that run on the same input -----------------------------
    /** From now on the K-weighted energy of the processed signal is also summed plainly. Call it
        on the running follower before the incoming one is copied from it, so that both sum the
        same stretch of signal. */
    void beginWindow() noexcept { windowEnergy = 0.0; windowSamples = 0; windowOpen = true; }
    void endWindow() noexcept   { windowOpen = false; }

    /** `running` belongs to the engine that is heard, this one to an engine that has processed
        the same input since beginWindow() and is about to be faded in. This follower was copied
        from `running`, so its correction is still the one for the other engine's output; where
        the two engines differ in level that would be heard as a level error that takes seconds
        to go away. The two sums give the loudness ratio of the two outputs over the window; the
        long-term estimate and the correction are set to what they would be had this follower
        always measured its own engine. False (nothing changed) if there is nothing to compare. */
    bool matchTo (const AutoGain& running) noexcept
    {
        if (! windowOpen || ! running.windowOpen || windowSamples <= 0 || windowSamples != running.windowSamples)
            return false;

        const double mine = windowEnergy / (double) windowSamples;
        const double theirs = running.windowEnergy / (double) running.windowSamples;
        const double floor = kAutoGainGateMeanSquare * 1.0e-3;
        if (! (mine > floor) || ! (theirs > floor))
            return false;                                   // too quiet to compare

        const double ratio = mine / theirs;                // this engine's output energy relative to the running one's
        const double limit = std::pow (10.0, kAutoGainLimitDb / 20.0);
        const double scale = 1.0 / std::sqrt (ratio);
        refMs = running.refMs;
        wetMs = running.wetMs * ratio;
        target = std::clamp (running.target * scale, 1.0 / limit, limit);
        gain = std::clamp (running.gain * scale, 1.0 / limit, limit);
        return std::isfinite (gain) && std::isfinite (wetMs);
    }

    bool isFinite() const noexcept
    {
        bool ok = std::isfinite (refMs) && std::isfinite (wetMs) && std::isfinite (gain);
        for (auto& f : refFilter) ok = ok && f.isFinite();
        for (auto& f : wetFilter) ok = ok && f.isFinite();
        return ok;
    }

private:
    BiquadCoeffs shelf, highPass;
    KWeightingFilter refFilter[kMaxChannels], wetFilter[kMaxChannels];
    double integrate = 1.0, smooth = 1.0;
    double refMs = 0.0, wetMs = 0.0, target = 1.0, gain = 1.0;
    double windowEnergy = 0.0;
    long long windowSamples = 0;
    bool windowOpen = false;
};
} // namespace tl::dsp
