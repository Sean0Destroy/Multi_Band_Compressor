#include "BandEngineBase.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <algorithm>
#include <cmath>

namespace tl::dsp
{
namespace
{
/** Replaces non-finite samples with 0 (engine-level input guard). */
inline void guardInput (float* x, int n) noexcept
{
    for (int i = 0; i < n; ++i)
        if (! isFiniteFloat (x[i]))
            x[i] = 0.0f;
}
} // namespace

void BandEngineBase::prepare (double sampleRate, int /*maximumBlockSize: internal chunking, fixed buffers*/)
{
    core.prepare (sampleRate);
    prepareRegions (core.sampleRate());
    inputGain.prepare (core.sampleRate());
    outputGain.prepare (core.sampleRate());
    globalMix.prepare (core.sampleRate());

    reset();
    resetCount.store (0, std::memory_order_relaxed);
    core.applySettings();

    // Playback starts on the current values (no fade from unity).
    applyGlobalTargets (false);
    inputGain.snap();
    outputGain.snap();
    globalMix.snap();
}

void BandEngineBase::applyGlobalTargets (bool neutral) noexcept
{
    // Host bypass and the plugin's own bypass return the input: gains ramp to unity.
    const bool unity = neutral || settings.bypassAll;
    inputGain.setTargetDb (unity ? 0.0f : std::clamp (settings.inputGainDb, -48.0f, 48.0f));
    outputGain.setTargetDb (unity ? 0.0f : std::clamp (settings.outputGainDb, -48.0f, 48.0f));
    globalMix.setTarget (std::clamp (settings.mixPercent * 0.01f, 0.0f, 1.0f));
}

void BandEngineBase::reset() noexcept
{
    sampleCounter = 0;
    core.reset();

    for (int i = 0; i < kMaxBands; ++i)
        resetRegion (i);

    meter.clear();
}

void BandEngineBase::setSettings (const EngineSettings& s) noexcept
{
    settings = s;
    core.setSettings (s);
}

void BandEngineBase::process (float* const* channels, int numChannels, int numSamples) noexcept
{
    core.setActivityTargets (false);
    applyGlobalTargets (false);
    run (channels, numChannels, numSamples, false);
}

void BandEngineBase::processNeutral (float* const* channels, int numChannels, int numSamples) noexcept
{
    // Host bypass: ramp every band to neutral over the activity ramp (no click), then the output
    // is the input bit-exactly. process() restores the targets, so leaving bypass ramps back in.
    core.setActivityTargets (true);
    applyGlobalTargets (true);
    run (channels, numChannels, numSamples, true);
}

void BandEngineBase::run (float* const* channels, int numChannels, int numSamples, bool neutral) noexcept
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

void BandEngineBase::processChunk (float* const* channels, int nCh, int offset, int n, bool neutral) noexcept
{
    const std::uint64_t pos = sampleCounter;

    const float* in[kMaxChannels] {};
    for (int c = 0; c < nCh; ++c)
    {
        guardInput (channels[c] + offset, n);
        in[c] = channels[c] + offset;
    }

    // ---- Input gain (skipped while exactly unity: the signal stays bit-identical) --------------
    if (! inputGain.isUnity())
        for (int j = 0; j < n; ++j)
        {
            const float g = inputGain.tick();
            for (int c = 0; c < nCh; ++c)
                channels[c][offset + j] *= g;
        }

    // ---- Global mix: the dry signal is the input after the input gain --------------------------
    // (Correct for engines whose neutral state passes the input unchanged with zero latency.)
    const bool blend = globalMix.isRamping() || ! isExactlyOne (globalMix.value());
    if (blend)
        for (int c = 0; c < nCh; ++c)
            std::copy (in[c], in[c] + n, dry[c]);

    // ---- Dynamics: runs on the main input before any band region modifies it -------------------
    core.computeChunk (in, nCh, n, pos, neutral ? nullptr : externalGains);

    for (int bi = 0; bi < kMaxBands; ++bi)
    {
        if (! core.isActive (bi))
            continue;

        for (int c = 0; c < nCh; ++c)
            blockMinGain[bi][c] = std::min (blockMinGain[bi][c], core.minReductionGain (bi, c));

        if (gainTrace != nullptr)
            for (int c = 0; c < nCh; ++c)
                if (float* dst = gainTrace->data[bi][c])
                {
                    const float* g = core.gains (bi, c);
                    for (int j = 0; j < n && gainTrace->position + j < gainTrace->capacity; ++j)
                        dst[gainTrace->position + j] = g[j];
                }
    }

    // ---- Band filters ------------------------------------------------------------------------
    for (int bi = 0; bi < kMaxBands; ++bi)
        if (core.consumeStarted (bi))
            resetRegion (bi);

    applyChunk (channels, nCh, offset, n, pos);

    if (blend)
    {
        for (int j = 0; j < n; ++j)
            mixBuffer[j] = globalMix.tick();

        for (int c = 0; c < nCh; ++c)
        {
            float* x = channels[c] + offset;
            for (int j = 0; j < n; ++j)
                x[j] = dry[c][j] + mixBuffer[j] * (x[j] - dry[c][j]);
        }
    }

    if (! outputGain.isUnity())
        for (int j = 0; j < n; ++j)
        {
            const float g = outputGain.tick();
            for (int c = 0; c < nCh; ++c)
                channels[c][offset + j] *= g;
        }

    // ---- Safety and lifecycle ------------------------------------------------------------------
    for (int bi = 0; bi < kMaxBands; ++bi)
    {
        if (! core.isActive (bi))
            continue;

        if (! core.stateIsFinite (bi) || ! regionIsFinite (bi))
        {
            core.resetAfterFault (bi);
            resetRegion (bi);
            resetCount.fetch_add (1, std::memory_order_relaxed);
        }

        if (core.shouldRetire (bi))
        {
            core.retire (bi);   // unused and silent: skipped from now on
            resetRegion (bi);
        }
    }
}
} // namespace tl::dsp
