#include "PluginProcessor.h"

#include "PluginIdentity.h"
#include "StateSerialization.h"
#include "dsp/A3RegionEngine.h"
#include "dsp/APrimeRegionEngine.h"
#include "dsp/Lookahead.h"
#include "ui/PluginEditor.h"

namespace tl
{
PluginProcessor::PluginProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)
                          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "TL_STATE", schema::createParameterLayout()),
      bandSlots (apvts, undoManager)
{
    engines[(size_t) dsp::EngineKind::A3] = std::make_unique<dsp::A3RegionEngine>();
    engines[(size_t) dsp::EngineKind::APrime] = std::make_unique<dsp::APrimeRegionEngine>();

    bypassParameter = apvts.getParameter (schema::global::bypass);
    globalBypass = apvts.getRawParameterValue (schema::global::bypass);
    inputGain = apvts.getRawParameterValue (schema::global::inGain);
    outputGain = apvts.getRawParameterValue (schema::global::outGain);
    globalMix = apvts.getRawParameterValue (schema::global::mix);
    autoGain = apvts.getRawParameterValue (schema::global::autoGain);
    clipMode = apvts.getRawParameterValue (schema::global::clipMode);
    clipCeiling = apvts.getRawParameterValue (schema::global::clipCeiling);
    jassert (inputGain != nullptr && outputGain != nullptr && globalMix != nullptr);
    constantLatency = apvts.getRawParameterValue (schema::global::constantLatency);
    apvts.addParameterListener (schema::global::constantLatency, this);
    charModel = apvts.getRawParameterValue (schema::global::charModel);
    charDrive = apvts.getRawParameterValue (schema::global::charDrive);
    oversampling = apvts.getRawParameterValue (schema::global::oversampling);
    apvts.addParameterListener (schema::global::oversampling, this);
    jassert (charModel != nullptr && charDrive != nullptr && oversampling != nullptr);
    jassert (autoGain != nullptr && clipMode != nullptr && clipCeiling != nullptr && constantLatency != nullptr);

    for (int slot = 1; slot <= schema::kActiveSlots; ++slot)
    {
        auto raw = [&] (const char* suffix)
        {
            auto* v = apvts.getRawParameterValue (schema::bandParamId (slot, suffix));
            jassert (v != nullptr);
            return v;
        };

        auto& r = bandParams[(size_t) slot - 1];
        r.fLo = raw (schema::band::fLo);          r.fHi = raw (schema::band::fHi);
        r.thresh = raw (schema::band::thresh);    r.ratio = raw (schema::band::ratio);
        r.knee = raw (schema::band::knee);        r.attack = raw (schema::band::attack);
        r.release = raw (schema::band::release);  r.range = raw (schema::band::range);
        r.detMode = raw (schema::band::detMode);  r.detBlend = raw (schema::band::detBlend);
        r.rmsTime = raw (schema::band::rmsTime);  r.link = raw (schema::band::link);
        r.makeup = raw (schema::band::makeup);    r.mix = raw (schema::band::mix);
        r.scSource = raw (schema::band::scSource); r.scMode = raw (schema::band::scMode);
        r.scRef = raw (schema::band::scRef);       r.scLo = raw (schema::band::scLo);
        r.scHi = raw (schema::band::scHi);         r.scChan = raw (schema::band::scChan);
        r.stereoMode = raw (schema::band::stereoMode);
        r.lookahead = raw (schema::band::lookahead);
        r.adaptRel = raw (schema::band::adaptRel); r.freqRel = raw (schema::band::freqRel);
        r.transient = raw (schema::band::transient);
        apvts.addParameterListener (schema::bandParamId (slot, schema::band::lookahead), this);
        r.bypass = raw (schema::band::bypass);
    }

    if (! apvts.state.getChildWithName ("NonParam").isValid())
        apvts.state.appendChild (juce::ValueTree ("NonParam"), nullptr);

    // Adding or removing a band changes saved state that is not a parameter: tell the host, so
    // that the project is marked as modified.
    bandSlots.onTopologyEdited = [this]
    {
        updateHostDisplay (juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged (true));
        refreshLatency();   // the largest lookahead of the used bands may have changed

        if (getAuditionSlot() != 0 && ! bandSlots.isUsed (getAuditionSlot()))
            setAudition (dsp::AuditionMode::None, 0);   // the auditioned band is gone
    };

    jassert (bypassParameter != nullptr);
}

PluginProcessor::~PluginProcessor()
{
    cancelPendingUpdate();
    for (int slot = 1; slot <= schema::kActiveSlots; ++slot)
        apvts.removeParameterListener (schema::bandParamId (slot, schema::band::lookahead), this);
    apvts.removeParameterListener (schema::global::constantLatency, this);
    apvts.removeParameterListener (schema::global::oversampling, this);
}

const juce::String PluginProcessor::getName() const { return JucePlugin_Name; }

void PluginProcessor::parameterChanged (const juce::String&, float)
{
    // Lookahead and constant latency are not automatable, so this comes from the editor or a
    // state load. The host is told from the message thread.
    if (juce::MessageManager::existsAndIsCurrentThread()) refreshLatency();
    else                                                 triggerAsyncUpdate();
}

void PluginProcessor::refreshLatency()
{
    const double fs = getSampleRate() > 0.0 ? getSampleRate() : 48000.0;
    const int latency = getEngine().getLatencySamples() + dsp::processingLatencySamples (readEngineSettings(), fs);
    if (latency != getLatencySamples())
        setLatencySamples (latency);
}

bool PluginProcessor::isBusesLayoutSupported (const BusesLayout& l) const
{
    if (l.inputBuses.size() != 2 || l.outputBuses.size() != 1)
        return false;

    const auto in  = l.getMainInputChannelSet();
    const auto out = l.getMainOutputChannelSet();

    if (in != out || (in != juce::AudioChannelSet::mono() && in != juce::AudioChannelSet::stereo()))
        return false;

    const auto sc = l.inputBuses.getReference (sidechainBusIndex);
    return sc.isDisabled() || sc == juce::AudioChannelSet::mono() || sc == juce::AudioChannelSet::stereo();
}

void PluginProcessor::prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock)
{
    // Settings first, so that bands used at prepare time start on their current edges (snapped).
    // The engine owns fixed-size buffers and chunks internally, so any host block size is safe.
    preparedSampleRate = sampleRate;
    preparedBlockSize = maximumExpectedSamplesPerBlock;
    for (auto& e : engines)
    {
        e->setSettings (readEngineSettings());
        e->prepare (sampleRate, maximumExpectedSamplesPerBlock);
    }
    activeEngine = selectedEngine.load (std::memory_order_relaxed);
    fadingFromEngine = -1;
    engineFadeLength = juce::jmax (1, juce::roundToInt (0.020 * sampleRate));
    engineFadeScratch.setSize (dsp::kMaxChannels, juce::jmax (1, maximumExpectedSamplesPerBlock), false, false, true);
    inputMeter.prepare (sampleRate);
    outputMeter.prepare (sampleRate);
    sidechainMeter.prepare (sampleRate);
    loudness.prepare (sampleRate);
    refreshLatency();
}

void PluginProcessor::releaseResources() {}

dsp::EngineSettings PluginProcessor::readEngineSettings() const noexcept
{
    dsp::EngineSettings s;
    s.usedMask = bandSlots.usedMaskSnapshot();
    s.bypassAll = globalBypass->load (std::memory_order_relaxed) >= 0.5f;
    s.inputGainDb = inputGain->load (std::memory_order_relaxed);
    s.outputGainDb = outputGain->load (std::memory_order_relaxed);
    s.mixPercent = globalMix->load (std::memory_order_relaxed);
    s.autoGain = autoGain->load (std::memory_order_relaxed) >= 0.5f;
    s.clipMode = juce::jlimit (0, 2, juce::roundToInt (clipMode->load (std::memory_order_relaxed)));
    s.clipCeilingDb = clipCeiling->load (std::memory_order_relaxed);
    s.constantLatency = constantLatency->load (std::memory_order_relaxed) >= 0.5f;
    s.characterModel = juce::jlimit (0, 1, juce::roundToInt (charModel->load (std::memory_order_relaxed)));
    s.characterDrivePercent = charDrive->load (std::memory_order_relaxed);
    s.oversamplingStages = juce::jlimit (0, 3, juce::roundToInt (oversampling->load (std::memory_order_relaxed)));

    for (size_t i = 0; i < bandParams.size(); ++i)
    {
        const auto& r = bandParams[i];
        auto& b = s.bands[i];
        auto get = [] (std::atomic<float>* v) { return v->load (std::memory_order_relaxed); };

        b.fLoHz = get (r.fLo);          b.fHiHz = get (r.fHi);
        b.thresholdDb = get (r.thresh); b.ratio = get (r.ratio);
        b.kneeDb = get (r.knee);        b.attackMs = get (r.attack);
        b.releaseMs = get (r.release);  b.rangeDb = get (r.range);
        b.detectorMode = (dsp::DetectorMode) juce::jlimit (0, 2, juce::roundToInt (get (r.detMode)));
        b.detectorBlend = get (r.detBlend);
        b.rmsTimeMs = get (r.rmsTime);  b.linkPercent = get (r.link);
        b.makeupDb = get (r.makeup);    b.mixPercent = get (r.mix);
        const auto choice = [&get] (std::atomic<float>* v, int last) { return juce::jlimit (0, last, juce::roundToInt (get (v))); };
        b.source = (dsp::DetectorSource) choice (r.scSource, 1);
        b.rangeMode = (dsp::DetectorRange) choice (r.scMode, 2);
        b.rangeRefSlot = juce::roundToInt (get (r.scRef));
        b.scLoHz = get (r.scLo);        b.scHiHz = get (r.scHi);
        b.detChannels = (dsp::DetectorChannels) choice (r.scChan, 4);
        b.stereoMode = (dsp::StereoMode) choice (r.stereoMode, 4);
        b.lookaheadMs = get (r.lookahead);
        b.adaptiveReleasePercent = get (r.adaptRel);
        b.freqReleasePercent = get (r.freqRel);
        b.transientPercent = get (r.transient);
        b.bypass = get (r.bypass) >= 0.5f;
    }
    return s;
}

void PluginProcessor::processMain (juce::AudioBuffer<float>& buffer, bool hostBypassed) noexcept
{
    processedBlocks.fetch_add (1, std::memory_order_relaxed);

    const int numSamples = buffer.getNumSamples();
    const int mainIn  = getMainBusNumInputChannels();
    const int mainOut = getMainBusNumOutputChannels();
    const int mainChannels = juce::jmin (mainIn, mainOut, buffer.getNumChannels());

    // The host buffer is in-place: channels [0, mainIn) carry the main input and become the
    // main output. Any sidechain channels live after them and are input-only (not used yet).
    // Output channels with no matching main input must be silent; sidechain data never reaches them.
    for (int ch = mainIn; ch < mainOut && ch < buffer.getNumChannels(); ++ch)
        juce::FloatVectorOperations::clear (buffer.getWritePointer (ch), numSamples);

    for (int ch = 0; ch < mainChannels; ++ch)
        finiteGuard.process (buffer.getWritePointer (ch), numSamples);   // input guard

    const bool analyze = mainChannels > 0 && isAnalyzerActive();
    if (analyze)
    {
        analyzerIn.push (buffer.getReadPointer (0), mainChannels > 1 ? buffer.getReadPointer (1) : nullptr, numSamples);
        inputMeter.setChannelCount (mainChannels);
        for (int ch = 0; ch < mainChannels; ++ch)
            inputMeter.process (ch, buffer.getReadPointer (ch), numSamples);
    }

    const auto settings = readEngineSettings();

    // Engine switch (a listening aid): the newly chosen engine starts from a clean state with
    // the current settings, and the two are crossfaded for 20 ms.
    const int wanted = juce::jlimit (0, dsp::kNumEngineKinds - 1, selectedEngine.load (std::memory_order_relaxed));
    if (wanted != activeEngine && fadingFromEngine < 0)
    {
        engines[(size_t) wanted]->setSettings (settings);
        engines[(size_t) wanted]->prepare (preparedSampleRate, preparedBlockSize);   // no allocation: fixed storage
        fadingFromEngine = activeEngine;
        activeEngine = wanted;
        engineFadeRemaining = engineFadeLength;
    }

    auto& engine = engines[(size_t) activeEngine];
    engine->setSettings (settings);

    // External sidechain: the input-only channels after the main input (0, 1 or 2 of them).
    const float* sidechain[dsp::kMaxChannels] {};
    int numSidechain = 0;
    const int busChannels = getSidechainChannelCount();
    for (int i = 0; i < busChannels && i < dsp::kMaxChannels && mainIn + i < buffer.getNumChannels(); ++i)
        sidechain[numSidechain++] = buffer.getReadPointer (mainIn + i);
    engine->setSidechain (sidechain, numSidechain);

    // While crossfading, the previous engine processes a copy of the input.
    const bool crossfade = fadingFromEngine >= 0 && numSamples <= engineFadeScratch.getNumSamples()
                           && mainChannels <= engineFadeScratch.getNumChannels();
    if (fadingFromEngine >= 0 && ! crossfade)
        fadingFromEngine = -1;                       // block larger than announced: switch without the fade

    if (crossfade)
    {
        for (int ch = 0; ch < mainChannels; ++ch)
            juce::FloatVectorOperations::copy (engineFadeScratch.getWritePointer (ch), buffer.getReadPointer (ch), numSamples);

        auto& previous = engines[(size_t) fadingFromEngine];
        previous->setSettings (settings);
        previous->setSidechain (sidechain, numSidechain);
        if (hostBypassed) previous->processNeutral (engineFadeScratch.getArrayOfWritePointers(), mainChannels, numSamples);
        else              previous->process (engineFadeScratch.getArrayOfWritePointers(), mainChannels, numSamples);
    }

    if (analyze)
    {
        sidechainMeter.setChannelCount (numSidechain);
        for (int ch = 0; ch < numSidechain; ++ch)
            sidechainMeter.process (ch, sidechain[ch], numSamples);
    }

    if (hostBypassed) engine->processNeutral (buffer.getArrayOfWritePointers(), mainChannels, numSamples);
    else              engine->process (buffer.getArrayOfWritePointers(), mainChannels, numSamples);

    if (crossfade)
    {
        const float step = 1.0f / (float) engineFadeLength;
        const float start = 1.0f - (float) engineFadeRemaining * step;
        for (int ch = 0; ch < mainChannels; ++ch)
        {
            float* fresh = buffer.getWritePointer (ch);
            const float* old = engineFadeScratch.getReadPointer (ch);
            for (int i = 0; i < numSamples; ++i)
            {
                const float t = juce::jmin (1.0f, start + step * (float) (i + 1));
                fresh[i] = old[i] + t * (fresh[i] - old[i]);
            }
        }
        engineFadeRemaining -= numSamples;
        if (engineFadeRemaining <= 0)
            fadingFromEngine = -1;
    }

    for (int ch = 0; ch < mainOut && ch < buffer.getNumChannels(); ++ch)
        finiteGuard.process (buffer.getWritePointer (ch), numSamples);   // output guard

    if (analyze)
    {
        analyzerOut.push (buffer.getReadPointer (0), mainChannels > 1 ? buffer.getReadPointer (1) : nullptr, numSamples);
        outputMeter.setChannelCount (mainChannels);
        for (int ch = 0; ch < mainChannels; ++ch)
            outputMeter.process (ch, buffer.getReadPointer (ch), numSamples);
        loudness.process (buffer.getReadPointer (0), mainChannels > 1 ? buffer.getReadPointer (1) : nullptr, numSamples);
    }
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    processMain (buffer, false);
}

void PluginProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    processMain (buffer, true);
}

juce::AudioProcessorEditor* PluginProcessor::createEditor()
{
    return new ui::PluginEditor (*this);
}

void PluginProcessor::setEngineKind (dsp::EngineKind kind)
{
    const int index = juce::jlimit (0, dsp::kNumEngineKinds - 1, (int) kind);
    if (index == selectedEngine.load (std::memory_order_relaxed))
        return;

    selectedEngine.store (index, std::memory_order_relaxed);

    auto nonParam = apvts.state.getChildWithName ("NonParam");
    if (nonParam.isValid())
    {
        if (index == 0) nonParam.removeProperty ("engine", nullptr);     // the default is not written
        else            nonParam.setProperty ("engine", index, nullptr);
    }
    updateHostDisplay (juce::AudioProcessor::ChangeDetails().withNonParameterStateChanged (true));
}

void PluginProcessor::setStateKeepingSession (const void* data, int size)
{
    const juce::ScopedLock sl (bandSlots.getLock());

    const auto before = apvts.state.getChildWithName ("NonParam");
    const juce::var width = before.getProperty ("editorWidth"), height = before.getProperty ("editorHeight");
    const int engineIndex = selectedEngine.load (std::memory_order_relaxed);

    setStateInformation (data, size);

    auto after = apvts.state.getChildWithName ("NonParam");
    if (after.isValid())
    {
        const auto put = [&after] (const char* key, const juce::var& v)
        {
            if (v.isVoid()) after.removeProperty (key, nullptr);
            else            after.setProperty (key, v, nullptr);
        };
        put ("editorWidth", width);
        put ("editorHeight", height);
        put ("engine", engineIndex == 0 ? juce::var() : juce::var (engineIndex));
    }
    selectedEngine.store (engineIndex, std::memory_order_relaxed);
}

void PluginProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    // Hosts may call the state functions from any thread: keep them apart from band edits.
    const juce::ScopedLock sl (bandSlots.getLock());
    dest = state::save (apvts);
}

void PluginProcessor::setStateInformation (const void* data, int size)
{
    if (size <= 0) return;

    const juce::ScopedLock sl (bandSlots.getLock());

    if (state::load (apvts, data, (size_t) size).ok)
    {
        bandSlots.refresh();
        bandSlots.clearHistory();
        setAudition (dsp::AuditionMode::None, 0);   // never left on by a preset or a reloaded project

        // The engine choice saved with the project (absent = A3).
        const int savedEngine = (int) apvts.state.getChildWithName ("NonParam").getProperty ("engine", 0);
        selectedEngine.store (juce::jlimit (0, dsp::kNumEngineKinds - 1, savedEngine), std::memory_order_relaxed);

        if (juce::MessageManager::existsAndIsCurrentThread()) refreshLatency();
        else                                                 triggerAsyncUpdate();
    }
}
} // namespace tl

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new tl::PluginProcessor();
}
