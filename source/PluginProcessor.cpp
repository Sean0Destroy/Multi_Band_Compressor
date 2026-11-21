#include "PluginProcessor.h"

#include "PluginIdentity.h"
#include "StateSerialization.h"
#include "dsp/A3RegionEngine.h"

namespace tl
{
PluginProcessor::PluginProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)
                          .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), false)),
      apvts (*this, nullptr, "TL_STATE", schema::createParameterLayout()),
      bandSlots (apvts, undoManager),
      engine (std::make_unique<dsp::A3RegionEngine>())
{
    bypassParameter = apvts.getParameter (schema::global::bypass);
    globalBypass = apvts.getRawParameterValue (schema::global::bypass);

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
        r.bypass = raw (schema::band::bypass);
    }

    if (! apvts.state.getChildWithName ("NonParam").isValid())
        apvts.state.appendChild (juce::ValueTree ("NonParam"), nullptr);

    jassert (bypassParameter != nullptr);
}

const juce::String PluginProcessor::getName() const { return JucePlugin_Name; }

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
    engine->setSettings (readEngineSettings());
    engine->prepare (sampleRate, maximumExpectedSamplesPerBlock);
    setLatencySamples (engine->getLatencySamples());
}

void PluginProcessor::releaseResources() {}

dsp::EngineSettings PluginProcessor::readEngineSettings() const noexcept
{
    dsp::EngineSettings s;
    s.usedMask = bandSlots.usedMaskSnapshot();
    s.bypassAll = globalBypass->load (std::memory_order_relaxed) >= 0.5f;

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
        b.bypass = get (r.bypass) >= 0.5f;
    }
    return s;
}

void PluginProcessor::processMain (juce::AudioBuffer<float>& buffer, bool hostBypassed) noexcept
{
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

    engine->setSettings (readEngineSettings());

    if (hostBypassed) engine->processNeutral (buffer.getArrayOfWritePointers(), mainChannels, numSamples);
    else              engine->process (buffer.getArrayOfWritePointers(), mainChannels, numSamples);

    for (int ch = 0; ch < mainOut && ch < buffer.getNumChannels(); ++ch)
        finiteGuard.process (buffer.getWritePointer (ch), numSamples);   // output guard
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
    return new juce::GenericAudioProcessorEditor (*this);
}

void PluginProcessor::getStateInformation (juce::MemoryBlock& dest)
{
    dest = state::save (apvts);
}

void PluginProcessor::setStateInformation (const void* data, int size)
{
    if (size <= 0) return;

    if (state::load (apvts, data, (size_t) size).ok)
    {
        bandSlots.refresh();
        undoManager.clearUndoHistory();
    }
}
} // namespace tl

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new tl::PluginProcessor();
}
