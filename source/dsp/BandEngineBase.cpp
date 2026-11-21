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
    autoGain.prepare (core.sampleRate());
    autoGainAmount.prepare (core.sampleRate());
    clipCeiling.prepare (core.sampleRate());
    autoGainRunning = false;
    autoGainDb.store (0.0f, std::memory_order_relaxed);
    lookFadeLength = std::max (1, (int) std::lround (kLookaheadFadeSeconds * core.sampleRate()));
    auditionMix.setTarget (0.0f);                       // prepare() below starts it at 0: the audition fades in
    auditionMix.prepare (core.sampleRate(), 0.010);
    auditionMode = AuditionMode::None;
    auditionBand = soloBand = -1;
    core.setListenBand (-1);

    reset();
    resetCount.store (0, std::memory_order_relaxed);
    core.applySettings();

    // Playback starts on the current values (no fade from unity).
    applyGlobalTargets (false);
    inputGain.snap();
    outputGain.snap();
    globalMix.snap();
    autoGainAmount.snap();
    clipCeiling.snap();

    // The lookahead delays start in place: the first samples out are the delayed silence before
    // the stream, with no fade (and no dependence on the block layout).
    for (auto& d : audioDelay) d.reset();
    for (auto& band : gainDelay) for (auto& d : band) d.reset();
    lookCurrent = lookNext = lookaheadTarget();
    lookState = lookCurrent.isZero() ? LookState::Off : LookState::Running;
    lookFading = false;
    lookFadePosition = lookPrimeRemaining = lookWrite = 0;
}

BandEngineBase::LookaheadConfig BandEngineBase::lookaheadTarget() const noexcept
{
    LookaheadConfig c;
    c.audio = lookaheadLatencySamples (settings, core.sampleRate());
    for (int i = 0; i < kMaxBands; ++i)
        if (((settings.usedMask >> i) & 1u) != 0u)
            c.gain[i] = std::max (0, c.audio - lookaheadSamples ((double) settings.bands[(size_t) i].lookaheadMs, core.sampleRate()));
    return c;
}

void BandEngineBase::applyLookahead (float* const* channels, int nCh, int offset, int n) noexcept
{
    const auto target = lookaheadTarget();

    if (lookState == LookState::Off)
    {
        if (target.isZero())
            return;                                  // the common case: nothing to do, nothing written

        // Switching on while running: collect history first (output unchanged), then fade.
        for (auto& d : audioDelay) d.reset();
        for (auto& band : gainDelay) for (auto& d : band) d.reset();
        lookCurrent = lookNext = {};
        lookWrite = 0;
        lookPrimeRemaining = target.audio;
        lookState = LookState::Priming;
    }

    const float* fade = nullptr;

    if (lookState == LookState::Priming)
    {
        lookPrimeRemaining -= n;
        if (lookPrimeRemaining <= 0)
            lookState = LookState::Running;
    }
    else
    {
        if (! lookFading && ! (lookCurrent == target))
        {
            lookNext = target;
            lookFading = true;
            lookFadePosition = 0;
        }

        if (lookFading)
        {
            for (int j = 0; j < n; ++j)
                lookFade[j] = std::min (1.0f, (float) (lookFadePosition + j + 1) / (float) lookFadeLength);
            fade = lookFade;
        }
    }

    const auto& to = lookFading ? lookNext : lookCurrent;

    for (int c = 0; c < nCh; ++c)
        audioDelay[c].process (channels[c] + offset, n, lookWrite, lookCurrent.audio, to.audio, fade);

    for (int bi = 0; bi < kMaxBands; ++bi)
    {
        if (! core.isActive (bi))
        {
            // Keep the history neutral, so that a band that starts later does not read stale gains.
            if (lookCurrent.gain[bi] != 0 || to.gain[bi] != 0)
                for (int c = 0; c < nCh; ++c)
                    for (int j = 0; j < n; ++j)
                        gainDelay[bi][c].buffer[(lookWrite + j) & (kLookaheadRing - 1)] = 1.0f;
            continue;
        }

        for (int c = 0; c < nCh; ++c)
            gainDelay[bi][c].process (core.mutableGains (bi, c), n, lookWrite, lookCurrent.gain[bi], to.gain[bi], fade);
    }

    lookWrite = (lookWrite + n) & (kLookaheadRing - 1);

    if (lookFading)
    {
        lookFadePosition += n;
        if (lookFadePosition >= lookFadeLength)
        {
            lookCurrent = lookNext;
            lookFading = false;
        }
    }

    if (lookState == LookState::Running && ! lookFading && lookCurrent.isZero() && target.isZero())
        lookState = LookState::Off;
}

void BandEngineBase::applyGlobalTargets (bool neutral) noexcept
{
    // Host bypass and the plugin's own bypass return the input: gains ramp to unity.
    const bool unity = neutral || settings.bypassAll;
    inputGain.setTargetDb (unity ? 0.0f : std::clamp (settings.inputGainDb, -48.0f, 48.0f));
    outputGain.setTargetDb (unity ? 0.0f : std::clamp (settings.outputGainDb, -48.0f, 48.0f));
    globalMix.setTarget (std::clamp (settings.mixPercent * 0.01f, 0.0f, 1.0f));
    autoGainAmount.setTarget (settings.autoGain && ! unity ? 1.0f : 0.0f);
    clipCeiling.setTarget (std::clamp (settings.clipCeilingDb, -24.0f, 0.0f));
    clipMode = unity ? ClipMode::Off : (ClipMode) std::clamp (settings.clipMode, 0, 2);
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

void BandEngineBase::updateAudition (bool neutral) noexcept
{
    const int request = auditionRequest.load (std::memory_order_relaxed);
    auto wantedMode = (AuditionMode) (request & 0xf);
    const int wantedBand = (request >> 4) - 1;
    if (neutral || settings.bypassAll || wantedBand < 0) wantedMode = AuditionMode::None;   // bypass plays the input

    const bool same = wantedMode == auditionMode && (wantedMode == AuditionMode::None || wantedBand == auditionBand);
    if (same)
    {
        auditionMix.setTarget (auditionMode != AuditionMode::None ? 1.0f : 0.0f);
        return;
    }

    // A change is made while the audition is faded out: fade out first, then switch and fade in.
    if (auditionMode != AuditionMode::None && (auditionMix.isRamping() || ! isExactlyZero (auditionMix.value())))
    {
        auditionMix.setTarget (0.0f);
        return;
    }

    auditionMode = wantedMode;
    auditionBand = wantedMode == AuditionMode::None ? -1 : wantedBand;
    soloBand = auditionMode == AuditionMode::Solo ? auditionBand : -1;
    core.setListenBand (auditionMode == AuditionMode::Listen ? auditionBand : -1);
    for (auto& d : listenDelay) d.reset();
    listenWrite = 0;
    auditionMix.setTarget (auditionMode != AuditionMode::None ? 1.0f : 0.0f);
}

void BandEngineBase::applyAudition (float* const* channels, int nCh, int offset, int n) noexcept
{
    if (auditionMode == AuditionMode::None)
        return;

    const float* source[kMaxChannels] { soloBuffer[0], soloBuffer[nCh > 1 ? 1 : 0] };

    if (auditionMode == AuditionMode::Listen)
    {
        // Detector lanes -> left / right, 6 dB down, delayed like the audio (lookahead).
        const float* l0 = core.listenSignal (0);
        const float* l1 = core.listenSignal (nCh > 1 ? 1 : 0);
        const bool midSide = nCh > 1 && core.listenIsMidSide();
        for (int j = 0; j < n; ++j)
        {
            soloBuffer[0][j] = 0.5f * (midSide ? l0[j] + l1[j] : l0[j]);
            if (nCh > 1) soloBuffer[1][j] = 0.5f * (midSide ? l0[j] - l1[j] : l1[j]);
        }
        for (int c = 0; c < nCh; ++c)
            listenDelay[c].process (soloBuffer[c], n, listenWrite, lookCurrent.audio, lookCurrent.audio, nullptr);
        listenWrite = (listenWrite + n) & (kLookaheadRing - 1);
    }

    for (int j = 0; j < n; ++j)
        auditionFade[j] = auditionMix.tick();

    for (int c = 0; c < nCh; ++c)
    {
        float* x = channels[c] + offset;
        for (int j = 0; j < n; ++j)
            x[j] = x[j] + auditionFade[j] * (source[c][j] - x[j]);
    }
}

void BandEngineBase::setSidechain (const float* const* channels, int numChannels) noexcept
{
    numSidechain = channels != nullptr ? std::clamp (numChannels, 0, kMaxChannels) : 0;
    for (int c = 0; c < kMaxChannels; ++c)
        sidechain[c] = c < numSidechain ? channels[c] : nullptr;
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
    {
        numSidechain = 0;
        return;
    }

    juce::ScopedNoDenormals noDenormals;

    for (auto& row : blockMinGain)
        for (auto& v : row) v = 1.0f;

    updateAudition (neutral);

    for (int offset = 0; offset < numSamples; offset += kChunk)
    {
        const int n = std::min (kChunk, numSamples - offset);
        processChunk (channels, nCh, offset, n, neutral);
        sampleCounter += (std::uint64_t) n;

        if (externalGains != nullptr) externalGains->position += n;
        if (gainTrace != nullptr)     gainTrace->position += n;
    }

    numSidechain = 0;   // the sidechain pointers are only valid for this call

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

    // Automatic gain needs the dry signal as its loudness reference. It runs while it is on or
    // fading out; when it starts again its filters and averages start from zero.
    const bool autoActive = autoGainAmount.isRamping() || ! isExactlyZero (autoGainAmount.value());
    if (autoActive && ! autoGainRunning) autoGain.reset();
    autoGainRunning = autoActive;

    // ---- Dynamics: runs on the main input before any band region modifies it -------------------
    // The sidechain is copied (it is input-only) so that non-finite samples can be removed.
    const float* sc[kMaxChannels] {};
    for (int c = 0; c < numSidechain; ++c)
    {
        std::copy (sidechain[c] + offset, sidechain[c] + offset + n, sidechainScratch[c]);
        guardInput (sidechainScratch[c], n);
        sc[c] = sidechainScratch[c];
    }

    if (soloBand >= 0)
        for (auto& lane : soloBuffer) std::fill (lane, lane + n, 0.0f);

    core.computeChunk (in, nCh, sc, numSidechain, n, pos, neutral ? nullptr : externalGains);

    // ---- Lookahead: from here on the audio is delayed, and each band's gain by the rest --------
    applyLookahead (channels, nCh, offset, n);

    // The dry signal for the global mix and the loudness reference is the DELAYED input.
    if (blend || autoActive)
        for (int c = 0; c < nCh; ++c)
            std::copy (in[c], in[c] + n, dry[c]);

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

    // ---- Automatic gain: processed signal brought to the loudness of the dry signal -------------
    if (autoActive)
    {
        for (int j = 0; j < n; ++j)
        {
            float reference[kMaxChannels] {}, processed[kMaxChannels] {};
            for (int c = 0; c < nCh; ++c)
            {
                reference[c] = dry[c][j];
                processed[c] = channels[c][offset + j];
            }

            const float correction = autoGain.tick (reference, processed, nCh, onGrid (pos, j));
            const float g = 1.0f + autoGainAmount.tick() * (correction - 1.0f);
            for (int c = 0; c < nCh; ++c)
                channels[c][offset + j] *= g;
        }

        if (! autoGain.isFinite()) autoGain.reset();
        autoGainDb.store ((float) autoGain.gainDb() * autoGainAmount.value(), std::memory_order_relaxed);
    }
    else
    {
        autoGainDb.store (0.0f, std::memory_order_relaxed);
    }

    if (! outputGain.isUnity())
        for (int j = 0; j < n; ++j)
        {
            const float g = outputGain.tick();
            for (int c = 0; c < nCh; ++c)
                channels[c][offset + j] *= g;
        }

    // ---- Clipper: the very last stage, so the ceiling holds for whatever came before ----------
    if (clipMode != ClipMode::Off)
    {
        for (int j = 0; j < n; ++j)
        {
            const float ceiling = dbToLinear (clipCeiling.tick());
            for (int c = 0; c < nCh; ++c)
            {
                float& x = channels[c][offset + j];
                x = clipMode == ClipMode::Hard ? hardClip (x, ceiling) : softClip (x, ceiling);
            }
        }
    }
    else if (clipCeiling.isRamping())
    {
        clipCeiling.snap();
    }

    // ---- Audition: solo / listen replace the output (crossfaded) -------------------------------
    applyAudition (channels, nCh, offset, n);

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
