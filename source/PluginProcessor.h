#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <memory>

#include "BandSlots.h"
#include "ParameterSchema.h"
#include "dsp/IBandEngine.h"
#include "rt/FiniteGuard.h"

namespace tl
{
/** Orchestrates parameters, topology, state and buses; all band DSP lives in a replaceable
    dsp::IBandEngine (P1-M2: the experimental A3 region engine). Audio path per block:
    input guard -> engine (main bus only, in place) -> output guard. Latency 0. */
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
    /** Replaces non-finite samples (input before the engine, output after it); triggered by either. */
    rt::FiniteGuard& getFiniteGuard() noexcept { return finiteGuard; }
    dsp::IBandEngine& getEngine() noexcept { return *engine; }
    const dsp::GrMeter& getGainReductionMeter() const noexcept { return engine->getMeter(); }

    /** Audio-thread safe: snapshot of the band parameters + topology as the engine sees them. */
    dsp::EngineSettings readEngineSettings() const noexcept;

    static constexpr int sidechainBusIndex = 1;

private:
    void processMain (juce::AudioBuffer<float>&, bool hostBypassed) noexcept;

    struct BandParamRefs
    {
        std::atomic<float>* fLo = nullptr;     std::atomic<float>* fHi = nullptr;
        std::atomic<float>* thresh = nullptr;  std::atomic<float>* ratio = nullptr;
        std::atomic<float>* knee = nullptr;    std::atomic<float>* attack = nullptr;
        std::atomic<float>* release = nullptr; std::atomic<float>* range = nullptr;
        std::atomic<float>* detMode = nullptr; std::atomic<float>* detBlend = nullptr;
        std::atomic<float>* rmsTime = nullptr; std::atomic<float>* link = nullptr;
        std::atomic<float>* makeup = nullptr;  std::atomic<float>* mix = nullptr;
        std::atomic<float>* bypass = nullptr;
    };

    // The APVTS gets no UndoManager: band create/delete are explicit UndoableActions on
    // `undoManager`; knob-edit undo is out of scope for this milestone.
    juce::UndoManager undoManager;
    juce::AudioProcessorValueTreeState apvts;
    BandSlots bandSlots;
    juce::AudioProcessorParameter* bypassParameter = nullptr;
    rt::FiniteGuard finiteGuard;
    std::unique_ptr<dsp::IBandEngine> engine;
    std::array<BandParamRefs, schema::kActiveSlots> bandParams {};
    std::atomic<float>* globalBypass = nullptr;
    std::atomic<float>* inputGain = nullptr;
    std::atomic<float>* outputGain = nullptr;
    std::atomic<float>* globalMix = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
} // namespace tl
