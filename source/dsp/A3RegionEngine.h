#pragma once

#include <atomic>
#include <cstdint>

#include "BandDetector.h"
#include "EdgeSmoother.h"
#include "GainComputer.h"
#include "GrSmoother.h"
#include "IBandEngine.h"
#include "TptSvf.h"

namespace tl::dsp
{
/** EXPERIMENTAL low-latency band engine ("A3"): one allpass-complementary 2nd-order region per
    used slot, cascaded in slot order, per channel (TPT SVF, delta form):
        y = x + (gEff - 1) 2R bp,   g = sqrt(wl wh),  R = (wh - wl) / (2g),  w = tan(pi f / fs)
    At gEff == 1 the output equals the input bit-exactly. Each band region has a bell response
    whose width depends on the gain (NOT accepted as final product behaviour).
    Feed-forward detector on the main input (pre-compression), self-range only.
    Zero latency; no allocation after construction (fixed internal chunk of kChunk samples). */
class A3RegionEngine final : public IBandEngine
{
public:
    A3RegionEngine() noexcept;

    void prepare (double sampleRate, int maximumBlockSize) override;
    void reset() noexcept override;
    void setSettings (const EngineSettings&) noexcept override;
    void process (float* const* channels, int numChannels, int numSamples) noexcept override;
    void processNeutral (float* const* channels, int numChannels, int numSamples) noexcept override;

    int getLatencySamples() const noexcept override { return 0; }
    const GrMeter& getMeter() const noexcept override { return meter; }
    std::uint32_t getStateResetCount() const noexcept override { return resetCount.load (std::memory_order_relaxed); }

    void setExternalGains (ExternalGains* g) noexcept override { externalGains = g; }
    void setGainTrace (GainTrace* t) noexcept override        { gainTrace = t; }

    /** Absolute sample counter (reset by prepare/reset); coefficient grid is counter % 16 == 0. */
    std::uint64_t getSampleCounter() const noexcept { return sampleCounter; }

private:
    struct SectionCoeffs { SvfCoeffs a3, hp, lp; };

    struct Band
    {
        bool used = false, active = false;
        bool engaged = false;   // used, not bypassed, no global bypass (activity target outside host bypass)
        EdgeSmoother edges;
        ActivityRamp activity;
        GainComputer computer;
        BandDetectorConfig detector;
        float link = 1.0f;
        GrSmoother smoother[kMaxChannels];
        BandDetectorState detState[kMaxChannels];
        SvfState region[kMaxChannels];
        SectionCoeffs coeffs;
        float coeffLo = -1.0f, coeffHi = -1.0f;   // clamped log2 edges the coefficients were made from
        float attackMs = -1.0f, releaseMs = -1.0f, rmsMs = -1.0f;
    };

    void applySettings() noexcept;
    void run (float* const* channels, int numChannels, int numSamples, bool neutral) noexcept;
    void processChunk (float* const* channels, int numChannels, int offset, int n, bool neutral) noexcept;
    void updateCoefficients (Band&, bool force) noexcept;
    static void resetBandState (Band&) noexcept;
    static bool bandStateIsFinite (const Band&) noexcept;

    double fs = 48000.0;
    std::uint64_t sampleCounter = 0;
    EngineSettings settings;
    Band bands[kMaxBands];

    // Fixed scratch (no allocation): per-chunk effective gains, activity and segment coefficients.
    float gains[kMaxBands][kMaxChannels][kChunk] {};
    float activity[kChunk] {};
    SectionCoeffs segments[kMaxBands][kMaxSegments] {};
    float blockMinGain[kMaxBands][kMaxChannels] {};

    GrMeter meter;
    std::atomic<std::uint32_t> resetCount { 0 };
    ExternalGains* externalGains = nullptr;
    GainTrace* gainTrace = nullptr;
};
} // namespace tl::dsp
