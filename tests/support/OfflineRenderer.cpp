#include "OfflineRenderer.h"

#include <cstring>

#include "AllocationCounter.h"

namespace tl::test
{
OfflineRenderer::OfflineRenderer (PluginProcessor& p) : proc (p) {}

bool OfflineRenderer::prepare (double sampleRate, int maxBlockSize, Layout l)
{
    const auto main = l.mainChannels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();
    const auto sc = l.sidechainChannels == 0 ? juce::AudioChannelSet::disabled()
                  : l.sidechainChannels == 1 ? juce::AudioChannelSet::mono() : juce::AudioChannelSet::stereo();

    proc.releaseResources();

    juce::AudioProcessor::BusesLayout layoutToSet;
    layoutToSet.inputBuses.add (main);
    layoutToSet.inputBuses.add (sc);
    layoutToSet.outputBuses.add (main);

    if (! proc.setBusesLayout (layoutToSet))
        return false;

    layout = l;
    maxBlock = maxBlockSize;
    scratch.setSize (l.mainChannels + l.sidechainChannels, maxBlockSize);
    midi.ensureSize (256);

    proc.setRateAndBufferSizeDetails (sampleRate, maxBlockSize);
    proc.prepareToPlay (sampleRate, maxBlockSize);
    return true;
}

juce::AudioBuffer<float> OfflineRenderer::render (const juce::AudioBuffer<float>& mainIn,
                                                  const juce::AudioBuffer<float>* scIn,
                                                  const std::vector<int>& blockSizes,
                                                  bool bypassed,
                                                  const BlockHook& hook)
{
    const int total = mainIn.getNumSamples();
    juce::AudioBuffer<float> out (layout.mainChannels, total);
    out.clear();

    int pos = 0, block = 0;
    while (pos < total)
    {
        const int n = juce::jmin (total - pos, juce::jmin (maxBlock, blockSizes[(size_t) block % blockSizes.size()]));

        if (hook) hook (block, proc);

        scratch.clear();
        for (int ch = 0; ch < layout.mainChannels; ++ch)
            scratch.copyFrom (ch, 0, mainIn, ch, pos, n);

        if (scIn != nullptr)
            for (int ch = 0; ch < layout.sidechainChannels; ++ch)
                scratch.copyFrom (layout.mainChannels + ch, 0, *scIn, ch, pos, n);

        {
            juce::AudioBuffer<float> view (scratch.getArrayOfWritePointers(), scratch.getNumChannels(), n);
            AudioCallbackScope scope;

            if (bypassed) proc.processBlockBypassed (view, midi);
            else          proc.processBlock (view, midi);
        }

        for (int ch = 0; ch < layout.mainChannels; ++ch)
            out.copyFrom (ch, pos, scratch, ch, 0, n);

        pos += n;
        ++block;
    }

    return out;
}

juce::AudioBuffer<float> makeNoise (int channels, int numSamples, std::uint32_t seed, float amplitude)
{
    juce::AudioBuffer<float> b (channels, numSamples);
    std::uint32_t s = seed * 2654435761u + 1u;

    for (int ch = 0; ch < channels; ++ch)
        for (int i = 0; i < numSamples; ++i)
        {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            b.setSample (ch, i, amplitude * (float (s) / 2147483648.0f - 1.0f));
        }
    return b;
}

bool bitIdentical (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    if (a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples()) return false;

    for (int ch = 0; ch < a.getNumChannels(); ++ch)
        if (std::memcmp (a.getReadPointer (ch), b.getReadPointer (ch), sizeof (float) * (size_t) a.getNumSamples()) != 0)
            return false;
    return true;
}
} // namespace tl::test
