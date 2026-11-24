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
      bandSlots (apvts, undoManager, stateGuard)
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
    clipOversampling = apvts.getRawParameterValue (schema::global::clipOversampling);
    apvts.addParameterListener (schema::global::clipOversampling, this);
    jassert (clipOversampling != nullptr);
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

    // A knob edit that has finished takes its place in the history before anything reads or
    // changes the history (message thread only; elsewhere this does nothing).
    bandSlots.beforeHistoryAccess = [this] { parameterUndo.flush(); };

    // Adding or removing a band: BandSlots records for the host that state which is not a
    // parameter changed (so that the project is marked as modified) and that the latency may
    // have changed; both are delivered after the state mutex is released. What is done here,
    // with the mutex held, touches nothing but atomics.
    bandSlots.onTopologyCommitted = [this]
    {
        if (getAuditionSlot() != 0 && ! bandSlots.isUsed (getAuditionSlot()))
            setAudition (dsp::AuditionMode::None, 0);   // the auditioned band is gone
    };
    stateGuard.onLatencyChanged = [this] { requestLatencyRefresh(); };

    jassert (bypassParameter != nullptr);
}

PluginProcessor::~PluginProcessor()
{
    cancelPendingUpdate();
    for (int slot = 1; slot <= schema::kActiveSlots; ++slot)
        apvts.removeParameterListener (schema::bandParamId (slot, schema::band::lookahead), this);
    apvts.removeParameterListener (schema::global::constantLatency, this);
    apvts.removeParameterListener (schema::global::oversampling, this);
    apvts.removeParameterListener (schema::global::clipOversampling, this);
}

const juce::String PluginProcessor::getName() const { return JucePlugin_Name; }

void PluginProcessor::parameterChanged (const juce::String&, float)
{
    // Lookahead, constant latency and both oversampling choices are not automatable, so this comes from the
    // editor, a band edit or a state load. During a commit (state mutex held by this thread)
    // the host must not be called: the latency is recorded as pending and brought up to date
    // when the mutex is released.
    if (stateGuard.isHeldByCurrentThread()) stateGuard.markLatency();
    else                                    requestLatencyRefresh();
}

void PluginProcessor::requestLatencyRefresh()
{
    jassert (! stateGuard.isHeldByCurrentThread());
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
    incomingEngine = -1;
    engineFadeLength = juce::jmax (1, juce::roundToInt (0.020 * sampleRate));
    // At least one internal chunk, so that a host that announces very small blocks and then
    // sends larger ones is not cut into tiny slices.
    engineFadeScratch.setSize (dsp::kMaxChannels, juce::jmax (256, maximumExpectedSamplesPerBlock), false, false, true);
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
    s.clipOversamplingStages = juce::jlimit (0, 3, juce::roundToInt (clipOversampling->load (std::memory_order_relaxed)));

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
        if (metersStale.exchange (false, std::memory_order_relaxed))
        {
            inputMeter.clear();
            outputMeter.clear();
            sidechainMeter.clear();
            loudness.restartWindows();
        }

        analyzerIn.push (buffer.getReadPointer (0), mainChannels > 1 ? buffer.getReadPointer (1) : nullptr, numSamples);
        inputMeter.setChannelCount (mainChannels);
        for (int ch = 0; ch < mainChannels; ++ch)
            inputMeter.process (ch, buffer.getReadPointer (ch), numSamples);
    }

    const auto settings = readEngineSettings();

    // External sidechain: the input-only channels after the main input (0, 1 or 2 of them).
    const float* sidechain[dsp::kMaxChannels] {};
    int numSidechain = 0;
    const int busChannels = getSidechainChannelCount();
    for (int i = 0; i < busChannels && i < dsp::kMaxChannels && mainIn + i < buffer.getNumChannels(); ++i)
        sidechain[numSidechain++] = buffer.getReadPointer (mainIn + i);

    if (analyze)
    {
        sidechainMeter.setChannelCount (numSidechain);
        for (int ch = 0; ch < numSidechain; ++ch)
            sidechainMeter.process (ch, sidechain[ch], numSamples);
    }

    // The engines are run on slices no longer than the scratch buffer that was allocated in
    // prepareToPlay, so that a host block larger than the announced maximum is handled like any
    // other (an engine changeover needs a copy of the input).
    const int engineChannels = juce::jmin (mainChannels, (int) dsp::kMaxChannels);
    const int sliceCapacity = juce::jmax (1, engineFadeScratch.getNumSamples());
    for (int start = 0; start < numSamples; start += sliceCapacity)
    {
        float* mainSlice[dsp::kMaxChannels] {};
        const float* sidechainSlice[dsp::kMaxChannels] {};
        for (int ch = 0; ch < engineChannels; ++ch) mainSlice[ch] = buffer.getWritePointer (ch) + start;
        for (int ch = 0; ch < numSidechain; ++ch)   sidechainSlice[ch] = sidechain[ch] + start;

        processEngines (mainSlice, engineChannels, sidechainSlice, numSidechain,
                        juce::jmin (sliceCapacity, numSamples - start), settings, hostBypassed);
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

void PluginProcessor::processEngines (float* const* main, int mainChannels, const float* const* sidechain, int numSidechain,
                                      int numSamples, const dsp::EngineSettings& settings, bool hostBypassed) noexcept
{
    jassert (numSamples <= engineFadeScratch.getNumSamples());

    // Engine changeover (a listening aid). The newly chosen engine takes over everything the two
    // engines share from the one that is running (detector and envelope states, ramps, lookahead
    // history, automatic gain), so its band gains are the running engine's from the first
    // sample. Only its band filters start from silence: it runs unheard on a copy of the input
    // for as long as those need to settle, then the two outputs are crossfaded for 20 ms.
    const int wanted = juce::jlimit (0, dsp::kNumEngineKinds - 1, selectedEngine.load (std::memory_order_relaxed));
    if (incomingEngine < 0 && wanted != activeEngine)
    {
        auto& incoming = *engines[(size_t) wanted];
        incoming.setSettings (settings);

        // Automatic gain works on each engine's own output level. To fade the new engine in at
        // the loudness of the running one, both outputs are measured over the unheard run, which
        // then has to be long enough for a loudness figure to mean something.
        engines[(size_t) activeEngine]->beginChangeoverMeasurement();

        double unheardSeconds = juce::jmax (incoming.getFilterSettlingSeconds(), settings.autoGain ? kEngineLoudnessWindowSeconds : 0.0);
        if (! incoming.continueFrom (*engines[(size_t) activeEngine]))
        {
            incoming.prepare (preparedSampleRate, preparedBlockSize);     // nothing in common: a clean start
            unheardSeconds = juce::jmax (unheardSeconds, 0.100);          // (no allocation: fixed storage)
        }

        engineOutputMatched = false;
        incomingEngine = wanted;
        enginePrerollRemaining = juce::roundToInt (juce::jlimit (kEngineMinUnheardSeconds, kEngineMaxUnheardSeconds, unheardSeconds) * preparedSampleRate);
        engineFadeRemaining = engineFadeLength;
    }
    else if (incomingEngine >= 0 && wanted == activeEngine && enginePrerollRemaining > 0)
    {
        engines[(size_t) incomingEngine]->endChangeoverMeasurement();
        engines[(size_t) activeEngine]->endChangeoverMeasurement();
        incomingEngine = -1;                         // changed back before anything was heard
    }

    // The unheard run is over: before the first faded sample, the incoming engine's automatic
    // gain is set for its own output (see beginChangeoverMeasurement above).
    if (incomingEngine >= 0 && enginePrerollRemaining <= 0 && ! engineOutputMatched)
    {
        engines[(size_t) incomingEngine]->matchOutputTo (*engines[(size_t) activeEngine]);
        engines[(size_t) incomingEngine]->endChangeoverMeasurement();
        engines[(size_t) activeEngine]->endChangeoverMeasurement();
        engineOutputMatched = true;
    }

    auto& engine = engines[(size_t) activeEngine];
    engine->setSettings (settings);
    engine->setSidechain (sidechain, numSidechain);

    const bool twoEngines = incomingEngine >= 0;
    if (twoEngines)
    {
        for (int ch = 0; ch < mainChannels; ++ch)
            juce::FloatVectorOperations::copy (engineFadeScratch.getWritePointer (ch), main[ch], numSamples);

        auto& incoming = engines[(size_t) incomingEngine];
        incoming->setSettings (settings);
        incoming->setSidechain (sidechain, numSidechain);
        if (hostBypassed) incoming->processNeutral (engineFadeScratch.getArrayOfWritePointers(), mainChannels, numSamples);
        else              incoming->process (engineFadeScratch.getArrayOfWritePointers(), mainChannels, numSamples);
    }

    if (hostBypassed) engine->processNeutral (main, mainChannels, numSamples);
    else              engine->process (main, mainChannels, numSamples);

    if (twoEngines)
    {
        if (enginePrerollRemaining > 0)
        {
            enginePrerollRemaining -= numSamples;    // still unheard
        }
        else
        {
            const float step = 1.0f / (float) engineFadeLength;
            const float start = 1.0f - (float) engineFadeRemaining * step;
            for (int ch = 0; ch < mainChannels; ++ch)
            {
                float* out = main[ch];
                const float* fresh = engineFadeScratch.getReadPointer (ch);
                for (int i = 0; i < numSamples; ++i)
                {
                    const float t = juce::jmin (1.0f, start + step * (float) (i + 1));
                    out[i] = out[i] + t * (fresh[i] - out[i]);
                }
            }
            engineFadeRemaining -= numSamples;
            if (engineFadeRemaining <= 0)
            {
                activeEngine = incomingEngine;
                incomingEngine = -1;
            }
        }
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

    // The choice and its place in the saved state change together (a load on another thread
    // cannot come in between).
    const StateGuard::Edit edit (stateGuard);
    selectedEngine.store (index, std::memory_order_relaxed);
    auto nonParam = apvts.state.getChildWithName ("NonParam");
    if (nonParam.isValid())
    {
        if (index == 0) nonParam.removeProperty ("engine", nullptr);     // the default is not written
        else            nonParam.setProperty ("engine", index, nullptr);
    }
    stateGuard.markStateChanged();      // the host is told when `edit` has released the mutex
}

void PluginProcessor::setStateKeepingSession (const void* data, int size)
{
    const StateGuard::Edit edit (stateGuard);

    const auto before = apvts.state.getChildWithName ("NonParam");
    const juce::var width = before.getProperty ("editorWidth"), height = before.getProperty ("editorHeight"),
                    scale = before.getProperty ("editorScale");
    const int engineIndex = selectedEngine.load (std::memory_order_relaxed);

    loadState (data, size, false);       // the engine choice is never touched, not even for a moment

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
        put ("editorScale", scale);
        put ("engine", engineIndex == 0 ? juce::var() : juce::var (engineIndex));
    }
}

void PluginProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    // Hosts may call the state functions from any thread: keep them apart from band edits.
    // A reader: nothing is delivered to the host from here.
    const StateGuard::Lock lock (stateGuard);
    dest = state::save (apvts);
}

void PluginProcessor::setStateInformation (const void* data, int size)
{
    loadState (data, size, true);
}

void PluginProcessor::loadState (const void* data, int size, bool adoptEngine)
{
    if (size <= 0) return;

    // One commit under the state mutex; the host hears about the changed parameters and the
    // latency after `edit` has released it (see StateGuard).
    const StateGuard::Edit edit (stateGuard);

    const auto result = state::load (apvts, data, (size_t) size);

    // Whatever was set is reported, also if the load did not complete.
    for (auto* parameter : result.changed)
        stateGuard.markParameter (*parameter, false);

    if (result.ok)
    {

        bandSlots.refresh();
        parameterUndo.invalidate();     // a knob gesture open across the load is not recorded
        bandSlots.clearHistory();
        setAudition (dsp::AuditionMode::None, 0);   // never left on by a preset or a reloaded project

        // The engine choice saved with the project (absent = A3).
        if (adoptEngine)
        {
            const int savedEngine = (int) apvts.state.getChildWithName ("NonParam").getProperty ("engine", 0);
            selectedEngine.store (juce::jlimit (0, dsp::kNumEngineKinds - 1, savedEngine), std::memory_order_relaxed);
        }

        stateGuard.markLatency();
    }
}
} // namespace tl

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new tl::PluginProcessor();
}
