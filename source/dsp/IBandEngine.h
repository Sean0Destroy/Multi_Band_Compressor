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
    bool bypass = false;
};

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

/** Measurement hook: receives the effective band gain gEff per sample (data[band][channel]),
    i.e. what the band filter applies: compression, makeup, band mix and activity combined.
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

    virtual int getLatencySamples() const noexcept = 0;
    virtual const GrMeter& getMeter() const noexcept = 0;

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
