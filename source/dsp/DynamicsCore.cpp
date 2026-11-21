#include "DynamicsCore.h"

#include <algorithm>
#include <cmath>

namespace tl::dsp
{
namespace
{
bool isMidSideMode (StereoMode m) noexcept
{
    return m == StereoMode::Mid || m == StereoMode::Side || m == StereoMode::MidSide;
}
} // namespace

DynamicsCore::DynamicsCore() noexcept
{
    for (auto& b : bands)
    {
        b.edges.prepare (fs);
        b.detEdges.prepare (fs);
        b.activity.prepare (fs);
        b.makeup.prepare (fs);
        b.mix.prepare (fs);
        b.sourceMix.prepare (fs, kSourceFadeSeconds);
    }
}

void DynamicsCore::prepare (double sampleRate) noexcept
{
    fs = sampleRate > 0.0 ? sampleRate : 48000.0;

    for (auto& b : bands)
    {
        b.edges.prepare (fs);
        b.detEdges.prepare (fs);
        b.activity.prepare (fs);
        b.makeup.prepare (fs);
        b.mix.prepare (fs);
        b.sourceMix.prepare (fs, kSourceFadeSeconds);
        b.attackMs = b.releaseMs = b.rmsMs = b.effectiveReleaseMs = -1.0f;   // force time-constant recomputation
        b.onsetFastA = onePoleCoefficient (kTransientFastSeconds, fs);
        b.onsetSlowB = 1.0 - onePoleCoefficient (kTransientSlowSeconds, fs);
        b.coeffLo = b.coeffHi = -1.0f;
        b.detLo = b.detHi = -1.0f;
    }
}

void DynamicsCore::reset() noexcept
{
    neutral = false;

    for (auto& b : bands)
    {
        resetBandState (b);
        b.active = false;
        b.used = false;
        b.started = false;
        b.midSide = false;
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
        b.slowAmount[c] = 0.0;
        b.onsetFast[c] = b.onsetSlow[c] = 0.0;
    }
}

bool DynamicsCore::stateIsFinite (int band) const noexcept
{
    const auto& b = bands[band];
    for (int c = 0; c < kMaxChannels; ++c)
        if (! b.smoother[c].isFinite() || ! b.detState[c].isFinite()
            || ! std::isfinite (b.slowAmount[c]) || ! std::isfinite (b.onsetFast[c]) || ! std::isfinite (b.onsetSlow[c]))
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
    b.edges.clampedLog2 (b.coeffLo, b.coeffHi);

    float lo = 0.0f, hi = 0.0f;
    b.detEdges.clampedLog2 (lo, hi);

    // Identical inputs give identical coefficients, so skipping is exact (saves tan/exp2).
    if (! force && sameBits (lo, b.detLo) && sameBits (hi, b.detHi))
        return;

    b.detLo = lo;
    b.detHi = hi;

    const double wl = std::tan (kPi * std::exp2 ((double) lo) / fs);
    const double wh = std::tan (kPi * std::exp2 ((double) hi) / fs);

    b.hp = SvfCoeffs::make (wl, kButterworthR);
    b.lp = SvfCoeffs::make (wh, kButterworthR);
}

void DynamicsCore::updateActivityTarget (Band& b) noexcept
{
    // A stereo-mode change is made while the band is faded out (its lanes change meaning).
    b.activity.setTarget (! neutral && b.engaged && b.stereo == b.pendingStereo);
}

void DynamicsCore::applySettings() noexcept
{
    for (int i = 0; i < kMaxBands; ++i)
    {
        auto& b = bands[i];
        const auto& p = settings.bands[(size_t) i];
        b.used = ((settings.usedMask >> i) & 1u) != 0u;

        b.edges.setTargetsHz (p.fLoHz, p.fHiHz);

        // Detector range: the band's own, another slot's nominal range, or a custom one.
        float detLoHz = p.fLoHz, detHiHz = p.fHiHz;
        if (p.rangeMode == DetectorRange::Band && p.rangeRefSlot >= 1 && p.rangeRefSlot <= kMaxBands)
        {
            detLoHz = settings.bands[(size_t) p.rangeRefSlot - 1].fLoHz;
            detHiHz = settings.bands[(size_t) p.rangeRefSlot - 1].fHiHz;
        }
        else if (p.rangeMode == DetectorRange::Custom)
        {
            detLoHz = p.scLoHz;
            detHiHz = p.scHiHz;
        }
        b.detEdges.setTargetsHz (detLoHz, detHiHz);

        b.sourceMix.setTarget (p.source == DetectorSource::External ? 1.0f : 0.0f);
        b.detChannels = p.detChannels;
        b.pendingStereo = p.stereoMode;

        b.computer.set (p.thresholdDb, p.ratio, p.kneeDb, p.rangeDb);
        b.detector.mode = p.detectorMode;
        b.detector.blend = std::clamp (p.detectorBlend, 0.0f, 1.0f);
        b.detector.setKneeStart (b.computer.kneeStartDb());
        b.link = std::clamp (p.linkPercent * 0.01f, 0.0f, 1.0f);
        b.makeup.setTargetDb (std::clamp (p.makeupDb, -48.0f, 48.0f));
        b.mix.setTarget (std::clamp (p.mixPercent * 0.01f, 0.0f, 1.0f));

        b.adaptive = std::clamp (p.adaptiveReleasePercent * 0.01f, 0.0f, 1.0f);
        b.transient = std::clamp (p.transientPercent * 0.01f, 0.0f, 1.0f);

        // Release after the frequency-dependent scaling (exactly p.releaseMs at 0 %).
        const float release = frequencyScaledReleaseMs (p.releaseMs, p.fLoHz, p.fHiHz, p.freqReleasePercent);

        if (! sameBits (p.attackMs, b.attackMs) || ! sameBits (release, b.effectiveReleaseMs))
        {
            b.attackMs = p.attackMs;
            b.releaseMs = p.releaseMs;
            b.effectiveReleaseMs = release;
            for (auto& sm : b.smoother) sm.setTimes (p.attackMs, release, fs);
            const double slowSeconds = std::max (kAdaptiveSlowFactor * (double) release * 1.0e-3, kAdaptiveSlowMinSeconds);
            b.slowB = 1.0 - onePoleCoefficient (slowSeconds, fs);
            b.slowChargeB = 1.0 - onePoleCoefficient (kAdaptiveChargeFactor * slowSeconds, fs);
        }

        if (! sameBits (p.rmsTimeMs, b.rmsMs))
        {
            b.rmsMs = p.rmsTimeMs;
            b.detector.setRmsTime (p.rmsTimeMs, fs);
        }

        if (b.used && ! b.active)
        {
            // Becoming used from the skipped state: fresh states (the engine clears its own via
            // consumeStarted()), edges and ramps start at their targets, activity ramps in from 0.
            // Coefficients are valid immediately (then grid updates).
            resetBandState (b);
            b.edges.snapToTargets();
            b.detEdges.snapToTargets();
            b.makeup.snap();
            b.mix.snap();
            b.sourceMix.snap();
            b.stereo = b.pendingStereo;
            updateCoefficients (b, true);
            b.activity.setValue (0.0f);
            b.active = true;
            b.started = true;
        }

        b.engaged = b.used && ! p.bypass && ! settings.bypassAll;
        updateActivityTarget (b);
    }
}

void DynamicsCore::setActivityTargets (bool shouldBeNeutral) noexcept
{
    neutral = shouldBeNeutral;
    for (auto& b : bands)
        updateActivityTarget (b);
}

void DynamicsCore::buildDetectorInput (Band& b, const float* const* mainIn, int nCh,
                                       const float* const* scIn, int nSc, int n,
                                       const float*& lane0, const float*& lane1) noexcept
{
    const float* m0 = mainIn[0];
    const float* m1 = mainIn[nCh > 1 ? 1 : 0];
    const bool fading = b.sourceMix.isRamping();
    const bool mainOnly = ! fading && isExactlyZero (b.sourceMix.value());

    // Fast path (the default): main input, per-channel detection, left/right lanes.
    if (mainOnly && b.detChannels == DetectorChannels::PerChannel && ! b.midSide)
    {
        lane0 = m0;
        lane1 = m1;
        return;
    }

    // Source lanes s0/s1: main, external sidechain (mono is duplicated, absent is silence) or
    // their crossfade while the source is being switched.
    const float* e0 = nSc > 0 ? scIn[0] : nullptr;
    const float* e1 = nSc > 0 ? scIn[nSc > 1 ? 1 : 0] : nullptr;

    if (fading)
        for (int j = 0; j < n; ++j)
            sourceBuffer[j] = b.sourceMix.tick();

    const float held = b.sourceMix.value();
    const bool stereoOut = nCh > 1;
    auto mode = b.detChannels;
    if (! stereoOut && (mode == DetectorChannels::PerChannel || mode == DetectorChannels::Max))
        mode = DetectorChannels::Mid;    // one lane: a stereo sidechain is summed to mono

    for (int j = 0; j < n; ++j)
    {
        const float x = fading ? sourceBuffer[j] : held;
        const float ext0 = e0 != nullptr ? e0[j] : 0.0f;
        const float ext1 = e1 != nullptr ? e1[j] : 0.0f;
        const float s0 = mainOnly ? m0[j] : m0[j] + x * (ext0 - m0[j]);
        const float s1 = mainOnly ? m1[j] : m1[j] + x * (ext1 - m1[j]);

        switch (mode)
        {
            case DetectorChannels::Mid:  detA[j] = detB[j] = 0.5f * (s0 + s1); break;
            case DetectorChannels::Side: detA[j] = detB[j] = 0.5f * (s0 - s1); break;
            case DetectorChannels::Sum:  detA[j] = detB[j] = s0 + s1;          break;
            case DetectorChannels::PerChannel:
            case DetectorChannels::Max:
            default:
                // Per lane: left/right, or mid/side when the band works on mid/side.
                detA[j] = b.midSide ? 0.5f * (s0 + s1) : s0;
                detB[j] = b.midSide ? 0.5f * (s0 - s1) : s1;
                break;
        }
    }

    lane0 = detA;
    lane1 = detB;
}

void DynamicsCore::computeChunk (const float* const* mainIn, int nCh,
                                 const float* const* scIn, int nSc,
                                 int n, std::uint64_t pos, const ExternalGains* external) noexcept
{
    // The segment layout only depends on the position.
    segmentCount = 0;
    for (int i = 0; i < n; i = segmentEnd (pos, i, n))
        ++segmentCount;

    if (listenBand >= 0)
    {
        for (auto& lane : listenBuffer) std::fill (lane, lane + n, 0.0f);
        listenMidSide = false;
    }

    for (int bi = 0; bi < kMaxBands; ++bi)
    {
        auto& b = bands[bi];
        if (! b.active)
            continue;

        // Adopt a requested stereo mode once the band has faded out; it then fades back in.
        if (b.stereo != b.pendingStereo && isExactlyZero (b.activity.value()))
        {
            b.stereo = b.pendingStereo;
            resetBandState (b);          // the lanes change meaning (L/R <-> M/S)
            updateActivityTarget (b);
        }
        b.midSide = nCh == 2 && isMidSideMode (b.stereo);

        // Edges (per sample), coefficients (on the grid), activity and gain ramps (per sample).
        int k = 0;
        for (int i = 0; i < n;)
        {
            const int end = segmentEnd (pos, i, n);
            for (int j = i; j < end; ++j)
            {
                b.edges.tick();
                b.detEdges.tick();
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

        const float* det[kMaxChannels] { nullptr, nullptr };
        buildDetectorInput (b, mainIn, nCh, scIn, nSc, n, det[0], det[1]);

        // Lane linking: Stereo uses the link amount, the independent modes none; the Max
        // detector applies the deeper reduction to both lanes.
        float link = b.stereo == StereoMode::Stereo ? b.link : 0.0f;
        if (b.detChannels == DetectorChannels::Max) link = 1.0f;
        const float unlink = 1.0f - link;

        float lowest[kMaxChannels] { 1.0f, 1.0f };
        const bool listening = bi == listenBand;
        if (listening)
            listenMidSide = b.midSide && (b.detChannels == DetectorChannels::PerChannel || b.detChannels == DetectorChannels::Max);

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
                    const float d = detectorFilter (b.detState[c], hpC, lpC, det[c][j]);
                    gr[c] = detectorGainReduction (b.detState[c], b.detector, b.computer, d);
                    if (listening) listenBuffer[c][j] = d;

                    if (b.transient > 0.0f)
                    {
                        // Onset detector: a peak follower with a 5 ms decay against its own 30 ms
                        // average. On a steady signal the two agree (no effect); at an onset the
                        // fast one is ahead, and the reduction command is scaled back for it.
                        const double a = std::abs ((double) d);
                        b.onsetFast[c] = std::max (a, b.onsetFastA * b.onsetFast[c]);
                        b.onsetSlow[c] += b.onsetSlowB * (b.onsetFast[c] - b.onsetSlow[c]);
                        if (gr[c] < 0.0f && b.onsetFast[c] > 1.0e-9)
                        {
                            const double onset = std::clamp (1.0 - b.onsetSlow[c] / b.onsetFast[c], 0.0, 1.0);
                            gr[c] *= (float) (1.0 - (double) b.transient * onset);
                        }
                    }
                }

                if (nCh == 2)
                {
                    const float deepest = std::min (gr[0], gr[1]);
                    gr[0] = unlink * gr[0] + link * deepest;
                    gr[1] = unlink * gr[1] + link * deepest;
                }

                const float a = activityBuffer[j];
                for (int c = 0; c < nCh; ++c)
                {
                    float amount = b.smoother[c].tick (gr[c]);

                    if (b.adaptive > 0.0f)
                    {
                        // Adaptive release: a slow follower of the reduction sets a floor for the
                        // recovery. A brief peak leaves it near 0 (recovery at the set release);
                        // sustained reduction charges it, and the last part recovers slowly.
                        // Never more reduction than the smoothed reduction reached.
                        const double difference = (double) amount - b.slowAmount[c];
                        b.slowAmount[c] += (difference > 0.0 ? b.slowChargeB : b.slowB) * difference;
                        amount = std::max (amount, (float) ((double) b.adaptive * b.slowAmount[c]));
                    }

                    const float gComp = GrSmoother::gainFromAmount (amount);
                    lowest[c] = std::min (lowest[c], 1.0f + a * (gComp - 1.0f));

                    // Wet band = compression * makeup; band mix and activity scale the whole
                    // change. With makeup 0 dB and mix 100 % this is 1 + a (gComp - 1) exactly.
                    gainBuffer[bi][c][j] = 1.0f + (a * mixBuffer[j]) * (gComp * makeupBuffer[j] - 1.0f);
                }
            }
            i = end;
        }

        // Mid only / Side only: the other component of the band is left untouched.
        if (b.midSide && b.stereo != StereoMode::MidSide)
        {
            const int idle = b.stereo == StereoMode::Mid ? 1 : 0;
            std::fill (gainBuffer[bi][idle], gainBuffer[bi][idle] + n, 1.0f);
            lowest[idle] = 1.0f;
        }

        for (int c = 0; c < nCh; ++c) minGrGain[bi][c] = lowest[c];
    }
}
} // namespace tl::dsp
