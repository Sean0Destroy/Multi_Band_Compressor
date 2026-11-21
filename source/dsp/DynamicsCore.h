#pragma once

#include <cstdint>

#include "BandDetector.h"
#include "EdgeSmoother.h"
#include "GainComputer.h"
#include "GrSmoother.h"
#include "IBandEngine.h"
#include "TptSvf.h"

namespace tl::dsp
{
/** Engine-independent dynamics stage. Per used slot it owns the edge smoothing, the detector
    (band-limited to the band's own range), the gain computer, stereo link, attack/release
    smoothing and the activity ramp, and produces for every chunk
      - the effective linear band gain per channel and sample, and
      - the (clamped, grid-held) band edges per coefficient segment,
    which a band engine then applies with whatever filter structure it uses. It also owns the
    band lifecycle (skipped -> active -> fading -> skipped).
    Feed-forward: the detector input is supplied by the caller (the main input before any band
    processing). No allocation, locks or I/O; fixed scratch of kChunk samples. */
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

    /** neutral = true: every band's activity ramps to 0 (host bypass); false: to its engaged state. */
    void setActivityTargets (bool neutral) noexcept;

    /** Processes one chunk (1 <= n <= kChunk) that starts at absolute sample `pos`.
        `external`, if non-null, replaces the gain of the bands it lists (measurement hook). */
    void computeChunk (const float* const* detectorIn, int numChannels, int n, std::uint64_t pos,
                       const ExternalGains* external) noexcept;

    // ---- Results of the last computeChunk() ---------------------------------------------------
    bool isActive (int band) const noexcept                   { return bands[band].active; }
    const float* gains (int band, int channel) const noexcept { return gainBuffer[band][channel]; }
    int numSegments() const noexcept                          { return segmentCount; }

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
    /** Unused and completely faded out. */
    bool shouldRetire (int band) const noexcept { return ! bands[band].used && bands[band].activity.isSettledOff(); }
    /** Clears the state and skips the band from now on. */
    void retire (int band) noexcept;

    double sampleRate() const noexcept { return fs; }

private:
    struct Segment { SvfCoeffs hp, lp; float lo = 0.0f, hi = 0.0f; };

    struct Band
    {
        bool used = false, active = false, started = false;
        bool engaged = false;   // used, not bypassed, no global bypass (activity target outside host bypass)
        EdgeSmoother edges;
        ActivityRamp activity;
        GainComputer computer;
        BandDetectorConfig detector;
        float link = 1.0f;
        GrSmoother smoother[kMaxChannels];
        BandDetectorState detState[kMaxChannels];
        SvfCoeffs hp, lp;                          // detector filters at the current grid edges
        float coeffLo = -1.0f, coeffHi = -1.0f;    // clamped log2 edges the coefficients were made from
        float attackMs = -1.0f, releaseMs = -1.0f, rmsMs = -1.0f;
    };

    void updateCoefficients (Band&, bool force) noexcept;
    static void resetBandState (Band&) noexcept;

    double fs = 48000.0;
    EngineSettings settings;
    Band bands[kMaxBands];

    // Fixed scratch (no allocation).
    float gainBuffer[kMaxBands][kMaxChannels][kChunk] {};
    float activityBuffer[kChunk] {};
    Segment segments[kMaxBands][kMaxSegments] {};
    int segmentCount = 0;
};
} // namespace tl::dsp
