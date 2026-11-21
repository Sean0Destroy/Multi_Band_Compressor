#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "BandSlots.h"
#include "ParameterSchema.h"
#include "rt/FiniteGuard.h"

namespace tl
{
/** P1-M1 processor: parameter/state/bus infrastructure only. The audio path is a pure
    pass-through of the main bus followed by an output finite-number guard. */
class PluginProcessor final : public juce::AudioProcessor
{
public:
    PluginProcessor();
    ~PluginProcessor() override = default;

    void prepareToPlay (double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout&) const override;

    using AudioProcessor::processBlock;
    using AudioProcessor::processBlockBypassed;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void processBlockBypassed (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    juce::AudioProcessorParameter* getBypassParameter() const override { return bypassParameter; }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override;
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock&) override;
    void setStateInformation (const void*, int) override;

    juce::AudioProcessorValueTreeState& getState() noexcept { return apvts; }
    BandSlots& getBandSlots() noexcept { return bandSlots; }
    juce::UndoManager& getUndoManager() noexcept { return undoManager; }
    rt::FiniteGuard& getFiniteGuard() noexcept { return finiteGuard; }

    static constexpr int sidechainBusIndex = 1;

private:
    void processPassThrough (juce::AudioBuffer<float>&) noexcept;

    // The APVTS gets no UndoManager: band create/delete are explicit UndoableActions on
    // `undoManager`; knob-edit undo is out of scope for this milestone.
    juce::UndoManager undoManager;
    juce::AudioProcessorValueTreeState apvts;
    BandSlots bandSlots;
    juce::AudioProcessorParameter* bypassParameter = nullptr;
    rt::FiniteGuard finiteGuard;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
} // namespace tl
