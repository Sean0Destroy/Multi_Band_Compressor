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
    clipAmount.prepare (core.sampleRate());
    clipHard.prepare (core.sampleRate());
    autoGainRunning = false;
    autoGainDb.store (0.0f, std::memory_order_relaxed);
    lookFadeLength = std::max (1, (int) std::lround (kLookaheadFadeSeconds * core.sampleRate()));
    lookMaxSamples = lookaheadSamples (kMaxLookaheadMs, core.sampleRate());
    for (auto& o : oversampler) o.prepare();
    for (auto& d : dcBlocker) d.prepare (core.sampleRate());
    characterDelay.prepare (core.sampleRate());
    characterMix.setTarget (0.0f);
    characterMix.prepare (core.sampleRate(), 0.010);
    characterDrive.prepare (core.sampleRate());
    characterRunning = false;

    auditionMix.setTarget (0.0f);                       // prepare() below starts it at 0: the audition fades in
    auditionMix.prepare (core.sampleRate(), 0.010);
    auditionMode = AuditionMode::None;
    auditionBand = soloBand = -1;
    auditionWanted = false;
    auditionWarmup = 0;
    auditionDelay.prepare (core.sampleRate());
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
    clipAmount.snap();
    clipHard.snap();

    // The lookahead delays start in place: the first samples out are the delayed silence before
    // the stream (and unity gains), with no fade and no dependence on the block layout.
    resetLookaheadLines();
    lookCurrent = lookNext = lookaheadTarget();
    lookRunning = ! lookCurrent.isZero();
    lookFading = false;
    lookFadePosition = lookWrite = 0;
    lookValid = kLookaheadRing;
    core.setNeutralHold (lookRunning ? lookMaxSamples : 0);

    // Oversampling latency and coloration likewise start in place.
    oversamplingStagesInUse = std::clamp (settings.oversamplingStages, 0, kMaxOversamplingStages);
    characterDelay.snapTo (oversamplingLatency (oversamplingStagesInUse));
    const bool coloured = settings.characterModel == (int) CharacterModel::WarmFet && settings.characterDrivePercent > 0.0f
                          && ! settings.bypassAll;
    characterDrive.setTarget (std::clamp (settings.characterDrivePercent * 0.01f, 0.0f, 1.0f));
    characterDrive.snap();
    characterMix.setTarget (coloured ? 1.0f : 0.0f);
    characterMix.snap();
    characterRunning = coloured;
    for (auto& o : oversampler) o.reset();
    for (auto& d : dcBlocker) d.reset();
}

void BandEngineBase::applyCharacter (float* const* channels, int nCh, int offset, int n) noexcept
{
    const int wantedStages = std::clamp (settings.oversamplingStages, 0, kMaxOversamplingStages);
    const bool bypassed = settings.bypassAll || inputGainNeutral;
    characterDrive.setTarget (std::clamp (settings.characterDrivePercent * 0.01f, 0.0f, 1.0f));
    const bool wantColour = ! bypassed && settings.characterModel == (int) CharacterModel::WarmFet
                            && (characterDrive.isRamping() || characterDrive.targetValue() > 0.0f || characterDrive.value() > 0.0f);

    // A change of the oversampling factor is made on the clean path: fade the colour out, let
    // the delay crossfade to the new latency, then fade the colour back in.
    const bool stagesMatch = wantedStages == oversamplingStagesInUse;
    characterMix.setTarget (wantColour && stagesMatch ? 1.0f : 0.0f);
    const bool colourAudible = characterMix.isRamping() || ! isExactlyZero (characterMix.value());

    if (! stagesMatch && ! colourAudible)
    {
        oversamplingStagesInUse = wantedStages;
        for (auto& o : oversampler) o.reset();
    }

    const int delay = oversamplingLatency (oversamplingStagesInUse);
    const bool delayActive = characterDelay.begin (delay, n);

    if (! delayActive && ! colourAudible)
    {
        characterRunning = false;
        characterDrive.snap();
        return;                                   // the default: clean, no oversampling, nothing touched
    }

    if (colourAudible && ! characterRunning)
    {
        for (auto& o : oversampler) o.reset();    // the coloured path starts from silence history
        for (auto& d : dcBlocker) d.reset();
    }
    characterRunning = colourAudible;

    // Coloured path (computed from the undelayed band output; the filters bring the latency).
    if (colourAudible)
    {
        const float driveStart = characterDrive.value();
        for (int j = 0; j < n; ++j) characterDrive.tick();
        const float driveEnd = characterDrive.value();

        WarmFetCurve curveA, curveB;
        const bool activeA = curveA.set (driveStart), activeB = curveB.set (driveEnd);
        const bool moving = ! sameBits (driveStart, driveEnd);

        for (int c = 0; c < nCh; ++c)
        {
            float* y = characterScratch[c];
            std::copy (channels[c] + offset, channels[c] + offset + n, y);

            const auto shaper = [&] (float* block, int count)
            {
                if (! moving)
                {
                    if (activeB) for (int i = 0; i < count; ++i) block[i] = curveB (block[i]);
                    return;
                }
                // The drive is moving: blend from the curve at the start of the block to the
                // curve at its end.
                for (int i = 0; i < count; ++i)
                {
                    const float t = (float) (i + 1) / (float) count;
                    const float a = activeA ? curveA (block[i]) : block[i];
                    const float b = activeB ? curveB (block[i]) : block[i];
                    block[i] = a + t * (b - a);
                }
            };

            if (oversamplingStagesInUse > 0) oversampler[c].process (y, n, oversamplingStagesInUse, shaper);
            else                             shaper (y, n);

            for (int j = 0; j < n; ++j) y[j] = dcBlocker[c].tick (y[j]);
            if (! dcBlocker[c].isFinite()) dcBlocker[c].reset();
        }

        for (int j = 0; j < n; ++j) characterFade[j] = characterMix.tick();
    }
    else
    {
        characterDrive.snap();
    }

    // Clean path: the same signal delayed by the oversampling latency (exact copy); the dry
    // signal for the global mix and automatic gain is delayed alongside.
    for (int c = 0; c < nCh; ++c)
    {
        float* x = channels[c] + offset;
        if (delayActive)
        {
            characterDelay.process (c, x, n);
            characterDelay.process (kMaxChannels + c, dry[c], n);
        }

        if (colourAudible)
            for (int j = 0; j < n; ++j)
                x[j] = x[j] + characterFade[j] * (characterScratch[c][j] - x[j]);
    }

    if (delayActive)
        characterDelay.end (delay, n);
}

BandEngineBase::LookaheadConfig BandEngineBase::lookaheadTarget() const noexcept
{
    // The audio delay follows the used bands only (it is the reported latency); every slot has
    // its gain delay, used or not, so that a band that is added needs no change of delay.
    LookaheadConfig c;
    c.audio = lookaheadLatencySamples (settings, core.sampleRate());
    for (int i = 0; i < kMaxBands; ++i)
        c.gain[i] = std::max (0, c.audio - lookaheadSamples ((double) settings.bands[(size_t) i].lookaheadMs, core.sampleRate()));
    return c;
}

void BandEngineBase::resetLookaheadLines() noexcept
{
    for (auto& d : audioDelay) d.fill (0.0f);
    for (auto& band : gainDelay) for (auto& d : band) d.fill (1.0f);
    for (auto& w : gainNeutralWritten) w = kLookaheadRing;
}

void BandEngineBase::applyLookahead (float* const* channels, int nCh, int offset, int n) noexcept
{
    const auto target = lookaheadTarget();

    if (! lookRunning)
    {
        if (target.isZero())
            return;                                  // the common case: nothing to do, nothing written

        // Switching on while running: history is collected from here on (output unchanged).
        resetLookaheadLines();
        lookCurrent = lookNext = {};
        lookWrite = 0;
        lookValid = 0;
        lookRunning = true;
    }

    // From here on a gain may be applied up to 10 ms after it was computed.
    core.setNeutralHold (lookMaxSamples);

    // A tap is only faded in once the lines hold that much real history, however the target
    // moves in the meantime.
    if (! lookFading && ! (lookCurrent == target) && lookValid >= target.longest())
    {
        lookNext = target;
        lookFading = true;
        lookFadePosition = 0;
    }

    const float* fade = nullptr;
    if (lookFading)
    {
        for (int j = 0; j < n; ++j)
            lookFade[j] = std::min (1.0f, (float) (lookFadePosition + j + 1) / (float) lookFadeLength);
        fade = lookFade;
    }

    const auto& to = lookFading ? lookNext : lookCurrent;

    for (int c = 0; c < nCh; ++c)
        audioDelay[c].process (channels[c] + offset, n, lookWrite, lookCurrent.audio, to.audio, fade);

    for (int bi = 0; bi < kMaxBands; ++bi)
    {
        if (! core.isActive (bi))
        {
            // A band that is not running keeps a history of unity gains (written once round the
            // ring, then left alone), so that one that starts later reads nothing stale.
            if (gainNeutralWritten[bi] < kLookaheadRing)
            {
                for (auto& line : gainDelay[bi])
                    for (int j = 0; j < n; ++j)
                        line.buffer[(lookWrite + j) & (kLookaheadRing - 1)] = 1.0f;
                gainNeutralWritten[bi] = std::min (kLookaheadRing, gainNeutralWritten[bi] + n);
            }
            continue;
        }

        gainNeutralWritten[bi] = 0;
        for (int c = 0; c < nCh; ++c)
            gainDelay[bi][c].process (core.mutableGains (bi, c), n, lookWrite, lookCurrent.gain[bi], to.gain[bi], fade);
    }

    lookWrite = (lookWrite + n) & (kLookaheadRing - 1);
    lookValid = std::min (kLookaheadRing, lookValid + n);

    if (lookFading)
    {
        lookFadePosition += n;
        if (lookFadePosition >= lookFadeLength)
        {
            lookCurrent = lookNext;
            lookFading = false;
        }
    }

    if (! lookFading && lookCurrent.isZero() && target.isZero())
    {
        lookRunning = false;
        core.setNeutralHold (0);
    }
}

void BandEngineBase::applyGlobalTargets (bool neutral) noexcept
{
    // Host bypass and the plugin's own bypass return the input: gains ramp to unity.
    inputGainNeutral = neutral;
    const bool unity = neutral || settings.bypassAll;
    inputGain.setTargetDb (unity ? 0.0f : std::clamp (settings.inputGainDb, -48.0f, 48.0f));
    outputGain.setTargetDb (unity ? 0.0f : std::clamp (settings.outputGainDb, -48.0f, 48.0f));
    globalMix.setTarget (std::clamp (settings.mixPercent * 0.01f, 0.0f, 1.0f));
    autoGainAmount.setTarget (settings.autoGain && ! unity ? 1.0f : 0.0f);
    clipCeiling.setTarget (std::clamp (settings.clipCeilingDb, -24.0f, 0.0f));

    // The clipper fades in and out (bypass, switching it off), and a change between the two
    // curves is a crossfade of their outputs, so the ceiling holds while the curve changes.
    const auto mode = unity ? ClipMode::Off : (ClipMode) std::clamp (settings.clipMode, 0, 2);
    if (mode != ClipMode::Off)
    {
        clipHard.setTarget (mode == ClipMode::Hard ? 1.0f : 0.0f);
        if (isExactlyZero (clipAmount.value()))
            clipHard.snap();                         // not audible yet: nothing to crossfade
    }
    clipAmount.setTarget (mode != ClipMode::Off ? 1.0f : 0.0f);
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

int BandEngineBase::auditionDelayTarget() const noexcept
{
    // Solo is taken at the band filters (after the lookahead delay), Listen at the detector
    // (before it); both then skip the oversampling filters of the coloration.
    return std::min (kLookaheadRing - 1,
                     (auditionMode == AuditionMode::Listen ? lookCurrent.audio : 0) + characterDelay.currentDelay());
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
        auditionWanted = auditionMode != AuditionMode::None;
        if (! auditionWanted) auditionMix.setTarget (0.0f);
        return;
    }

    // A change is made while the audition is faded out: fade out first, then switch and fade in.
    if (auditionMode != AuditionMode::None && (auditionMix.isRamping() || ! isExactlyZero (auditionMix.value())))
    {
        auditionWanted = false;
        auditionMix.setTarget (0.0f);
        return;
    }

    auditionMode = wantedMode;
    auditionBand = wantedMode == AuditionMode::None ? -1 : wantedBand;
    soloBand = auditionMode == AuditionMode::Solo ? auditionBand : -1;
    core.setListenBand (auditionMode == AuditionMode::Listen ? auditionBand : -1);
    auditionWanted = auditionMode != AuditionMode::None;
    auditionMix.setTarget (0.0f);

    // The audition signal is delayed like the output; it is faded in once it has arrived.
    auditionDelay.snapTo (auditionDelayTarget());
    auditionWarmup = auditionDelay.currentDelay();
}

void BandEngineBase::applyAudition (float* const* channels, int nCh, int offset, int n) noexcept
{
    if (auditionMode == AuditionMode::None)
        return;

    if (auditionMode == AuditionMode::Listen)
    {
        // Detector lanes -> left / right, 6 dB down.
        const float* l0 = core.listenSignal (0);
        const float* l1 = core.listenSignal (nCh > 1 ? 1 : 0);
        const bool midSide = nCh > 1 && core.listenIsMidSide();
        for (int j = 0; j < n; ++j)
        {
            soloBuffer[0][j] = 0.5f * (midSide ? l0[j] + l1[j] : l0[j]);
            if (nCh > 1) soloBuffer[1][j] = 0.5f * (midSide ? l0[j] - l1[j] : l1[j]);
        }
    }

    const int delay = auditionDelayTarget();
    if (auditionDelay.begin (delay, n))
    {
        for (int c = 0; c < nCh; ++c)
            auditionDelay.process (c, soloBuffer[c], n);
        auditionDelay.end (delay, n);
    }

    auditionMix.setTarget (auditionWanted && auditionWarmup <= 0 ? 1.0f : 0.0f);
    if (auditionWarmup > 0) auditionWarmup -= n;

    for (int j = 0; j < n; ++j)
        auditionFade[j] = auditionMix.tick();

    const float* source[kMaxChannels] { soloBuffer[0], soloBuffer[nCh > 1 ? 1 : 0] };
    for (int c = 0; c < nCh; ++c)
    {
        float* x = channels[c] + offset;
        for (int j = 0; j < n; ++j)
        {
            // Fully on: the audition signal itself, not a sum that rounds against the output.
            const float f = auditionFade[j];
            x[j] = isExactlyOne (f) ? source[c][j] : x[j] + f * (source[c][j] - x[j]);
        }
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

    // The dry signal for the global mix and the loudness reference is the DELAYED input. It is
    // also kept while oversampling is (or was just) in use, so that its delay line has history.
    const bool keepDry = blend || autoActive || settings.oversamplingStages > 0 || oversamplingStagesInUse > 0
                         || ! characterDelay.isSettledAt (0);
    if (keepDry)
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

    // ---- Coloration (wet path) and the oversampling latency --------------------------------------
    applyCharacter (channels, nCh, offset, n);

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

    // ---- Audition: solo / listen replace the signal (crossfaded), before the clipper -----------
    applyAudition (channels, nCh, offset, n);

    // ---- Clipper: the very last stage, so the ceiling holds for whatever came before ----------
    if (clipAmount.isRamping() || ! isExactlyZero (clipAmount.value()))
    {
        const bool settled = ! clipAmount.isRamping() && isExactlyOne (clipAmount.value()) && ! clipHard.isRamping();
        const bool hard = isExactlyOne (clipHard.value());

        for (int j = 0; j < n; ++j)
        {
            const float ceiling = dbToLinear (clipCeiling.tick());

            if (settled)
            {
                for (int c = 0; c < nCh; ++c)
                {
                    float& x = channels[c][offset + j];
                    x = hard ? hardClip (x, ceiling) : softClip (x, ceiling);
                }
                continue;
            }

            // Fading in or out, or between the two curves.
            const float amount = clipAmount.tick(), h = clipHard.tick();
            for (int c = 0; c < nCh; ++c)
            {
                float& x = channels[c][offset + j];
                const float soft = softClip (x, ceiling);
                const float clipped = hardClip (soft + h * (hardClip (x, ceiling) - soft), ceiling);
                x = isExactlyOne (amount) ? clipped : x + amount * (clipped - x);
            }
        }
    }
    else if (clipCeiling.isRamping())
    {
        clipCeiling.snap();
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
