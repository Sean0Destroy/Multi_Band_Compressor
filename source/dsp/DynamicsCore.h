#pragma once

#include <cstdint>

#include "BandDetector.h"
#include "EdgeSmoother.h"
#include "GainComputer.h"
#include "GrSmoother.h"
#include "IBandEngine.h"
#include "Ramp.h"
#include "TptSvf.h"

namespace tl::dsp
{
inline constexpr double kSourceFadeSeconds = 0.005;   // main <-> external detector source crossfade

// Timing character (see BandSettings).
inline constexpr double kFreqReleaseReferenceHz = 1000.0;   // band centre at which the release is unchanged
inline constexpr double kFreqReleaseExponent = 0.5;         // release *= (1 kHz / fc)^(0.5 amount)
inline constexpr double kAdaptiveSlowFactor = 8.0;          // slow follower fall time = max (8 release, 400 ms)
inline constexpr double kAdaptiveSlowMinSeconds = 0.4;
inline constexpr double kAdaptiveChargeFactor = 2.5;        // it rises 2.5 times slower than it falls, so a brief
                                                            // peak charges it very little
inline constexpr double kTransientFastSeconds = 0.005;      // peak-hold decay of the onset detector
inline constexpr double kTransientSlowSeconds = 0.030;      // how long an onset counts as an onset

/** Release time after the frequency-dependent scaling (ms). amount 0 returns releaseMs exactly. */
inline float frequencyScaledReleaseMs (float releaseMs, float fLoHz, float fHiHz, float amountPercent) noexcept
{
    const double amount = std::clamp ((double) amountPercent * 0.01, 0.0, 1.0);
    if (amount <= 0.0 || ! (fLoHz > 0.0f) || ! (fHiHz > 0.0f))
        return releaseMs;

    const double centre = std::sqrt ((double) fLoHz * (double) fHiHz);
    const double scale = std::pow (kFreqReleaseReferenceHz / centre, kFreqReleaseExponent * amount);
    return (float) std::clamp ((double) releaseMs * scale, 1.0, 20000.0);
}

/** Engine-independent dynamics stage. Per used slot it owns the edge smoothing, the detector
    (source, frequency range and channel routing), the gain computer, stereo link, attack/release
    smoothing, makeup, mix and the activity ramp, and produces for every chunk
      - the effective linear band gain per lane and sample, and
      - the (clamped, grid-held) band edges per coefficient segment,
    which a band engine then applies with whatever filter structure it uses. It also owns the
    band lifecycle (skipped -> active -> fading -> skipped).

    Lanes: a stereo band works either on left/right (lane 0 = L, lane 1 = R) or, in the Mid, Side
    and Mid/Side stereo modes, on mid/side (lane 0 = M, lane 1 = S); see isMidSide().

    Feed-forward: the detector inputs (main input before any band processing, external sidechain)
    are supplied by the caller. No allocation, locks or I/O; fixed scratch of kChunk samples. */
class DynamicsCore
{
public:
    DynamicsCore() noexcept;

    /** Sets the sample rate and forces every time constant / coefficient to be recomputed.
        Does not reset states: call reset() afterwards. */
    void prepare (double sampleRate) noexcept;

    /** All states cleared, every band skipped (until the next applySettings()). */
    void reset() noexcept;

    void setSettings (const EngineSettings& s) noexcept { settings = s; applySettings(); }
    void applySettings() noexcept;

    /** How many samples a band's gain must have been exactly neutral before its lanes may change
        meaning (stereo mode) or the band may be retired. The owning engine sets this to the
        longest time it may still apply an older gain (its gain delay for the lookahead); 0 when
        gains are applied at once. */
    void setNeutralHold (int samples) noexcept { neutralHold = std::max (0, samples); }

    /** neutral = true: every band's activity ramps to 0 (host bypass); false: to its engaged state. */
    void setActivityTargets (bool neutral) noexcept;

    /** Processes one chunk (1 <= n <= kChunk) that starts at absolute sample `pos`.
        mainIn: numChannels (1 or 2) channels. sidechainIn: numSidechain (0, 1 or 2) channels.
        `external`, if non-null, replaces the gain of the bands it lists (measurement hook). */
    void computeChunk (const float* const* mainIn, int numChannels,
                       const float* const* sidechainIn, int numSidechain,
                       int n, std::uint64_t pos, const ExternalGains* external) noexcept;

    // ---- Results of the last computeChunk() ---------------------------------------------------
    bool isActive (int band) const noexcept                { return bands[band].active; }
    /** Used, not bypassed and no global bypass (host bypass is the engine's to know). */
    bool isEngaged (int band) const noexcept               { return bands[band].engaged; }
    /** Effective linear gain the band filter applies: 1 + activity * mix * (gComp * makeup - 1). */
    const float* gains (int band, int lane) const noexcept { return gainBuffer[band][lane]; }
    /** Mutable view of the same buffer: the owning engine delays the gains in place (lookahead). */
    float* mutableGains (int band, int lane) noexcept      { return gainBuffer[band][lane]; }
    /** Lowest compression gain of the chunk, 1 + activity * (gComp - 1): what the GR meter shows
        (makeup and band mix are not gain reduction). */
    float minReductionGain (int band, int lane) const noexcept { return minGrGain[band][lane]; }
    /** Audio thread: the band (0-based, -1 = none) whose detector signal is kept for listening. */
    void setListenBand (int band) noexcept { listenBand = band; }
    /** Detector signal of the listen band for the last chunk, per lane (zeros if it is not running). */
    const float* listenSignal (int lane) const noexcept { return listenBuffer[lane]; }
    /** True if those two lanes are mid and side (to be decoded to left / right). */
    bool listenIsMidSide() const noexcept { return listenMidSide; }

    /** True if the band's lanes are mid/side (stereo input and a Mid, Side or Mid/Side mode). */
    bool isMidSide (int band) const noexcept               { return bands[band].midSide; }
    int numSegments() const noexcept                       { return segmentCount; }

    /** Clamped log2 edges the band's coefficients are made from during segment k. */
    void segmentEdges (int band, int segment, float& loLog2, float& hiLog2) const noexcept
    {
        loLog2 = segments[band][segment].lo;
        hiLog2 = segments[band][segment].hi;
    }

    // ---- Lifecycle (driven by the owning engine after it applied the chunk) --------------------
    /** True once after a band left the skipped state: the engine must clear its own band state. */
    bool consumeStarted (int band) noexcept
    {
        const bool s = bands[band].started;
        bands[band].started = false;
        return s;
    }

    bool stateIsFinite (int band) const noexcept;
    /** Non-finite state somewhere in the band: clear the dynamics state, fade in again from 0. */
    void resetAfterFault (int band) noexcept;
    /** Unused and completely faded out (for at least the neutral hold, see setNeutralHold). */
    bool shouldRetire (int band) const noexcept
    {
        return ! bands[band].used && bands[band].activity.isSettledOff() && bands[band].zeroRun >= neutralHold;
    }
    /** Clears the state and skips the band from now on. */
    void retire (int band) noexcept;

    double sampleRate() const noexcept { return fs; }

private:
    struct Segment { SvfCoeffs hp, lp; float lo = 0.0f, hi = 0.0f; };

    struct Band
    {
        bool used = false, active = false, started = false;
        bool engaged = false;   // used, not bypassed, no global bypass (activity target outside host bypass)
        bool midSide = false;   // lanes are M/S during the current chunk
        EdgeSmoother edges;     // the band itself (what the engine filters)
        EdgeSmoother detEdges;  // what the detector listens to (own range, another band's, custom)
        ActivityRamp activity;
        int zeroRun = 0;        // consecutive samples with activity exactly 0 (capped)
        GainComputer computer;
        BandDetectorConfig detector;
        float link = 1.0f;
        GainRamp makeup;            // dB, ramped
        LinearRamp mix { 1.0f };    // 0..1, ramped
        LinearRamp sourceMix;       // 0 = main, 1 = external sidechain (5 ms crossfade)
        DetectorChannels detChannels = DetectorChannels::PerChannel;
        StereoMode stereo = StereoMode::Stereo;          // in effect
        StereoMode pendingStereo = StereoMode::Stereo;   // requested; adopted while faded out
        GrSmoother smoother[kMaxChannels];
        BandDetectorState detState[kMaxChannels];

        // Timing character
        float adaptive = 0.0f, transient = 0.0f;          // amounts 0..1
        double slowB = 1.0, slowChargeB = 1.0;            // 1 - a of the slow reduction follower (fall / rise)
        double slowAmount[kMaxChannels] {};               // slow follower of the smoothed reduction (dB)
        double onsetFast[kMaxChannels] {}, onsetSlow[kMaxChannels] {};   // onset detector envelopes (linear)
        double onsetFastA = 0.0, onsetSlowB = 1.0;
        float effectiveReleaseMs = -1.0f;
        SvfCoeffs hp, lp;                          // detector filters at the current grid edges
        float coeffLo = -1.0f, coeffHi = -1.0f;    // clamped log2 band edges on the grid
        float detLo = -1.0f, detHi = -1.0f;        // clamped log2 detector edges hp/lp were made from
        float attackMs = -1.0f, releaseMs = -1.0f, rmsMs = -1.0f;
    };

    void updateCoefficients (Band&, bool force) noexcept;
    void updateActivityTarget (Band&) noexcept;
    static void resetBandState (Band&) noexcept;
    /** Fills detA/detB (or points them at the inputs) with the two detector lane signals. */
    void buildDetectorInput (Band&, const float* const* mainIn, int nCh,
                             const float* const* sidechainIn, int nSc, int n,
                             const float*& lane0, const float*& lane1) noexcept;

    double fs = 48000.0;
    bool neutral = false;
    int neutralHold = 0;
    EngineSettings settings;
    Band bands[kMaxBands];

    // Fixed scratch (no allocation).
    float gainBuffer[kMaxBands][kMaxChannels][kChunk] {};
    float activityBuffer[kChunk] {};
    float makeupBuffer[kChunk] {};
    float mixBuffer[kChunk] {};
    float sourceBuffer[kChunk] {};
    float detA[kChunk] {}, detB[kChunk] {};
    float listenBuffer[kMaxChannels][kChunk] {};
    int listenBand = -1;
    bool listenMidSide = false;
    float minGrGain[kMaxBands][kMaxChannels] {};
    Segment segments[kMaxBands][kMaxSegments] {};
    int segmentCount = 0;
};
} // namespace tl::dsp
