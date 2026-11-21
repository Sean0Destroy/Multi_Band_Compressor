#pragma once

#include <algorithm>
#include <cmath>

#include "DspConstants.h"
#include "GainComputer.h"
#include "TptSvf.h"

namespace tl::dsp
{
enum class DetectorMode { Peak = 0, Rms = 1, Blend = 2 };

/** Per band, per channel level detector on the band's own range (self-range only):
    2nd-order Butterworth HP at fLo, then 2nd-order Butterworth LP at fHi (TPT SVF), then
      peak:  20 log10(max(|d|, 1e-9))
      RMS:   m = b m + (1-b) d^2,  10 log10(max(m, 1e-18))   (evaluated as m += (1-b)(d^2 - m),
             double state: a float mean square stalls short of its target for long times)
      blend: (1-k) peak + k RMS                                                         */
struct BandDetectorState
{
    SvfState hp, lp;
    double meanSquare = 0.0;

    void reset() noexcept { hp.reset(); lp.reset(); meanSquare = 0.0; }
    bool isFinite() const noexcept { return hp.isFinite() && lp.isFinite() && std::isfinite (meanSquare); }
};

/** Per-band detector configuration (shared by the channels of the band). */
struct BandDetectorConfig
{
    DetectorMode mode = DetectorMode::Peak;
    float blend = 0.5f;          // k
    double rmsB = 1.0;           // 1 - b
    float kneeStartLin = 0.0f;   // 10^((T - W/2)/20): |d| below this is below the knee
    double kneeStartPow = 0.0;   // kneeStartLin^2 for the mean square

    void setRmsTime (double rmsMs, double sampleRate) noexcept
    {
        const double b = onePoleCoefficient (rmsMs * 1.0e-3, sampleRate);
        rmsB = 1.0 - b;
    }

    void setKneeStart (float kneeStartDb) noexcept
    {
        const double lin = std::pow (10.0, (double) kneeStartDb / 20.0);
        kneeStartLin = (float) lin;
        kneeStartPow = lin * lin;
    }
};

/** Filters one sample and returns the band-limited detector signal d. */
inline float detectorFilter (BandDetectorState& st, const SvfCoeffs& hpC, const SvfCoeffs& lpC, float x) noexcept
{
    const float hp = svfTick (hpC, st.hp, x).hp;
    return svfTick (lpC, st.lp, hp).lp;
}

/** Updates the mean square and returns the static gain reduction (dB, <= 0, range applied).
    Below the knee the result is exactly 0; the log is skipped there (linear-domain compare). */
inline float detectorGainReduction (BandDetectorState& st, const BandDetectorConfig& cfg,
                                    const GainComputer& gc, float d) noexcept
{
    const float ad = std::abs (d);
    st.meanSquare += cfg.rmsB * ((double) d * d - st.meanSquare);

    float level = 0.0f;
    switch (cfg.mode)
    {
        case DetectorMode::Peak:
            if (ad < cfg.kneeStartLin) return 0.0f;
            level = 20.0f * std::log10 (std::max (ad, 1.0e-9f));
            break;

        case DetectorMode::Rms:
            if (st.meanSquare < cfg.kneeStartPow) return 0.0f;
            level = 10.0f * std::log10 ((float) std::max (st.meanSquare, 1.0e-18));
            break;

        case DetectorMode::Blend:
        default:
        {
            if (ad < cfg.kneeStartLin && st.meanSquare < cfg.kneeStartPow) return 0.0f;   // convex combination stays below
            const float peakDb = 20.0f * std::log10 (std::max (ad, 1.0e-9f));
            const float rmsDb = 10.0f * std::log10 ((float) std::max (st.meanSquare, 1.0e-18));
            level = (1.0f - cfg.blend) * peakDb + cfg.blend * rmsDb;
            break;
        }
    }

    return gc.gainReductionDb (level);
}
} // namespace tl::dsp
