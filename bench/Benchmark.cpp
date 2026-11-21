// CPU benchmark (NOT a test; not registered with ctest).
// Reports ns per sample frame and % of real time for the processor and the bare engine.
// Numbers from shared / virtualised machines are indicative only.
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "PluginProcessor.h"
#include "dsp/A3RegionEngine.h"

namespace
{
struct Result { double nsPerFrame, percentRealtime; };

void fillNoise (juce::AudioBuffer<float>& b, float amplitude, std::uint32_t seed)
{
    std::uint32_t s = seed * 2654435761u + 1u;
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
        {
            s ^= s << 13; s ^= s >> 17; s ^= s << 5;
            b.setSample (c, i, amplitude * ((float) s / 2147483648.0f - 1.0f));
        }
}

void setBand (tl::PluginProcessor& p, int slot, const char* suffix, float plain)
{
    auto* prm = p.getState().getParameter (tl::schema::bandParamId (slot, suffix));
    prm->setValueNotifyingHost (prm->convertTo0to1 (plain));
}

/** Median of `repeats` runs over `seconds` of audio, processed in place block by block. */
template <typename ProcessFn>
Result measure (ProcessFn&& process, double fs, int block, int channels, double seconds, int repeats)
{
    const int total = (int) (seconds * fs);
    juce::AudioBuffer<float> source (channels, total);
    fillNoise (source, 0.5f, 7u);
    juce::AudioBuffer<float> work (channels, block);

    std::vector<double> runs;
    volatile float sink = 0.0f;
    for (int r = 0; r < repeats; ++r)
    {
        const auto t0 = std::chrono::steady_clock::now();
        for (int pos = 0; pos + block <= total; pos += block)
        {
            for (int c = 0; c < channels; ++c) work.copyFrom (c, 0, source, c, pos, block);
            process (work);
            sink = sink + work.getSample (0, block - 1);
        }
        const auto t1 = std::chrono::steady_clock::now();
        runs.push_back (std::chrono::duration<double> (t1 - t0).count());
    }
    std::sort (runs.begin(), runs.end());
    const double t = runs[runs.size() / 2];
    const double frames = (double) (total / block * block);
    return { t / frames * 1e9, t / (frames / fs) * 100.0 };
}

void report (const char* name, Result r)
{
    std::printf ("%-58s %9.1f ns/frame  %6.2f %% realtime\n", name, r.nsPerFrame, r.percentRealtime);
}
} // namespace

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit;
    const double fs = 48000.0;
    const int block = 128;
    const double seconds = argc > 1 ? std::atof (argv[1]) : 10.0;
    const int repeats = 5;

    std::printf ("Tone Legion Multiband benchmark: fs %.0f Hz, block %d, stereo, %.1f s audio x %d (median)\n", fs, block, seconds, repeats);
    std::printf ("Indicative only on shared or virtualised machines.\n\n");

    struct Scenario { const char* name; int bands; float thresh; int detMode; bool linkHalf; };
    for (auto sc : { Scenario { "processor, 8 bands, compressing (peak)", 8, -30.0f, 0, false },
                     Scenario { "processor, 8 bands, compressing (blend, link 50%)", 8, -30.0f, 2, true },
                     Scenario { "processor, 8 bands, below threshold", 8, 12.0f, 0, false },
                     Scenario { "processor, 1 band, compressing (peak)", 1, -30.0f, 0, false },
                     Scenario { "processor, 0 bands (pass-through)", 0, 0.0f, 0, false } })
    {
        tl::PluginProcessor p;
        for (int i = 0; i < sc.bands; ++i) p.getBandSlots().createBand();
        for (int s = 1; s <= sc.bands; ++s)
        {
            setBand (p, s, "thresh", sc.thresh);
            setBand (p, s, "detMode", (float) sc.detMode);
            if (sc.linkHalf) setBand (p, s, "link", 50.0f);
        }
        p.setRateAndBufferSizeDetails (fs, block);
        p.prepareToPlay (fs, block);
        juce::MidiBuffer midi;
        report (sc.name, measure ([&] (juce::AudioBuffer<float>& b) { p.processBlock (b, midi); }, fs, block, 2, seconds, repeats));
    }

    {
        tl::dsp::A3RegionEngine e;
        e.prepare (fs, block);
        tl::dsp::EngineSettings s;
        for (int i = 0; i < tl::dsp::kMaxBands; ++i)
        {
            s.usedMask |= 1u << i;
            s.bands[(size_t) i].fLoHz = tl::schema::slotDefaultLo (i + 1);
            s.bands[(size_t) i].fHiHz = tl::schema::slotDefaultHi (i + 1);
            s.bands[(size_t) i].thresholdDb = -30.0f;
        }
        report ("engine only, 8 bands, compressing (peak)", measure ([&] (juce::AudioBuffer<float>& b)
        {
            e.setSettings (s);
            e.process (b.getArrayOfWritePointers(), 2, b.getNumSamples());
        }, fs, block, 2, seconds, repeats));
    }
    return 0;
}
