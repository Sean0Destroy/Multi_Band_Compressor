#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "BandDetector.h"
#include "ClipMeter.h"
#include "DspConstants.h"
#include "GrMeter.h"

// Replaceable band-processing engine interface. The processor only builds EngineSettings from
// parameters/topology once per block and hands the main-bus channels to the engine.
namespace tl::dsp
{
// Choice orders match the parameter schema (scSource, scMode, scChan, stereoMode).
enum class DetectorSource   { Main = 0, External = 1 };
enum class DetectorRange    { Own = 0, Band = 1, Custom = 2 };
enum class DetectorChannels { PerChannel = 0, Mid = 1, Side = 2, Max = 3, Sum = 4 };
enum class StereoMode       { Stereo = 0, LeftRight = 1, Mid = 2, Side = 3, MidSide = 4 };

struct BandSettings
{
    float fLoHz = 100.0f, fHiHz = 1000.0f;
    float thresholdDb = -18.0f, ratio = 2.0f, kneeDb = 6.0f;
    float attackMs = 10.0f, releaseMs = 150.0f, rangeDb = 24.0f;
    DetectorMode detectorMode = DetectorMode::Peak;
    float detectorBlend = 0.5f;   // 0 = peak, 1 = RMS (Blend mode only)
    float rmsTimeMs = 30.0f;
    float linkPercent = 100.0f;   // stereo link, 0..100
    float makeupDb = 0.0f;        // static band gain after compression (part of the wet band)
    float mixPercent = 100.0f;    // band dry/wet: scales the band's whole gain change

    // Detector routing. Every source is taken before any band processing (feed-forward).
    DetectorSource source = DetectorSource::Main;       // External with no sidechain bus: silence, 0 dB GR
    DetectorRange rangeMode = DetectorRange::Own;       // which frequency range the detector listens to
    int rangeRefSlot = 1;                               // Band: 1-based slot whose fLo/fHi are followed
    float scLoHz = 100.0f, scHiHz = 1000.0f;            // Custom range
    DetectorChannels detChannels = DetectorChannels::PerChannel;

    // Stereo: Stereo = L/R linked by linkPercent; LeftRight = independent; Mid / Side = only that
    // component of the band is processed; MidSide = both, independently. Ignored for mono.
    StereoMode stereoMode = StereoMode::Stereo;

    float lookaheadMs = 0.0f;     // 0..10 ms: this band's gain leads the audio by this much

    // Timing character (all 0..100 %, 0 = off and bit-identical to the plain compressor).
    float adaptiveReleasePercent = 0.0f;   // sustained reduction recovers slowly, brief peaks at the set release
    float freqReleasePercent = 0.0f;       // release scaled by the band centre: longer below 1 kHz, shorter above
    float transientPercent = 0.0f;         // onsets are compressed less than the sustained part

    // Character: the Warm FET curve on the band's own component after its gain (0 = off and
    // bit-identical; 100 % = the global colour stage's full drive).
    float characterPercent = 0.0f;

    // Width: how far the band's filter reaches around its centre, as a % of the span between
    // its edges (in octaves). 100 % = the edges as set; 200 % = twice the span, so the shoulders
    // reach further out; 50 % = half. The detector keeps listening to the edges as set.
    float widthPercent = 100.0f;
    bool bypass = false;
};

// The edges the band's filter uses: fLo/fHi scaled about their log-centre by widthPercent.
// At exactly 100 % the inputs are returned untouched (bit-identical to a project without Width).
struct FilterEdges { float loHz, hiHz; };

inline FilterEdges widenedEdges (float fLoHz, float fHiHz, float widthPercent) noexcept
{
    if (! (widthPercent < 100.0f || widthPercent > 100.0f) || ! std::isfinite (widthPercent) || ! (fLoHz > 0.0f) || ! (fHiHz > 0.0f))
        return { fLoHz, fHiHz };

    const double w = std::clamp ((double) widthPercent, 25.0, 400.0) * 0.01;
    const double lo = std::log2 ((double) std::min (fLoHz, fHiHz));
    const double hi = std::log2 ((double) std::max (fLoHz, fHiHz));
    const double c = 0.5 * (lo + hi), h = 0.5 * (hi - lo) * w;
    // The engines clamp to their own limits (minimum width, Nyquist) when they use the edges.
    return { (float) std::max (std::exp2 (c - h), 1.0), (float) std::exp2 (c + h) };
}

struct EngineSettings
{
    std::array<BandSettings, kMaxBands> bands {};
    std::uint32_t usedMask = 0;   // bit (slot - 1) set for used slots (BandSlots::usedMaskSnapshot)
    bool bypassAll = false;       // global bypass: every band and the global gains ramp to neutral
    float inputGainDb = 0.0f;     // before the detector and the bands
    float outputGainDb = 0.0f;    // after the global mix
    float mixPercent = 100.0f;    // global dry/wet; dry = the input after the input gain
    bool autoGain = false;        // slow loudness match of the processed signal to the dry one (before the output gain)
    int clipMode = 0;             // 0 off, 1 soft, 2 hard: the very last stage
    float clipCeilingDb = -0.3f;  // dBFS
    bool constantLatency = false; // always delay by the maximum lookahead (10 ms): latency never changes

    // Coloration after the bands (wet path) and the oversampling it runs at.
    int characterModel = 0;              // 0 clean, 1 warm FET (see Character.h)
    float characterDrivePercent = 0.0f;  // 0..100
    int oversamplingStages = 0;          // 0 off, 1 = 2x, 2 = 4x, 3 = 8x; adds latency while not off
    int clipOversamplingStages = 0;      // the output clipper's own factor (same steps); adds latency while not off
    int overlapMode = 0;                 // 0 Stack (band gains add up), 1 Blend (neighbours' skirts are taken off
                                         // each band's own gain); 2 Split is not built yet and acts as Stack
};

/** Audition: what the output carries instead of the processed signal.
      Solo:   the processed component of one band (what that band lets through, after its gain);
      Listen: the detector signal of one band (after its detector filter, before the envelope),
              6 dB down. With an external source this is the one way sidechain audio reaches
              the output.
    Not a parameter and never saved; switched with a 10 ms crossfade; off while bypassed. */
enum class AuditionMode { None = 0, Solo = 1, Listen = 2 };

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

/** Measurement hook: receives the effective band gain gEff per sample (data[band][channel]),
    i.e. what the band filter applies: compression, makeup, band mix and activity combined.
    Writes stop at `capacity`; position advances by every processed sample. */
struct GainTrace
{
    float* data[kMaxBands][kMaxChannels] {};
    long long capacity = 0;
    long long position = 0;
};

class BandEngineBase;

class IBandEngine
{
public:
    virtual ~IBandEngine() = default;

    // ---- Changing over from another engine while audio runs (optional) --------------------------
    /** Audio thread. Starts this engine as the continuation of `running`, which has just
        processed the previous block: everything the two engines have in common (detector and
        envelope states, gain ramps, lookahead history, automatic gain, position on the
        coefficient grid ...) is taken over as it is, so that from the next block on this engine
        computes the same band gains as `running` does. What is specific to this engine (its band
        filters) starts afresh. Settings must have been set. No allocation, bounded work.
        Returns false if nothing can be taken over (unrelated engines): use prepare() then. */
    virtual bool continueFrom (const IBandEngine& running) noexcept { (void) running; return false; }

    /** Seconds this engine's band filters need, with the current settings, to forget a start
        from silence (for the unheard run before a changeover is faded in). */
    virtual double getFilterSettlingSeconds() const noexcept { return 0.1; }

    /** Audio thread, on the RUNNING engine, just before another engine continues from it: start
        measuring what is needed to compare the two outputs (the loudness the automatic gain
        works with). */
    virtual void beginChangeoverMeasurement() noexcept {}
    /** Audio thread, on the INCOMING engine, when its unheard run is over and it is about to be
        faded in: bring whatever depends on this engine's own output level (automatic gain) to
        where it would be had the engine always been the one running. */
    virtual void matchOutputTo (const IBandEngine& running) noexcept { (void) running; }
    /** Audio thread, on both engines once the comparison is no longer needed. */
    virtual void endChangeoverMeasurement() noexcept {}

    /** The shared base, for engines that have it (continueFrom). */
    virtual const BandEngineBase* asBandEngineBase() const noexcept { return nullptr; }

    /** Resets all state and the absolute sample counter. Normally the message thread; the
        processor also calls it on the audio thread when the engine is switched, so it must not
        allocate, lock or block (fixed storage, bounded work). */
    virtual void prepare (double sampleRate, int maximumBlockSize) = 0;
    virtual void reset() noexcept = 0;

    /** Audio thread, before process(). Applied at the start of the next processed block. */
    virtual void setSettings (const EngineSettings&) noexcept = 0;

    /** Audio thread, before process() / processNeutral(): the external sidechain input of the next
        call only (0, 1 or 2 channels, same length as that call's block). It is read, never
        written, and never reaches the output. Without this call there is no sidechain. */
    virtual void setSidechain (const float* const* channels, int numChannels) noexcept = 0;

    /** Audio thread. In-place processing of up to kMaxChannels channels; any block length. */
    virtual void process (float* const* channels, int numChannels, int numSamples) noexcept = 0;

    /** Audio thread (host bypass). Every band ramps to neutral over the 10 ms activity ramp (no
        click when entered while compressing); after the ramp the output equals the input exactly.
        States keep running, and a later process() ramps the bands back in. */
    virtual void processNeutral (float* const* channels, int numChannels, int numSamples) noexcept = 0;

    /** Latency of the filter structure itself (0 for minimum-phase engines). The lookahead delay
        is added on top of it: see dsp::lookaheadLatencySamples(). */
    virtual int getLatencySamples() const noexcept = 0;
    virtual const GrMeter& getMeter() const noexcept = 0;
    /** What the output clipper did, column by column (see ClipMeter). */
    virtual const ClipMeter& getClipMeter() const noexcept = 0;

    /** Any thread: request an audition of slot 1..8 (None ignores the slot). Takes effect at the
        next block. */
    virtual void setAudition (AuditionMode, int slot) noexcept = 0;
    virtual AuditionMode getAuditionMode() const noexcept = 0;
    virtual int getAuditionSlot() const noexcept = 0;

    /** Gain the automatic gain stage currently applies, in dB (0 when it is off). Lock-free read. */
    virtual float getAutoGainDb() const noexcept = 0;

    /** Number of band resets caused by non-finite internal state since prepare(). */
    virtual std::uint32_t getStateResetCount() const noexcept = 0;

    /** Measurement hooks (tests / tools only, nullptr in the plugin). Must not be changed while
        process() may run concurrently. */
    virtual void setExternalGains (ExternalGains*) noexcept = 0;
    virtual void setGainTrace (GainTrace*) noexcept = 0;
};
} // namespace tl::dsp
