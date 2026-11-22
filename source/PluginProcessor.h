#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <memory>

#include "BandSlots.h"
#include "ParameterSchema.h"
#include "PresetManager.h"
#include "StateGuard.h"
#include "dsp/A3Model.h"
#include "dsp/IBandEngine.h"
#include "rt/AnalyzerFifo.h"
#include "rt/FiniteGuard.h"
#include "rt/LevelMeter.h"
#include "rt/LoudnessMeter.h"

namespace tl
{
/** Orchestrates parameters, topology, state and buses; all band DSP lives in a replaceable
    dsp::IBandEngine (two experimental engines, see setEngineKind). Audio path per block:
    input guard -> engine (main bus only, in place) -> output guard. Latency: the lookahead
    plus the oversampling filters (see refreshLatency), 0 by default. */
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
    /** The mutex for state that is not a parameter value, and the rule for telling the host
        about changes made under it (never while it is held). See StateGuard. */
    StateGuard& getStateGuard() noexcept { return stateGuard; }
    /** Presets and A/B (message thread). Lives in the processor so that A/B survives closing the editor. */
    PresetManager& getPresets() noexcept { return presets; }
    juce::UndoManager& getUndoManager() noexcept { return undoManager; }
    /** Replaces non-finite samples (input before the engine, output after it); triggered by either. */
    rt::FiniteGuard& getFiniteGuard() noexcept { return finiteGuard; }
    /** The band engine currently selected (see setEngineKind). */
    dsp::IBandEngine& getEngine() noexcept { return *engines[(size_t) selectedEngine.load (std::memory_order_relaxed)]; }
    const dsp::GrMeter& getGainReductionMeter() const noexcept { return engines[(size_t) selectedEngine.load (std::memory_order_relaxed)]->getMeter(); }

    /** Which of the experimental band engines runs. A listening switch, not a parameter: it is
        saved with the project (NonParam) but a preset or an A/B switch does not change it.
        Switching while audio runs first lets the new engine run unheard for 100 ms (so that its
        delays, ramps and detectors are in place) and then crossfades the two over 20 ms.
        Message thread. */
    void setEngineKind (dsp::EngineKind);
    dsp::EngineKind getEngineKind() const noexcept { return (dsp::EngineKind) selectedEngine.load (std::memory_order_relaxed); }

    /** Message thread: like setStateInformation, but keeps what belongs to the session rather
        than to a preset (editor window size, selected engine). Used for presets and A/B. */
    void setStateKeepingSession (const void* data, int sizeInBytes);

    /** Audio-thread safe: snapshot of the band parameters + topology as the engine sees them. */
    dsp::EngineSettings readEngineSettings() const noexcept;

    // ---- Spectrum analyzer feed (editor) -----------------------------------------------------
    /** Message thread: an editor asks for / releases the analyzer feed (counted). While nobody
        asks, the audio thread does not copy anything. */
    void addAnalyzerClient() noexcept
    {
        // Nothing was measured while no editor was open: the first one starts the meters afresh.
        if (analyzerClients.fetch_add (1, std::memory_order_relaxed) == 0)
            metersStale.store (true, std::memory_order_relaxed);
    }
    void removeAnalyzerClient() noexcept { analyzerClients.fetch_sub (1, std::memory_order_relaxed); }
    bool isAnalyzerActive() const noexcept { return analyzerClients.load (std::memory_order_relaxed) > 0; }
    /** Peak / RMS of the plugin input, output and external sidechain (updated only while an
        editor asks for the analyzer feed). One reader at a time. */
    const rt::LevelMeter& getInputMeter() const noexcept     { return inputMeter; }
    const rt::LevelMeter& getOutputMeter() const noexcept    { return outputMeter; }
    const rt::LevelMeter& getSidechainMeter() const noexcept { return sidechainMeter; }

    /** Loudness (LUFS) and true peak of the output; measured only while an editor is open, so
        "integrated" covers the time the editor was open since the last reset. */
    rt::LoudnessMeter& getLoudnessMeter() noexcept { return loudness; }

    /** Mono mix of the plugin input / output. One reader at a time (the open editor). */
    rt::AnalyzerFifo& getInputAnalyzerFifo() noexcept  { return analyzerIn; }
    rt::AnalyzerFifo& getOutputAnalyzerFifo() noexcept { return analyzerOut; }

    /** Increases with every processed block (wraps). An editor uses it to tell "no gain
        reduction" from "the host stopped calling us" (meters would otherwise keep their last value). */
    std::uint32_t getProcessedBlockCount() const noexcept { return processedBlocks.load (std::memory_order_relaxed); }

    /** Audition (solo a band's component / listen to its detector). Not saved; cleared by a state
        load, by removing that band and when the editor closes. One at a time. */
    void setAudition (dsp::AuditionMode mode, int slot) noexcept { for (auto& e : engines) e->setAudition (mode, slot); }
    dsp::AuditionMode getAuditionMode() const noexcept          { return engines[0]->getAuditionMode(); }
    int getAuditionSlot() const noexcept                        { return engines[0]->getAuditionSlot(); }

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
    void loadState (const void* data, int sizeInBytes, bool adoptEngine);
    void parameterChanged (const juce::String& parameterID, float newValue) override;
    void handleAsyncUpdate() override { refreshLatency(); }
    /** Brings the reported latency up to date from wherever it is called: at once on the
        message thread, through the message queue otherwise. Never with the state mutex held. */
    void requestLatencyRefresh();

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
    StateGuard stateGuard { *this };             // after apvts: one pending flag per parameter
    BandSlots bandSlots;
    PresetManager presets { *this };
    juce::AudioProcessorParameter* bypassParameter = nullptr;
    rt::FiniteGuard finiteGuard;
    std::array<std::unique_ptr<dsp::IBandEngine>, dsp::kNumEngineKinds> engines;
    std::atomic<int> selectedEngine { 0 };       // what the user chose
    int activeEngine = 0;                        // audio thread: the engine that produces the output
    int incomingEngine = -1;                     // audio thread: the engine being started (-1 = none)
    int enginePrerollRemaining = 0, enginePrerollLength = 4800;   // it first runs unheard for 100 ms,
    int engineFadeRemaining = 0, engineFadeLength = 960;          // then the two are crossfaded for 20 ms
    double preparedSampleRate = 48000.0;
    int preparedBlockSize = 512;
    juce::AudioBuffer<float> engineFadeScratch;  // allocated in prepareToPlay
    std::array<BandParamRefs, schema::kActiveSlots> bandParams {};
    std::atomic<std::uint32_t> processedBlocks { 0 };
    std::atomic<int> analyzerClients { 0 };
    std::atomic<bool> metersStale { false };
    rt::AnalyzerFifo analyzerIn, analyzerOut;
    rt::LevelMeter inputMeter, outputMeter, sidechainMeter;
    rt::LoudnessMeter loudness;
    std::atomic<float>* globalBypass = nullptr;
    std::atomic<float>* inputGain = nullptr;
    std::atomic<float>* outputGain = nullptr;
    std::atomic<float>* globalMix = nullptr;
    std::atomic<float>* autoGain = nullptr;
    std::atomic<float>* clipMode = nullptr;
    std::atomic<float>* clipCeiling = nullptr;
    std::atomic<float>* constantLatency = nullptr;
    std::atomic<float>* charModel = nullptr;
    std::atomic<float>* charDrive = nullptr;
    std::atomic<float>* oversampling = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PluginProcessor)
};
} // namespace tl
