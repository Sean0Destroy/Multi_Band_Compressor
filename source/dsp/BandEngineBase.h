#pragma once

#include <atomic>
#include <cstdint>

#include "A3Model.h"
#include "Character.h"
#include "DynamicsCore.h"
#include "IBandEngine.h"
#include "Lookahead.h"
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

    void setAudition (AuditionMode mode, int slot) noexcept final
    {
        const int s = mode == AuditionMode::None ? 0 : std::clamp (slot, 1, kMaxBands);
        auditionRequest.store ((int) mode | (s << 4), std::memory_order_relaxed);
    }
    AuditionMode getAuditionMode() const noexcept final { return (AuditionMode) (auditionRequest.load (std::memory_order_relaxed) & 0xf); }
    int getAuditionSlot() const noexcept final          { return auditionRequest.load (std::memory_order_relaxed) >> 4; }

    bool continueFrom (const IBandEngine& running) noexcept final;
    double getFilterSettlingSeconds() const noexcept final;
    void beginChangeoverMeasurement() noexcept final { autoGain.beginWindow(); }
    void matchOutputTo (const IBandEngine& running) noexcept final;
    void endChangeoverMeasurement() noexcept final   { autoGain.endWindow(); }
    const BandEngineBase* asBandEngineBase() const noexcept final { return this; }

    void setExternalGains (ExternalGains* g) noexcept final { externalGains = g; }
    void setGainTrace (GainTrace* t) noexcept final        { gainTrace = t; }

    /** Lookahead delay currently applied to the audio, in samples (audio thread value). */
    int getLookaheadDelay() const noexcept { return lookCurrent.audio; }

    /** Absolute sample counter (reset by prepare/reset); coefficient grid is counter % 16 == 0. */
    std::uint64_t getSampleCounter() const noexcept { return sampleCounter; }

protected:
    BandEngineBase() noexcept = default;

    /** Message thread, from prepare(): the sample rate changed, drop cached coefficients. */
    virtual void prepareRegions (double sampleRate) noexcept = 0;
    /** Clears the engine's filter state of one band. */
    virtual void resetRegion (int band) noexcept = 0;
    virtual bool regionIsFinite (int band) const noexcept = 0;
    /** continueFrom(): copies the band filter state of `other` if it is the same kind of engine
        (then the continuation is exact) and returns true; otherwise leaves everything alone. */
    virtual bool copyRegionsFrom (const BandEngineBase& other) noexcept = 0;
    /** continueFrom() from another kind of engine, after the regions were reset: the gains are
        already where they belong, so anything that normally follows them slowly may start there. */
    virtual void primeRegions() noexcept {}

    /** The band filter's memory in a form every region engine has: the state of the band's
        unity-peak band-pass, i.e. of the 2nd-order filter with the band's centre and its
        unity-gain damping R, fed with 2R x. For a band at unity gain this is exact for every
        region engine here; for a band that is applying gain it is exact for what lies at the band
        centre and close for the rest of the band (the engines then differ in damping).
        export: false if the band has no filter state to give. */
    virtual bool exportRegionState (int band, float state[kMaxChannels][2]) const noexcept = 0;
    virtual void importRegionState (int band, const float state[kMaxChannels][2]) noexcept = 0;

    /** The band's clamped log2 edges in the last processed chunk (false if it was not running). */
    bool lastBandEdges (int band, float& loLog2, float& hiLog2) const noexcept
    {
        if (! core.isActive (band) || core.numSegments() <= 0)
            return false;
        core.segmentEdges (band, core.numSegments() - 1, loLog2, hiLog2);
        return true;
    }
    /** Identifies the concrete engine type without RTTI (address of a static). */
    virtual const void* engineTypeTag() const noexcept = 0;
    /** Which closed-form response model (A3Model.h) describes this engine's band filters. */
    virtual EngineKind modelKind() const noexcept = 0;
    static const void* otherTypeTag (const BandEngineBase& other) noexcept { return other.engineTypeTag(); }

    /** Applies the gains of the core's last chunk in place to channels[c][offset .. offset + n). */
    virtual void applyChunk (float* const* channels, int numChannels, int offset, int n, std::uint64_t pos) noexcept = 0;

    DynamicsCore core;

    /** For applyChunk(): the band (0-based) whose processed component is wanted, or -1, and where
        to write it (left / right, n samples; already zeroed). */
    int soloBand = -1;
    float soloBuffer[kMaxChannels][kChunk] {};

private:
    void applyCharacter (float* const* channels, int numChannels, int offset, int n) noexcept;
    void updateAudition (bool neutral) noexcept;
    void applyAudition (float* const* channels, int numChannels, int offset, int n) noexcept;

    void applyGlobalTargets (bool neutral) noexcept;
    void applyLookahead (float* const* channels, int numChannels, int offset, int n) noexcept;

    /** One lookahead configuration: the audio delay and the gain delay of every band. */
    struct LookaheadConfig
    {
        int audio = 0;
        int gain[kMaxBands] {};
        bool isZero() const noexcept
        {
            if (audio != 0) return false;
            for (int g : gain) if (g != 0) return false;
            return true;
        }
        int longest() const noexcept
        {
            int m = audio;
            for (int g : gain) m = std::max (m, g);
            return m;
        }
        bool operator== (const LookaheadConfig&) const = default;
    };
    LookaheadConfig lookaheadTarget() const noexcept;

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
    // Lookahead: delay lines for the audio and for every band gain, crossfaded on changes.
    // A band that is not running has a history of unity gains, so a band can start (or the
    // lines can be read further back) at any time without replaying anything.
    bool lookRunning = false;
    LookaheadConfig lookCurrent, lookNext;
    bool lookFading = false;
    int lookFadePosition = 0, lookFadeLength = 480, lookWrite = 0;
    int lookValid = 0;                       // samples of real history in the lines (capped at the ring size)
    int lookMaxSamples = 480;                // 10 ms at the current sample rate
    int gainNeutralWritten[kMaxBands] {};    // unity samples written since the band last ran (capped)
    FadingDelayLine audioDelay[kMaxChannels];
    FadingDelayLine gainDelay[kMaxBands][kMaxChannels];
    float lookFade[kChunk] {};
    void resetLookaheadLines() noexcept;

    // Coloration with oversampling. Lines 0/1: the wet signal, 2/3: the dry signal (kept aligned).
    Oversampler oversampler[kMaxChannels];
    DcBlocker dcBlocker[kMaxChannels];
    SwitchableDelay<2 * kMaxChannels> characterDelay;
    LinearRamp characterMix;            // 0 = clean (delayed) path, 1 = coloured path
    LinearRamp characterDrive;          // 0..1
    int oversamplingStagesInUse = 0;
    bool characterRunning = false;
    bool inputGainNeutral = false;      // host bypass in effect for the current call
    float characterScratch[kMaxChannels][kChunk] {};
    float characterFade[kChunk] {};

    // Audition (solo / listen)
    std::atomic<int> auditionRequest { 0 };
    AuditionMode auditionMode = AuditionMode::None;     // in effect on the audio thread
    int auditionBand = -1;
    bool auditionWanted = false;                        // false while fading out for a change
    int auditionWarmup = 0;                             // samples until the delayed audition signal has arrived
    LinearRamp auditionMix;                             // 0 = normal output, 1 = audition
    SwitchableDelay<kMaxChannels> auditionDelay;        // keeps the audition aligned with the output
    int auditionDelayTarget() const noexcept;
    float auditionFade[kChunk] {};

    LinearRamp clipCeiling;             // dB
    LinearRamp clipAmount;              // 0 = clipper off, 1 = on (20 ms)
    LinearRamp clipHard;                // 0 = soft curve, 1 = hard curve (crossfaded: the ceiling holds throughout)

    // Oversampled clipper (its own factor, settings.clipOversamplingStages). While that factor is
    // on, the whole output is delayed by its filters, clipper on or off, so that switching the
    // clipper never changes the latency. The oversampled clip is crossfaded with the base-rate
    // clip of the delayed signal (factor change, start), so the ceiling holds throughout.
    Oversampler clipOversampler[kMaxChannels];
    SwitchableDelay<kMaxChannels> clipDelay;
    LinearRamp clipOsMix;               // 0 = base-rate clip of the delayed signal, 1 = oversampled clip
    int clipStagesInUse = 0;
    bool clipOsRunning = false;         // the oversampled path is being computed (its filters hold history)
    int clipWarmup = 0;                 // samples until the oversampled path's silence history has passed
    float clipScratch[kMaxChannels][kChunk] {};
    float clipCeilingBuf[kChunk] {}, clipAmountBuf[kChunk] {}, clipHardBuf[kChunk] {}, clipOsFade[kChunk] {};
    void applyClipper (float* const* channels, int nCh, int offset, int n) noexcept;

    // Overlap mode Blend: each band's gain (in dB) is reduced by what its neighbours' skirts
    // already do at its centre, so stacked bands land near their own command instead of deeper.
    // Never beyond the band's own command and never across unity (no boost from a cut, no cut from
    // a boost). Solved at each coefficient-grid point from the closed-form model, interpolated
    // across the segment, faded in and out over 20 ms.
    void applyOverlapBlend (int nCh, int n, std::uint64_t pos) noexcept;
    void updateOverlapWeights (int segment) noexcept;
    LinearRamp overlapAmount;                       // 0 = Stack, 1 = Blend
    bool overlapRunning = false;                    // corrections are in use (or fading)
    float overlapCorr[kMaxBands][kMaxChannels] {};  // linear correction at the last grid point
    // Response of band j at band i's centre for any gain G, from two numbers per pair:
    //   A-prime |H|^2 = (a + b G) / (a + b / G);   A3 |H|^2 = 1 + 2 (G - 1) a + (G - 1)^2 b.
    double overlapA[kMaxBands][kMaxBands] {}, overlapB[kMaxBands][kMaxBands] {};
    double overlapApplied[kMaxBands][kMaxChannels] {};   // dB solution at the last grid point (warm start)
    double skirtDb (int i, int j, double gain) const noexcept;
    float overlapLo[kMaxBands] {}, overlapHi[kMaxBands] {};  // log2 edges the weights were made from
    std::uint32_t overlapActiveMask = 0;            // bands the weights were made for
    bool overlapWeightsValid = false;
public:
    static constexpr double kOverlapSmallDb = 0.1;       // below this a band's skirt share is taken at this depth
    static constexpr int kOverlapIterations = 8;         // projected Gauss-Seidel sweeps per grid point
private:
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
