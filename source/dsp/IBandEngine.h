#pragma once

#include <array>
#include <cstdint>

#include "BandDetector.h"
#include "DspConstants.h"
#include "GrMeter.h"

// Replaceable band-processing engine interface. The processor only builds EngineSettings from
// parameters/topology once per block and hands the main-bus channels to the engine.
namespace tl::dsp
{
struct BandSettings
{
    float fLoHz = 100.0f, fHiHz = 1000.0f;
    float thresholdDb = -18.0f, ratio = 2.0f, kneeDb = 6.0f;
    float attackMs = 10.0f, releaseMs = 150.0f, rangeDb = 24.0f;
    DetectorMode detectorMode = DetectorMode::Peak;
    float detectorBlend = 0.5f;   // 0 = peak, 1 = RMS (Blend mode only)
    float rmsTimeMs = 30.0f;
    float linkPercent = 100.0f;   // stereo link, 0..100
    bool bypass = false;
};

struct EngineSettings
{
    std::array<BandSettings, kMaxBands> bands {};
    std::uint32_t usedMask = 0;   // bit (slot - 1) set for used slots (BandSlots::usedMaskSnapshot)
    bool bypassAll = false;       // global bypass: every band ramps to neutral
};

/** Measurement hook: externally supplied per-band linear gain trajectories that REPLACE the
    band's effective gain gEff (detector, smoother and activity ramp are bypassed for that band).
    data[b] == nullptr keeps the normal path. Sample index = position + n; beyond `length` the
    last value is held (length 1 = constant). position advances by every processed sample. */
struct ExternalGains
{
    const float* data[kMaxBands] {};
    long long length = 1;
    long long position = 0;
};

/** Measurement hook: receives the effective band gain gEff per sample (data[band][channel]).
    Writes stop at `capacity`; position advances by every processed sample. */
struct GainTrace
{
    float* data[kMaxBands][kMaxChannels] {};
    long long capacity = 0;
    long long position = 0;
};

class IBandEngine
{
public:
    virtual ~IBandEngine() = default;

    /** Message thread. Resets all state and the absolute sample counter. */
    virtual void prepare (double sampleRate, int maximumBlockSize) = 0;
    virtual void reset() noexcept = 0;

    /** Audio thread, before process(). Applied at the start of the next processed block. */
    virtual void setSettings (const EngineSettings&) noexcept = 0;

    /** Audio thread. In-place processing of up to kMaxChannels channels; any block length. */
    virtual void process (float* const* channels, int numChannels, int numSamples) noexcept = 0;

    /** Audio thread (host bypass). Every band ramps to neutral over the 10 ms activity ramp (no
        click when entered while compressing); after the ramp the output equals the input exactly.
        States keep running, and a later process() ramps the bands back in. */
    virtual void processNeutral (float* const* channels, int numChannels, int numSamples) noexcept = 0;

    virtual int getLatencySamples() const noexcept = 0;
    virtual const GrMeter& getMeter() const noexcept = 0;

    /** Number of band resets caused by non-finite internal state since prepare(). */
    virtual std::uint32_t getStateResetCount() const noexcept = 0;

    /** Measurement hooks (tests / tools only, nullptr in the plugin). Must not be changed while
        process() may run concurrently. */
    virtual void setExternalGains (ExternalGains*) noexcept = 0;
    virtual void setGainTrace (GainTrace*) noexcept = 0;
};
} // namespace tl::dsp
