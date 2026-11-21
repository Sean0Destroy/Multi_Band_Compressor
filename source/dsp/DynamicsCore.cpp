#include "DynamicsCore.h"

#include <algorithm>
#include <cmath>

namespace tl::dsp
{
DynamicsCore::DynamicsCore() noexcept
{
    for (auto& b : bands)
    {
        b.edges.prepare (fs);
        b.activity.prepare (fs);
        b.makeup.prepare (fs);
        b.mix.prepare (fs);
    }
}

void DynamicsCore::prepare (double sampleRate) noexcept
{
    fs = sampleRate > 0.0 ? sampleRate : 48000.0;

    for (auto& b : bands)
    {
        b.edges.prepare (fs);
        b.activity.prepare (fs);
        b.makeup.prepare (fs);
        b.mix.prepare (fs);
        b.attackMs = b.releaseMs = b.rmsMs = -1.0f;   // force time-constant recomputation
        b.coeffLo = b.coeffHi = -1.0f;
    }
}

void DynamicsCore::reset() noexcept
{
    for (auto& b : bands)
    {
        resetBandState (b);
        b.active = false;
        b.used = false;
        b.started = false;
        b.activity.setValue (0.0f);
        b.activity.setTarget (false);
    }
}

void DynamicsCore::resetBandState (Band& b) noexcept
{
    for (int c = 0; c < kMaxChannels; ++c)
    {
        b.smoother[c].reset();
        b.detState[c].reset();
    }
}

bool DynamicsCore::stateIsFinite (int band) const noexcept
{
    const auto& b = bands[band];
    for (int c = 0; c < kMaxChannels; ++c)
        if (! b.smoother[c].isFinite() || ! b.detState[c].isFinite())
            return false;
    return true;
}

void DynamicsCore::resetAfterFault (int band) noexcept
{
    auto& b = bands[band];
    resetBandState (b);
    b.activity.setValue (0.0f);
}

void DynamicsCore::retire (int band) noexcept
{
    auto& b = bands[band];
    resetBandState (b);
    b.active = false;
}

void DynamicsCore::updateCoefficients (Band& b, bool force) noexcept
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

    b.hp = SvfCoeffs::make (wl, kButterworthR);
    b.lp = SvfCoeffs::make (wh, kButterworthR);
}

void DynamicsCore::applySettings() noexcept
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
        b.makeup.setTargetDb (std::clamp (p.makeupDb, -48.0f, 48.0f));
        b.mix.setTarget (std::clamp (p.mixPercent * 0.01f, 0.0f, 1.0f));

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
            // Becoming used from the skipped state: fresh states (the engine clears its own via
            // consumeStarted()), edges start at their targets, activity ramps in from 0.
            // Coefficients are valid immediately (then grid updates).
            resetBandState (b);
            b.edges.snapToTargets();
            b.makeup.snap();
            b.mix.snap();
            updateCoefficients (b, true);
            b.activity.setValue (0.0f);
            b.active = true;
            b.started = true;
        }

        b.engaged = b.used && ! p.bypass && ! settings.bypassAll;
        b.activity.setTarget (b.engaged);
    }
}

void DynamicsCore::setActivityTargets (bool neutral) noexcept
{
    for (auto& b : bands)
        b.activity.setTarget (neutral ? false : b.engaged);
}

void DynamicsCore::computeChunk (const float* const* in, int nCh, int n, std::uint64_t pos,
                                 const ExternalGains* external) noexcept
{
    // The segment layout only depends on the position.
    segmentCount = 0;
    for (int i = 0; i < n; i = segmentEnd (pos, i, n))
        ++segmentCount;

    for (int bi = 0; bi < kMaxBands; ++bi)
    {
        auto& b = bands[bi];
        if (! b.active)
            continue;

        // Edges (per sample), coefficients (on the grid) and activity (per sample).
        int k = 0;
        for (int i = 0; i < n;)
        {
            const int end = segmentEnd (pos, i, n);
            for (int j = i; j < end; ++j)
            {
                b.edges.tick();
                if (j == i && onGrid (pos, j))
                    updateCoefficients (b, false);
                activityBuffer[j] = b.activity.tick();
                makeupBuffer[j] = b.makeup.tick();
                mixBuffer[j] = b.mix.tick();
            }
            auto& seg = segments[bi][k++];
            seg.hp = b.hp;
            seg.lp = b.lp;
            seg.lo = b.coeffLo;
            seg.hi = b.coeffHi;
            i = end;
        }

        const float* ext = external != nullptr ? external->data[bi] : nullptr;

        if (ext != nullptr)
        {
            const long long len = std::max (1LL, external->length);
            float lowest = 1.0f;
            for (int j = 0; j < n; ++j)
            {
                const long long idx = std::min (external->position + j, len - 1);
                lowest = std::min (lowest, ext[idx]);
                for (int c = 0; c < nCh; ++c)
                    gainBuffer[bi][c][j] = ext[idx];
            }
            for (int c = 0; c < nCh; ++c) minGrGain[bi][c] = lowest;
            // The detector states are not advanced while overridden; they restart cleanly.
            for (int c = 0; c < nCh; ++c) { b.detState[c].reset(); b.smoother[c].reset(); }
            continue;
        }

        const float link = b.link, unlink = 1.0f - b.link;
        float lowest[kMaxChannels] { 1.0f, 1.0f };
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
                    const float d = detectorFilter (b.detState[c], hpC, lpC, in[c][j]);
                    gr[c] = detectorGainReduction (b.detState[c], b.detector, b.computer, d);
                }

                if (nCh == 2)
                {
                    const float lowest = std::min (gr[0], gr[1]);
                    gr[0] = unlink * gr[0] + link * lowest;
                    gr[1] = unlink * gr[1] + link * lowest;
                }

                const float a = activityBuffer[j];
                for (int c = 0; c < nCh; ++c)
                {
                    const float gComp = GrSmoother::gainFromAmount (b.smoother[c].tick (gr[c]));
                    lowest[c] = std::min (lowest[c], 1.0f + a * (gComp - 1.0f));

                    // Wet band = compression * makeup; band mix and activity scale the whole
                    // change. With makeup 0 dB and mix 100 % this is 1 + a (gComp - 1) exactly.
                    gainBuffer[bi][c][j] = 1.0f + (a * mixBuffer[j]) * (gComp * makeupBuffer[j] - 1.0f);
                }
            }
            i = end;
        }

        for (int c = 0; c < nCh; ++c) minGrGain[bi][c] = lowest[c];
    }
}
} // namespace tl::dsp
