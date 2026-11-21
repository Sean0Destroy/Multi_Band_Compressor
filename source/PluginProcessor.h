#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <memory>

#include "BandSlots.h"
#include "ParameterSchema.h"
#include "PresetManager.h"
#include "dsp/IBandEngine.h"
#include "rt/AnalyzerFifo.h"
#include "rt/FiniteGuard.h"
#include "rt/LevelMeter.h"

namespace tl
{
/** Orchestrates parameters, topology, state and buses; all band DSP lives in a replaceable
    dsp::IBandEngine (P1-M2: the experimental A3 region engine). Audio path per block:
    input guard -> engine (main bus only, in place) -> output guard. Latency 0. */
class PluginProcessor final : public juce::AudioProcessor,
                              private juce::AudioProcessorValueTreeState::Listener,
                              private juce::AsyncUpdater
{
public:
    PluginProcessor();
    ~PluginProcessor() override;

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
    /** Presets and A/B (message thread). Lives in the processor so that A/B survives closing the editor. */
    PresetManager& getPresets() noexcept { return presets; }
    juce::UndoManager& getUndoManager() noexcept { return undoManager; }
    /** Replaces non-finite samples (input before the engine, output after it); triggered by either. */
    rt::FiniteGuard& getFiniteGuard() noexcept { return finiteGuard; }
    dsp::IBandEngine& getEngine() noexcept { return *engine; }
    const dsp::GrMeter& getGainReductionMeter() const noexcept { return engine->getMeter(); }

    /** Audio-thread safe: snapshot of the band parameters + topology as the engine sees them. */
    dsp::EngineSettings readEngineSettings() const noexcept;

    // ---- Spectrum analyzer feed (editor) -----------------------------------------------------
    /** Message thread: an editor asks for / releases the analyzer feed (counted). While nobody
        asks, the audio thread does not copy anything. */
    void addAnalyzerClient() noexcept    { analyzerClients.fetch_add (1, std::memory_order_relaxed); }
    void removeAnalyzerClient() noexcept { analyzerClients.fetch_sub (1, std::memory_order_relaxed); }
    bool isAnalyzerActive() const noexcept { return analyzerClients.load (std::memory_order_relaxed) > 0; }
    /** Peak / RMS of the plugin input, output and external sidechain (updated only while an
        editor asks for the analyzer feed). One reader at a time. */
    const rt::LevelMeter& getInputMeter() const noexcept     { return inputMeter; }
    const rt::LevelMeter& getOutputMeter() const noexcept    { return outputMeter; }
    const rt::LevelMeter& getSidechainMeter() const noexcept { return sidechainMeter; }

    /** Mono mix of the plugin input / output. One reader at a time (the open editor). */
    rt::AnalyzerFifo& getInputAnalyzerFifo() noexcept  { return analyzerIn; }
    rt::AnalyzerFifo& getOutputAnalyzerFifo() noexcept { return analyzerOut; }

    /** Increases with every processed block (wraps). An editor uses it to tell "no gain
        reduction" from "the host stopped calling us" (meters would otherwise keep their last value). */
    std::uint32_t getProcessedBlockCount() const noexcept { return processedBlocks.load (std::memory_order_relaxed); }

    /** Message thread: recomputes the latency (engine + lookahead) from the current parameters
        and topology and reports it to the host if it changed. Called automatically when a
        lookahead, the constant-latency switch or the band set changes. */
    void refreshLatency();

    static constexpr int sidechainBusIndex = 1;

    /** Message thread: channels of the external sidechain bus as the host configured it (0 = off). */
    int getSidechainChannelCount() const
    {
        return getBusCount (true) > sidechainBusIndex ? getChannelCountOfBus (true, sidechainBusIndex) : 0;
    }

private:
    void processMain (juce::AudioBuffer<float>&, bool hostBypassed) noexcept;
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void handleAsyncUpdate() override { refreshLatency(); }

    struct BandParamRefs
    {
        std::atomic<float>* fLo = nullptr;     std::atomic<float>* fHi = nullptr;
        std::atomic<float>* thresh = nullptr;  std::atomic<float>* ratio = nullptr;
        std::atomic<float>* knee = nullptr;    std::atomic<float>* attack = nullptr;
        std::atomic<float>* release = nullptr; std::atomic<float>* range = nullptr;
        std::atomic<float>* detMode = nullptr; std::atomic<float>* detBlend = nullptr;
        std::atomic<float>* rmsTime = nullptr; std::atomic<float>* link = nullptr;
        std::atomic<float>* makeup = nullptr;  std::atomic<float>* mix = nullptr;
        std::atomic<float>* scSource = nullptr; std::atomic<float>* scMode = nullptr;
        std::atomic<float>* scRef = nullptr;    std::atomic<float>* scLo = nullptr;
        std::atomic<float>* scHi = nullptr;     std::atomic<float>* scChan = nullptr;
        std::atomic<float>* stereoMode = nullptr;
        std::atomic<float>* lookahead = nullptr;
        std::atomic<float>* adaptRel = nullptr; std::atomic<float>* freqRel = nullptr;
        std::atomic<float>* transient = nullptr;
        std::atomic<float>* bypass = nullptr;
    };

    // The APVTS gets no UndoManager: band create/delete are explicit UndoableActions on
    // `undoManager`; knob-edit undo is out of scope for this milestone.
    juce::UndoManager undoManager;
    juce::AudioProcessorValueTreeState apvts;
    BandSlots bandSlots;
    PresetManager presets { *this };
    juce::AudioProcessorParameter* bypassParameter = nullptr;
    rt::FiniteGuard finiteGuard;
    std::unique_ptr<dsp::IBandEngine> engine;
    std::array<BandParamRefs, schema::kActiveSlots> bandParams {};
    std::atomic<std::uint32_t> processedBlocks { 0 };
    std::atomic<int> analyzerClients { 0 };
    rt::AnalyzerFifo analyzerIn, analyzerOut;
    rt::LevelMeter inputMeter, outputMeter, sidechainMeter;
    std::atomic<float>* globalBypass = nullptr;
    std::atomic<float>* inputGain = nullptr;
    std::atomic<float>* outputGain = nullptr;
    std::atomic<float>* globalMix = nullptr;
    std::atomic<float>* autoGain = nullptr;
    std::atomic<float>* clipMode = nullptr;
    std::atomic<float>* clipCeiling = nullptr;
    std::atomic<float>* constantLatency = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
} // namespace tl
