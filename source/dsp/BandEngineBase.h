#pragma once

#include <atomic>
#include <cstdint>

#include "DynamicsCore.h"
#include "IBandEngine.h"
#include "OutputStage.h"

namespace tl::dsp
{
/** Everything a band engine has in common: chunking, the input guard, the shared DynamicsCore,
    metering, measurement hooks and the band lifecycle. A concrete engine only supplies its filter
    structure through four hooks and applies the gains the core computed.
    No allocation after construction (fixed internal chunk of kChunk samples). */
class BandEngineBase : public IBandEngine
{
public:
    void prepare (double sampleRate, int maximumBlockSize) final;
    void reset() noexcept final;
    void setSettings (const EngineSettings&) noexcept final;
    void setSidechain (const float* const* channels, int numChannels) noexcept final;
    void process (float* const* channels, int numChannels, int numSamples) noexcept final;
    void processNeutral (float* const* channels, int numChannels, int numSamples) noexcept final;

    const GrMeter& getMeter() const noexcept final { return meter; }
    std::uint32_t getStateResetCount() const noexcept final { return resetCount.load (std::memory_order_relaxed); }
    float getAutoGainDb() const noexcept final { return autoGainDb.load (std::memory_order_relaxed); }

    void setExternalGains (ExternalGains* g) noexcept final { externalGains = g; }
    void setGainTrace (GainTrace* t) noexcept final        { gainTrace = t; }

    /** Absolute sample counter (reset by prepare/reset); coefficient grid is counter % 16 == 0. */
    std::uint64_t getSampleCounter() const noexcept { return sampleCounter; }

protected:
    BandEngineBase() noexcept = default;

    /** Message thread, from prepare(): the sample rate changed, drop cached coefficients. */
    virtual void prepareRegions (double sampleRate) noexcept = 0;
    /** Clears the engine's filter state of one band. */
    virtual void resetRegion (int band) noexcept = 0;
    virtual bool regionIsFinite (int band) const noexcept = 0;
    /** Applies the gains of the core's last chunk in place to channels[c][offset .. offset + n). */
    virtual void applyChunk (float* const* channels, int numChannels, int offset, int n, std::uint64_t pos) noexcept = 0;

    DynamicsCore core;

private:
    void applyGlobalTargets (bool neutral) noexcept;

    void run (float* const* channels, int numChannels, int numSamples, bool neutral) noexcept;
    void processChunk (float* const* channels, int numChannels, int offset, int n, bool neutral) noexcept;

    std::uint64_t sampleCounter = 0;
    EngineSettings settings;
    GainRamp inputGain, outputGain;
    LinearRamp globalMix { 1.0f };
    AutoGain autoGain;
    LinearRamp autoGainAmount;          // 0 = off, 1 = on (20 ms fade of the correction)
    bool autoGainRunning = false;
    std::atomic<float> autoGainDb { 0.0f };
    LinearRamp clipCeiling;             // dB
    ClipMode clipMode = ClipMode::Off;
    const float* sidechain[kMaxChannels] {};
    int numSidechain = 0;
    float sidechainScratch[kMaxChannels][kChunk] {};
    float dry[kMaxChannels][kChunk] {};
    float mixBuffer[kChunk] {};
    float blockMinGain[kMaxBands][kMaxChannels] {};
    GrMeter meter;
    std::atomic<std::uint32_t> resetCount { 0 };
    ExternalGains* externalGains = nullptr;
    GainTrace* gainTrace = nullptr;
};
} // namespace tl::dsp
