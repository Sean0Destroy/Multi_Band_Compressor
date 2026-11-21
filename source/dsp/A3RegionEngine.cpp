#include "A3RegionEngine.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <bit>
#include <cmath>

namespace tl::dsp
{
namespace
{
bool sameBits (float a, float b) noexcept { return std::bit_cast<std::uint32_t> (a) == std::bit_cast<std::uint32_t> (b); }

/** End (exclusive) of the coefficient segment that starts at chunk index i: segments are cut
    at absolute sample positions that are multiples of kCoeffGrid. */
inline int segmentEnd (std::uint64_t chunkStart, int i, int n) noexcept
{
    const int phase = (int) ((chunkStart + (std::uint64_t) i) % (std::uint64_t) kCoeffGrid);
    return std::min (n, i + kCoeffGrid - phase);
}

inline bool onGrid (std::uint64_t chunkStart, int i) noexcept
{
    return (chunkStart + (std::uint64_t) i) % (std::uint64_t) kCoeffGrid == 0;
}

/** Replaces non-finite samples with 0 (engine-level input guard). */
inline void guardInput (float* x, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        if (! isFiniteFloat (x[i]))
            x[i] = 0.0f;
}
} // namespace

A3RegionEngine::A3RegionEngine() noexcept
{
    for (auto& b : bands)
    {
        b.edges.prepare (fs);
        b.activity.prepare (fs);
    }
}

void A3RegionEngine::prepare (double sampleRate, int /*maximumBlockSize: internal chunking, fixed buffers*/)
{
    fs = sampleRate > 0.0 ? sampleRate : 48000.0;

    for (auto& b : bands)
    {
        b.edges.prepare (fs);
        b.activity.prepare (fs);
        b.attackMs = b.releaseMs = b.rmsMs = -1.0f;   // force time-constant recomputation
        b.coeffLo = b.coeffHi = -1.0f;
    }

    reset();
    resetCount.store (0, std::memory_order_relaxed);
    applySettings();
}

void A3RegionEngine::reset() noexcept
{
    sampleCounter = 0;

    for (auto& b : bands)
    {
        resetBandState (b);
        b.active = false;
        b.used = false;
        b.activity.setValue (0.0f);
        b.activity.setTarget (false);
    }

    meter.clear();
}

void A3RegionEngine::resetBandState (Band& b) noexcept
{
    for (int c = 0; c < kMaxChannels; ++c)
    {
        b.smoother[c].reset();
        b.detState[c].reset();
        b.region[c].reset();
    }
}

bool A3RegionEngine::bandStateIsFinite (const Band& b) noexcept
{
    for (int c = 0; c < kMaxChannels; ++c)
        if (! b.smoother[c].isFinite() || ! b.detState[c].isFinite() || ! b.region[c].isFinite())
            return false;
    return true;
}

void A3RegionEngine::setSettings (const EngineSettings& s) noexcept
{
    settings = s;
    applySettings();
}

void A3RegionEngine::updateCoefficients (Band& b, bool force) noexcept
{
    float lo = 0.0f, hi = 0.0f;
    b.edges.clampedLog2 (lo, hi);

    // Identical inputs give identical coefficients, so skipping is exact (saves tan/exp2).
    if (! force && sameBits (lo, b.coeffLo) && sameBits (hi, b.coeffHi))
        return;

    b.coeffLo = lo;
    b.coeffHi = hi;

    const double wl = std::tan (kPi * std::exp2 ((double) lo) / fs);
    const double wh = std::tan (kPi * std::exp2 ((double) hi) / fs);
    const double g = std::sqrt (wl * wh);
    const double R = (wh - wl) / (2.0 * g);

    b.coeffs.a3 = SvfCoeffs::make (g, R);
    b.coeffs.hp = SvfCoeffs::make (wl, kButterworthR);
    b.coeffs.lp = SvfCoeffs::make (wh, kButterworthR);
}

void A3RegionEngine::applySettings() noexcept
{
    for (int i = 0; i < kMaxBands; ++i)
    {
        auto& b = bands[i];
        const auto& p = settings.bands[(size_t) i];
        b.used = ((settings.usedMask >> i) & 1u) != 0u;

        b.edges.setTargetsHz (p.fLoHz, p.fHiHz);
        b.computer.set (p.thresholdDb, p.ratio, p.kneeDb, p.rangeDb);
        b.detector.mode = p.detectorMode;
        b.detector.blend = std::clamp (p.detectorBlend, 0.0f, 1.0f);
        b.detector.setKneeStart (b.computer.kneeStartDb());
        b.link = std::clamp (p.linkPercent * 0.01f, 0.0f, 1.0f);

        if (! sameBits (p.attackMs, b.attackMs) || ! sameBits (p.releaseMs, b.releaseMs))
        {
            b.attackMs = p.attackMs;
            b.releaseMs = p.releaseMs;
            for (auto& sm : b.smoother) sm.setTimes (p.attackMs, p.releaseMs, fs);
        }

        if (! sameBits (p.rmsTimeMs, b.rmsMs))
        {
            b.rmsMs = p.rmsTimeMs;
            b.detector.setRmsTime (p.rmsTimeMs, fs);
        }

        if (b.used && ! b.active)
        {
            // Becoming used from the skipped state: fresh states, edges start at their targets,
            // activity ramps in from 0. Coefficients are valid immediately (then grid updates).
            resetBandState (b);
            b.edges.snapToTargets();
            updateCoefficients (b, true);
            b.activity.setValue (0.0f);
            b.active = true;
        }

        b.activity.setTarget (b.used && ! p.bypass && ! settings.bypassAll);
    }
}

void A3RegionEngine::process (float* const* channels, int numChannels, int numSamples) noexcept
{
    run (channels, numChannels, numSamples, false);
}

void A3RegionEngine::processNeutral (float* const* channels, int numChannels, int numSamples) noexcept
{
    for (auto& b : bands)
        b.activity.setValue (0.0f);

    run (channels, numChannels, numSamples, true);
}

void A3RegionEngine::run (float* const* channels, int numChannels, int numSamples, bool neutral) noexcept
{
    const int nCh = std::min (numChannels, kMaxChannels);
    if (nCh <= 0 || numSamples <= 0 || channels == nullptr)
        return;

    juce::ScopedNoDenormals noDenormals;

    for (auto& row : blockMinGain)
        for (auto& v : row) v = 1.0f;

    for (int offset = 0; offset < numSamples; offset += kChunk)
    {
        const int n = std::min (kChunk, numSamples - offset);
        processChunk (channels, nCh, offset, n, neutral);
        sampleCounter += (std::uint64_t) n;

        if (externalGains != nullptr) externalGains->position += n;
        if (gainTrace != nullptr)     gainTrace->position += n;
    }

    for (int i = 0; i < kMaxBands; ++i)
        for (int c = 0; c < kMaxChannels; ++c)
        {
            const float m = blockMinGain[i][c];
            meter.publish (i, c, m > 0.0f ? (isExactlyOne (m) ? 0.0f : 20.0f * std::log10 (m)) : -200.0f);
        }
}

void A3RegionEngine::processChunk (float* const* channels, int nCh, int offset, int n, bool neutral) noexcept
{
    const std::uint64_t pos = sampleCounter;

    for (int c = 0; c < nCh; ++c)
        guardInput (channels[c] + offset, n);

    // ---- Pass 1: per band edges/coefficients, activity and detector -> effective gains -------
    // Runs on the main input before any band region modifies it (detector source = main input).
    for (int bi = 0; bi < kMaxBands; ++bi)
    {
        auto& b = bands[bi];
        if (! b.active)
            continue;

        int k = 0;
        for (int i = 0; i < n;)
        {
            const int end = segmentEnd (pos, i, n);
            for (int j = i; j < end; ++j)
            {
                b.edges.tick();
                if (j == i && onGrid (pos, j))
                    updateCoefficients (b, false);
                activity[j] = neutral ? 0.0f : b.activity.tick();   // host bypass holds a at 0
            }
            segments[bi][k++] = b.coeffs;
            i = end;
        }

        const float* ext = (! neutral && externalGains != nullptr) ? externalGains->data[bi] : nullptr;

        if (ext != nullptr)
        {
            const long long len = std::max (1LL, externalGains->length);
            for (int j = 0; j < n; ++j)
            {
                const long long idx = std::min (externalGains->position + j, len - 1);
                for (int c = 0; c < nCh; ++c)
                    gains[bi][c][j] = ext[idx];
            }
            // The detector states are not advanced while overridden; they restart cleanly.
            for (int c = 0; c < nCh; ++c) { b.detState[c].reset(); b.smoother[c].reset(); }
        }
        else
        {
            const float link = b.link, unlink = 1.0f - b.link;
            k = 0;
            for (int i = 0; i < n;)
            {
                const int end = segmentEnd (pos, i, n);
                const SvfCoeffs hpC = segments[bi][k].hp, lpC = segments[bi][k].lp;
                ++k;

                for (int j = i; j < end; ++j)
                {
                    float gr[kMaxChannels] { 0.0f, 0.0f };
                    for (int c = 0; c < nCh; ++c)
                    {
                        const float d = detectorFilter (b.detState[c], hpC, lpC, channels[c][offset + j]);
                        gr[c] = detectorGainReduction (b.detState[c], b.detector, b.computer, d);
                    }

                    if (nCh == 2)
                    {
                        const float lowest = std::min (gr[0], gr[1]);
                        gr[0] = unlink * gr[0] + link * lowest;
                        gr[1] = unlink * gr[1] + link * lowest;
                    }

                    const float a = activity[j];
                    for (int c = 0; c < nCh; ++c)
                    {
                        const float gComp = GrSmoother::gainFromAmount (b.smoother[c].tick (gr[c]));
                        gains[bi][c][j] = 1.0f + a * (gComp - 1.0f);
                    }
                }
                i = end;
            }
        }

        for (int c = 0; c < nCh; ++c)
        {
            float m = blockMinGain[bi][c];
            for (int j = 0; j < n; ++j) m = std::min (m, gains[bi][c][j]);
            blockMinGain[bi][c] = m;
        }

        if (gainTrace != nullptr)
            for (int c = 0; c < nCh; ++c)
                if (float* dst = gainTrace->data[bi][c])
                    for (int j = 0; j < n && gainTrace->position + j < gainTrace->capacity; ++j)
                        dst[gainTrace->position + j] = gains[bi][c][j];
    }

    // ---- Pass 2: band regions in slot order (cascade), per channel -----------------------------
    for (int bi = 0; bi < kMaxBands; ++bi)
    {
        auto& b = bands[bi];
        if (! b.active)
            continue;

        for (int c = 0; c < nCh; ++c)
        {
            float* x = channels[c] + offset;
            const float* g = gains[bi][c];
            SvfState st = b.region[c];

            int k = 0;
            for (int i = 0; i < n;)
            {
                const int end = segmentEnd (pos, i, n);
                const SvfCoeffs cf = segments[bi][k++].a3;

                for (int j = i; j < end; ++j)
                {
                    const float bp = svfTick (cf, st, x[j]).bp;
                    const float cg = (g[j] - 1.0f) * cf.twoR;
                    if (! isExactlyZero (cg))           // gEff == 1: output stays bit-identical
                        x[j] = x[j] + cg * bp;
                }
                i = end;
            }
            b.region[c] = st;
        }
    }

    // ---- Safety and lifecycle --------------------------------------------------------------
    for (auto& b : bands)
    {
        if (! b.active)
            continue;

        if (! bandStateIsFinite (b))
        {
            resetBandState (b);
            b.activity.setValue (0.0f);
            resetCount.fetch_add (1, std::memory_order_relaxed);
        }

        if (! b.used && b.activity.isSettledOff())
        {
            resetBandState (b);   // unused and silent: skipped from now on
            b.active = false;
        }
    }
}
} // namespace tl::dsp
