#pragma once

#include "BandEngineBase.h"
#include "TptSvf.h"

namespace tl::dsp
{
/** EXPERIMENTAL band engine "A-prime": the A3 region with a gain-dependent damping, so that the
    band keeps its width at any depth (symmetric peaking form):
        settled response  H = (s'^2 + 2R sqrt(G) s' + 1) / (s'^2 + 2 (R / sqrt(G)) s' + 1)
    At the nominal edges the depth is half the command (in dB) whatever the command, cut and
    boost are mirror images, and at G == 1 the output equals the input bit-exactly.

    Structure (the variant that passed the numerical study; the plain form overshoots badly):
        R' = R / sqrt (Gw),   u = 2R' x,   y = x + (G - 1) bp_R' (u)
    - the scaling 2R' sits at the filter INPUT, so the in-band state does not depend on R';
    - Gw is a lagged copy of the gain (one pole in dB, time constant max (5 ms, 4 / (R w0))),
      updated with the coefficients on the 16-sample grid;
    - the output term uses the instantaneous gain, so the centre depth is never lagged.
    Consequences: the constant-width shape is reached only after the lag (5 ms for an octave at
    1 kHz, hundreds of ms for narrow low bands); during a fast gain change the band has A3's
    shape. Skirts of adjacent bands add. Both lanes of a band share one width (the deeper lane).
    Zero latency; same cost as A3 plus one exp and one divide per band every 16 samples. */
class APrimeRegionEngine final : public BandEngineBase
{
public:
    APrimeRegionEngine() noexcept = default;

    int getLatencySamples() const noexcept override { return 0; }

    static constexpr double lagFactor = 4.0;          // lag = max (minLagSeconds, lagFactor / (R w0))
    static constexpr double minLagSeconds = 0.005;

private:
    void prepareRegions (double sampleRate) noexcept override;
    void resetRegion (int band) noexcept override;
    bool regionIsFinite (int band) const noexcept override;
    void applyChunk (float* const* channels, int numChannels, int offset, int n, std::uint64_t pos) noexcept override;
    bool copyRegionsFrom (const BandEngineBase& other) noexcept override;
    void primeRegions() noexcept override;
    const void* engineTypeTag() const noexcept override { return &typeTag; }
    static inline const char typeTag = 0;

    struct Region
    {
        SvfState state[kMaxChannels];
        float lo = -1.0f, hi = -1.0f;   // log2 edges g / R were made from
        double g = 0.1, R = 1.0;        // prototype at unity gain
        double lagB = 1.0;              // 1 - a of the width lag per grid step
        double widthDb = 0.0;           // lagged gain (dB) that sets the damping
        bool snapWidth = false;         // the next grid update takes the gain as it is (no lag)
        SvfCoeffs coeffs;               // for the current R'
    };

    double fs = 48000.0;
    Region regions[kMaxBands];
    SvfCoeffs segmentCoeffs[kMaxSegments] {};
};
} // namespace tl::dsp
