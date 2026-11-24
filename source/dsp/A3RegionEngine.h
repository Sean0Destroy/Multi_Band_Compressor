#pragma once

#include "BandEngineBase.h"
#include "TptSvf.h"

namespace tl::dsp
{
/** EXPERIMENTAL low-latency band engine ("A3"): one allpass-complementary 2nd-order region per
    used slot, cascaded in slot order, per channel (TPT SVF, delta form):
        y = x + (gEff - 1) 2R bp,   g = sqrt(wl wh),  R = (wh - wl) / (2g),  w = tan(pi f / fs)
    At gEff == 1 the output equals the input bit-exactly. Each band region has a bell response
    whose width depends on the gain (NOT accepted as final product behaviour).
    The gains come from the shared DynamicsCore (feed-forward detector on the main input).
    Zero latency; no allocation after construction. */
class A3RegionEngine final : public BandEngineBase
{
public:
    A3RegionEngine() noexcept = default;

    int getLatencySamples() const noexcept override { return 0; }

private:
    void prepareRegions (double sampleRate) noexcept override;
    void resetRegion (int band) noexcept override;
    bool regionIsFinite (int band) const noexcept override;
    void applyChunk (float* const* channels, int numChannels, int offset, int n, std::uint64_t pos) noexcept override;
    bool copyRegionsFrom (const BandEngineBase& other) noexcept override;
    bool exportRegionState (int band, float state[kMaxChannels][2]) const noexcept override;
    void importRegionState (int band, const float state[kMaxChannels][2]) noexcept override;
    const void* engineTypeTag() const noexcept override { return &typeTag; }
    EngineKind modelKind() const noexcept override { return EngineKind::A3; }
    static inline const char typeTag = 0;

    struct Region
    {
        SvfState state[kMaxChannels];
        SvfCoeffs coeffs;
        float lo = -1.0f, hi = -1.0f;   // log2 edges `coeffs` was made from
    };

    double fs = 48000.0;
    Region regions[kMaxBands];
    SvfCoeffs segmentCoeffs[kMaxSegments] {};
};
} // namespace tl::dsp
