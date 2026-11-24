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
    for (auto& o : clipOversampler) o.prepare();
    clipDelay.prepare (core.sampleRate());
    clipOsMix.setTarget (0.0f);
    clipOsMix.prepare (core.sampleRate(), 0.010);
    overlapAmount.prepare (core.sampleRate());
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

    // The clipper's oversampling likewise: in place, from the silence before the stream.
    clipStagesInUse = std::clamp (settings.clipOversamplingStages, 0, kMaxOversamplingStages);
    clipDelay.snapTo (oversamplingLatency (clipStagesInUse));
    clipOsRunning = clipStagesInUse > 0 && ! isExactlyZero (clipAmount.value());
    clipOsMix.setTarget (clipOsRunning ? 1.0f : 0.0f);
    clipOsMix.snap();
    clipWarmup = 0;
    for (auto& o : clipOversampler) o.reset();

    // Overlap mode starts in place too.
    overlapAmount.setTarget (settings.overlapMode == 1 ? 1.0f : 0.0f);
    overlapAmount.snap();
    overlapRunning = false;
    overlapWeightsValid = false;
    for (auto& band : overlapCorr) for (auto& c : band) c = 1.0f;
    for (auto& band : overlapApplied) for (auto& c : band) c = 0.0;
}

bool BandEngineBase::continueFrom (const IBandEngine& running) noexcept
{
    const BandEngineBase* from = running.asBandEngineBase();
    if (from == nullptr || from == this)
        return false;

    // Everything below is state the two engines share by construction (this class and the
    // dynamics core). Plain data, fixed size: no allocation. The settings are this engine's own
    // (the caller set them; they are the same as the running engine's).
    const EngineSettings mine = settings;

    sampleCounter = from->sampleCounter;
    core = from->core;
    core.setSettings (mine);

    inputGain = from->inputGain;
    outputGain = from->outputGain;
    globalMix = from->globalMix;
    autoGain = from->autoGain;
    autoGainAmount = from->autoGainAmount;
    autoGainRunning = from->autoGainRunning;
    autoGainDb.store (from->autoGainDb.load (std::memory_order_relaxed), std::memory_order_relaxed);

    lookRunning = from->lookRunning;
    lookCurrent = from->lookCurrent;
    lookNext = from->lookNext;
    lookFading = from->lookFading;
    lookFadePosition = from->lookFadePosition;
    lookFadeLength = from->lookFadeLength;
    lookWrite = from->lookWrite;
    lookValid = from->lookValid;
    lookMaxSamples = from->lookMaxSamples;
    std::copy (std::begin (from->gainNeutralWritten), std::end (from->gainNeutralWritten), std::begin (gainNeutralWritten));
    for (int c = 0; c < kMaxChannels; ++c) audioDelay[c] = from->audioDelay[c];
    for (int b = 0; b < kMaxBands; ++b)
        for (int c = 0; c < kMaxChannels; ++c)
            gainDelay[b][c] = from->gainDelay[b][c];

    for (int c = 0; c < kMaxChannels; ++c)
    {
        oversampler[c] = from->oversampler[c];
        dcBlocker[c] = from->dcBlocker[c];
    }
    characterDelay = from->characterDelay;
    characterMix = from->characterMix;
    characterDrive = from->characterDrive;
    oversamplingStagesInUse = from->oversamplingStagesInUse;
    characterRunning = from->characterRunning;
    inputGainNeutral = from->inputGainNeutral;

    auditionMode = from->auditionMode;
    auditionBand = from->auditionBand;
    auditionWanted = from->auditionWanted;
    auditionWarmup = from->auditionWarmup;
    auditionMix = from->auditionMix;
    auditionDelay = from->auditionDelay;
    soloBand = from->soloBand;

    clipCeiling = from->clipCeiling;
    clipAmount = from->clipAmount;
    clipHard = from->clipHard;
    for (int c = 0; c < kMaxChannels; ++c) clipOversampler[c] = from->clipOversampler[c];
    clipDelay = from->clipDelay;
    clipOsMix = from->clipOsMix;
    clipStagesInUse = from->clipStagesInUse;
    clipOsRunning = from->clipOsRunning;
    clipWarmup = from->clipWarmup;

    overlapAmount = from->overlapAmount;
    overlapRunning = from->overlapRunning;
    for (int b = 0; b < kMaxBands; ++b)
        for (int c = 0; c < kMaxChannels; ++c)
            overlapCorr[b][c] = from->overlapCorr[b][c];
    for (int b = 0; b < kMaxBands; ++b)
        for (int c = 0; c < kMaxChannels; ++c)
            overlapApplied[b][c] = from->overlapApplied[b][c];
    overlapWeightsValid = false;          // this engine's own model

    meter.clear();
    numSidechain = 0;

    // The band filters are this engine's own. The same kind of engine continues exactly; another
    // kind starts its filters from silence (the caller lets it run unheard for
    // getFilterSettlingSeconds() before it is faded in).
    if (! copyRegionsFrom (*from))
    {
        prepareRegions (core.sampleRate());
        for (int i = 0; i < kMaxBands; ++i)
            resetRegion (i);
        primeRegions();

        // The band filters do not start from silence either: each band's memory is carried over
        // in the form both kinds of filter share (see exportRegionState). What is left for the
        // unheard run is the difference in damping of a band that is applying gain.
        float state[kMaxChannels][2] {};
        for (int i = 0; i < kMaxBands; ++i)
            if (core.isActive (i) && from->exportRegionState (i, state))
                importRegionState (i, state);
    }

    return true;
}

void BandEngineBase::matchOutputTo (const IBandEngine& running) noexcept
{
    if (const BandEngineBase* from = running.asBandEngineBase())
        if (from != this && autoGain.matchTo (from->autoGain))
            autoGainDb.store ((float) autoGain.gainDb() * autoGainAmount.value(), std::memory_order_relaxed);
}

double BandEngineBase::getFilterSettlingSeconds() const noexcept
{
    // A band filter forgets its starting state with the time constant of its band-pass,
    // 1 / (pi * bandwidth); four of them leave about 2 % (-35 dB) of the difference. A band
    // that boosts (makeup above 0 dB) may be narrower in an engine whose width follows the gain,
    // by the square root of that gain.
    double longest = 0.0;
    if (! settings.bypassAll)
        for (int i = 0; i < kMaxBands; ++i)
        {
            const auto& b = settings.bands[(size_t) i];
            if (((settings.usedMask >> i) & 1u) == 0u || b.bypass)
                continue;

            const double lo = std::min ((double) b.fLoHz, (double) b.fHiHz), hi = std::max ((double) b.fLoHz, (double) b.fHiHz);
            const double bandwidth = std::max (hi - lo, lo * (std::exp2 (1.0 / 24.0) - 1.0));     // the engine's minimum width
            if (! (bandwidth > 0.0) || ! std::isfinite (bandwidth))
                continue;

            const double boost = std::sqrt (std::max (1.0, (double) dbToLinear (std::clamp (b.makeupDb, 0.0f, 48.0f))));
            longest = std::max (longest, boost / (kPi * bandwidth));
        }

    return 4.0 * longest;
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

void BandEngineBase::applyClipper (float* const* channels, int nCh, int offset, int n) noexcept
{
    const int wantedStages = std::clamp (settings.clipOversamplingStages, 0, kMaxOversamplingStages);
    const bool clipperOn = clipAmount.isRamping() || ! isExactlyZero (clipAmount.value());

    // A change of the factor: the oversampled path fades to the base-rate clip, the delay
    // crossfades to the new latency, then the oversampled path (new factor) fades back in.
    const bool osAudible = clipOsMix.isRamping() || ! isExactlyZero (clipOsMix.value());
    if (wantedStages != clipStagesInUse && ! osAudible)
    {
        clipStagesInUse = wantedStages;
        clipOsRunning = false;                    // its history belongs to the old factor
    }

    const int delay = oversamplingLatency (clipStagesInUse);
    const bool delayActive = clipDelay.begin (delay, n);

    if (! clipperOn)
    {
        // Off: nothing is clipped; with oversampling on the output keeps its delay.
        clipOsRunning = false;
        clipOsMix.setTarget (0.0f);
        clipOsMix.snap();
        if (clipCeiling.isRamping()) clipCeiling.snap();
        if (delayActive)
        {
            for (int c = 0; c < nCh; ++c) clipDelay.process (c, channels[c] + offset, n);
            clipDelay.end (delay, n);
        }
        return;
    }

    // The oversampled path starts from silence history: it is only faded in once that history
    // has passed its filters and the delay of the base-rate path has arrived at its latency.
    const bool wantOs = clipStagesInUse > 0 && wantedStages == clipStagesInUse;
    if (wantOs && ! clipOsRunning)
    {
        for (auto& o : clipOversampler) o.reset();
        clipOsRunning = true;
        clipWarmup = delay;
    }
    clipOsMix.setTarget (wantOs && clipWarmup <= 0 && clipDelay.isSettledAt (delay) ? 1.0f : 0.0f);

    if (! delayActive && ! clipOsRunning)
    {
        // Base rate, no oversampling: the clipper as it has always been.
        clipOsMix.snap();
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
        return;
    }

    // Control values for each output sample.
    for (int j = 0; j < n; ++j)
    {
        clipCeilingBuf[j] = dbToLinear (clipCeiling.tick());
        clipAmountBuf[j] = clipAmount.tick();
        clipHardBuf[j] = clipHard.tick();
        clipOsFade[j] = clipOsMix.tick();
    }

    // Oversampled clip of the undelayed signal; the filters bring it out `delay` samples late, in
    // line with the delayed signal below. (While a ramp moves, the oversampled path sees each
    // control value `delay` samples earlier than the base-rate path: at most 55 samples.)
    if (clipOsRunning)
    {
        const int shift = clipStagesInUse;
        // The curve is usually settled (the ramp is monotonic, so equal ends mean constant):
        // then only that curve is computed (no tanh for the hard curve).
        const bool curveSettled = sameBits (clipHardBuf[0], clipHardBuf[n - 1])
                                  && (isExactlyZero (clipHardBuf[0]) || isExactlyOne (clipHardBuf[0]));
        const bool hardOnly = curveSettled && isExactlyOne (clipHardBuf[0]);
        const bool softOnly = curveSettled && isExactlyZero (clipHardBuf[0]);
        for (int c = 0; c < nCh; ++c)
        {
            float* y = clipScratch[c];
            std::copy (channels[c] + offset, channels[c] + offset + n, y);
            clipOversampler[c].process (y, n, clipStagesInUse, [&] (float* block, int count)
            {
                if (hardOnly)
                {
                    for (int i = 0; i < count; ++i) block[i] = hardClip (block[i], clipCeilingBuf[i >> shift]);
                    return;
                }
                if (softOnly)
                {
                    for (int i = 0; i < count; ++i) block[i] = softClip (block[i], clipCeilingBuf[i >> shift]);
                    return;
                }
                for (int i = 0; i < count; ++i)
                {
                    const int j = i >> shift;
                    const float ceiling = clipCeilingBuf[j], h = clipHardBuf[j], v = block[i];
                    const float soft = softClip (v, ceiling);
                    block[i] = hardClip (soft + h * (hardClip (v, ceiling) - soft), ceiling);
                }
            });

            // The decimation filter rings a little past the ceiling on clipped peaks (well under
            // 1 dB); a final clamp at the base rate keeps the ceiling exact.
            for (int j = 0; j < n; ++j)
                y[j] = hardClip (y[j], clipCeilingBuf[j]);
        }
        clipWarmup = std::max (0, clipWarmup - n);
    }

    for (int c = 0; c < nCh; ++c)
    {
        float* x = channels[c] + offset;
        if (delayActive) clipDelay.process (c, x, n);

        for (int j = 0; j < n; ++j)
        {
            const float ceiling = clipCeilingBuf[j], amount = clipAmountBuf[j], fade = clipOsFade[j];
            float clipped;
            if (clipOsRunning && isExactlyOne (fade))
            {
                clipped = clipScratch[c][j];
            }
            else
            {
                const float soft = softClip (x[j], ceiling);
                clipped = hardClip (soft + clipHardBuf[j] * (hardClip (x[j], ceiling) - soft), ceiling);
                if (clipOsRunning && ! isExactlyZero (fade))
                    clipped += fade * (clipScratch[c][j] - clipped);
            }
            x[j] = isExactlyOne (amount) ? clipped : x[j] + amount * (clipped - x[j]);
        }
    }

    if (delayActive)
        clipDelay.end (delay, n);
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

    applyOverlapBlend (nCh, n, pos);
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
    applyClipper (channels, nCh, offset, n);

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

// ---- Overlap mode -------------------------------------------------------------------------------
void BandEngineBase::updateOverlapWeights (int segment) noexcept
{
    // Two numbers per pair (band j seen at band i's digital centre) that give band j's exact
    // settled response there at any gain (see overlapA / overlapB). Rebuilt when an edge moves.
    std::uint32_t mask = 0;
    bool changed = ! overlapWeightsValid;
    for (int b = 0; b < kMaxBands; ++b)
    {
        if (! core.isActive (b)) continue;
        mask |= 1u << b;
        float lo = 0.0f, hi = 0.0f;
        core.segmentEdges (b, segment, lo, hi);
        if (! sameBits (lo, overlapLo[b]) || ! sameBits (hi, overlapHi[b]))
        {
            overlapLo[b] = lo;
            overlapHi[b] = hi;
            changed = true;
        }
    }
    if (mask != overlapActiveMask) changed = true;
    if (! changed) return;

    overlapActiveMask = mask;
    overlapWeightsValid = true;
    const double fs = core.sampleRate();
    const bool aPrime = modelKind() == EngineKind::APrime;
    double gProto[kMaxBands] {}, rProto[kMaxBands] {}, centre[kMaxBands] {};
    for (int b = 0; b < kMaxBands; ++b)
    {
        if ((mask & (1u << b)) == 0) continue;
        const double wl = std::tan (kPi * std::exp2 ((double) overlapLo[b]) / fs);
        const double wh = std::tan (kPi * std::exp2 ((double) overlapHi[b]) / fs);
        gProto[b] = std::sqrt (wl * wh);
        rProto[b] = (wh - wl) / (2.0 * gProto[b]);
        centre[b] = gProto[b];               // tan (pi fc / fs) at the band's digital centre
    }

    for (int i = 0; i < kMaxBands; ++i)
        for (int j = 0; j < kMaxBands; ++j)
        {
            overlapA[i][j] = overlapB[i][j] = 0.0;
            if (i == j || (mask & (1u << i)) == 0 || (mask & (1u << j)) == 0) continue;
            const double w = centre[i] / gProto[j], R = rProto[j];
            const double re = 1.0 - w * w, im = 2.0 * R * w;
            if (aPrime)
            {
                overlapA[i][j] = re * re;     // |s'^2 + 1|^2
                overlapB[i][j] = im * im;     // |2R s'|^2
            }
            else
            {
                // Unity-peak band-pass p = 2R s' / (s'^2 + 2R s' + 1) at s' = j w: Re p and |p|^2.
                const double den = re * re + im * im;
                overlapA[i][j] = im * im / den;
                overlapB[i][j] = im * im / den;
            }
        }
}

double BandEngineBase::skirtDb (int i, int j, double gain) const noexcept
{
    constexpr double kTenOverLn10 = 4.3429448190325182765;
    const double a = overlapA[i][j], b = overlapB[i][j];
    const double mag2 = modelKind() == EngineKind::APrime ? (a + b * gain) / (a + b / gain)
                                                          : 1.0 + 2.0 * (gain - 1.0) * a + (gain - 1.0) * (gain - 1.0) * b;
    return kTenOverLn10 * std::log (std::max (mag2, 1.0e-12));
}

void BandEngineBase::applyOverlapBlend (int nCh, int n, std::uint64_t pos) noexcept
{
    overlapAmount.setTarget (settings.overlapMode == 1 ? 1.0f : 0.0f);

    int active[kMaxBands] {};
    int numActive = 0;
    for (int b = 0; b < kMaxBands; ++b)
    {
        if (core.isActive (b)) active[numActive++] = b;
        else
            for (int c = 0; c < kMaxChannels; ++c)
            {
                overlapCorr[b][c] = 1.0f;                 // a band that starts again starts uncorrected
                overlapApplied[b][c] = 0.0;
            }
    }

    const bool off = ! overlapAmount.isRamping() && isExactlyZero (overlapAmount.value());
    if (off || numActive < 2)
    {
        // Nothing to correct: the gains pass as they are (bit-identical to Stack).
        for (int j = 0; j < n; ++j) overlapAmount.tick();
        if (overlapRunning)
            for (int b = 0; b < kMaxBands; ++b)
                for (int c = 0; c < kMaxChannels; ++c) { overlapCorr[b][c] = 1.0f; overlapApplied[b][c] = 0.0; }
        overlapRunning = false;
        return;
    }
    overlapRunning = true;

    constexpr double kLn10Over20 = 0.11512925464970228420;   // ln 10 / 20
    float* gains[kMaxBands][kMaxChannels] {};
    for (int a = 0; a < numActive; ++a)
        for (int c = 0; c < nCh; ++c)
            gains[active[a]][c] = core.mutableGains (active[a], c);

    int k = 0;
    for (int i = 0; i < n;)
    {
        const int end = segmentEnd (pos, i, n);
        updateOverlapWeights (k++);
        const double amount = (double) overlapAmount.value();
        for (int j = i; j < end; ++j) overlapAmount.tick();

        for (int c = 0; c < nCh; ++c)
        {
            // Commands (dB) at the start of the segment, and the applied ones: c_b between 0 and
            // t_b such that t_b = c_b + (skirts of the others at b's centre) wherever the bounds
            // allow. The skirts are linearised at the previous grid point's solution (exact at
            // any depth once the gains hold still), then solved by projected Gauss-Seidel.
            double t[kMaxBands] {}, applied[kMaxBands] {}, w[kMaxBands][kMaxBands] {};
            for (int a = 0; a < numActive; ++a)
            {
                const int b = active[a];
                t[b] = 20.0 * std::log10 (std::max ((double) gains[b][c][i], 1.0e-6));
                const double prev = overlapApplied[b][c];
                applied[b] = (prev < 0.0) == (t[b] < 0.0) ? std::clamp (prev, std::min (0.0, t[b]), std::max (0.0, t[b])) : t[b];
            }
            for (int o = 0; o < numActive; ++o)
            {
                const int j = active[o];
                double ref = applied[j];
                if (std::abs (ref) < kOverlapSmallDb) ref = t[j] > 0.0 ? kOverlapSmallDb : -kOverlapSmallDb;
                const double gRef = std::exp (ref * kLn10Over20);
                for (int a = 0; a < numActive; ++a)
                {
                    const int b = active[a];
                    w[b][j] = b == j ? 0.0 : std::clamp (skirtDb (b, j, gRef) / ref, 0.0, 1.0);
                }
            }
            for (int it = 0; it < kOverlapIterations; ++it)
                for (int a = 0; a < numActive; ++a)
                {
                    const int b = active[a];
                    double others = 0.0;
                    for (int o = 0; o < numActive; ++o)
                        others += w[b][active[o]] * applied[active[o]];
                    applied[b] = std::clamp (t[b] - others, std::min (0.0, t[b]), std::max (0.0, t[b]));
                }
            for (int a = 0; a < numActive; ++a)
                overlapApplied[active[a]][c] = applied[active[a]];

            for (int a = 0; a < numActive; ++a)
            {
                const int b = active[a];
                const float next = (float) std::exp (amount * (applied[b] - t[b]) * kLn10Over20);
                const float prev = overlapCorr[b][c];
                float* g = gains[b][c];
                const float step = (next - prev) / (float) (end - i);
                for (int j = i; j < end; ++j)
                {
                    const float corr = prev + step * (float) (j - i + 1);
                    const float in = g[j];
                    if (isExactlyOne (in)) continue;       // neutral stays bit-identical
                    const float out = in * corr;
                    g[j] = in < 1.0f ? std::clamp (out, in, 1.0f) : std::clamp (out, 1.0f, in);
                }
                overlapCorr[b][c] = next;
            }
        }
        i = end;
    }
}

} // namespace tl::dsp
