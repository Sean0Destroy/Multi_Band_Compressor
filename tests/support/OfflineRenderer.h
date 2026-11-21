#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <vector>

#include "PluginProcessor.h"

namespace tl::test
{
struct Layout
{
    int mainChannels = 2;      // 1 or 2
    int sidechainChannels = 0; // 0 = bus disabled
};

/** Drives a PluginProcessor directly, mimicking a host: in-place buffer whose channels are
    [main in/out][sidechain in]. The renderer sets the thread_local "audio callback" flag
    around processBlock only. */
class OfflineRenderer
{
public:
    explicit OfflineRenderer (PluginProcessor&);

    /** setBusesLayout + prepareToPlay. Returns false if the layout was rejected. */
    bool prepare (double sampleRate, int maxBlockSize, Layout);

    /** Called before block `index` (outside the audio-callback scope), may change parameters. */
    using BlockHook = std::function<void (int blockIndex, PluginProcessor&)>;

    /** Renders main (and optional sidechain, same length) through the processor using the cycled
        block-size sequence. Returns the main output. */
    juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& mainIn,
                                     const juce::AudioBuffer<float>* sidechainIn,
                                     const std::vector<int>& blockSizes,
                                     bool bypassed = false,
                                     const BlockHook& hook = {});

private:
    PluginProcessor& proc;
    Layout layout;
    int maxBlock = 0;
    juce::AudioBuffer<float> scratch;
    juce::MidiBuffer midi;
};

juce::AudioBuffer<float> makeNoise (int channels, int numSamples, std::uint32_t seed, float amplitude = 1.0f);
bool bitIdentical (const juce::AudioBuffer<float>&, const juce::AudioBuffer<float>&);
} // namespace tl::test
