#include "PluginProcessor.h"

#include "PluginIdentity.h"
#include "StateSerialization.h"

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
    bypassParameter = apvts.getParameter (schema::global::bypass);

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

void PluginProcessor::prepareToPlay (double, int)
{
    setLatencySamples (0);   // P1-M1: no engine, no latency. Allocation for the engine will go here.
}

void PluginProcessor::releaseResources() {}

void PluginProcessor::processPassThrough (juce::AudioBuffer<float>& buffer) noexcept
{
    const int numSamples = buffer.getNumSamples();
    const int mainIn  = getMainBusNumInputChannels();
    const int mainOut = getMainBusNumOutputChannels();

    // The host buffer is in-place: channels [0, mainIn) carry the main input and become the
    // main output untouched. Any sidechain channels live after them and are input-only.
    // Output channels with no matching main input must be silent; sidechain data never reaches them.
    for (int ch = mainIn; ch < mainOut && ch < buffer.getNumChannels(); ++ch)
        juce::FloatVectorOperations::clear (buffer.getWritePointer (ch), numSamples);

    for (int ch = 0; ch < mainOut && ch < buffer.getNumChannels(); ++ch)
        finiteGuard.process (buffer.getWritePointer (ch), numSamples);
}

void PluginProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    processPassThrough (buffer);
}

void PluginProcessor::processBlockBypassed (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;
    processPassThrough (buffer);
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
