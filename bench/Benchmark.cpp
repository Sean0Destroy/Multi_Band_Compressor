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
#include "dsp/LinearPhase.h"

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

void setGlobal (tl::PluginProcessor& p, const char* id, float plain)
{
    auto* prm = p.getState().getParameter (id);
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

const char* nameFilter = nullptr;   // second argument: only the scenarios whose name contains it

bool wanted (const char* name)
{
    return nameFilter == nullptr || juce::String (name).contains (nameFilter);
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
    if (argc > 2) nameFilter = argv[2];

    std::printf ("Tone Legion Multiband benchmark: fs %.0f Hz, block %d, stereo, %.1f s audio x %d (median)\n", fs, block, seconds, repeats);
    std::printf ("Indicative only on shared or virtualised machines.\n\n");

    struct Scenario { const char* name; int bands; float thresh; int detMode; bool linkHalf; bool overlapBlend = false; float character = 0.0f;
                      int lpLength = -1; int stereoMode = 0; };
    for (auto sc : { Scenario { "processor, 8 bands, compressing (peak)", 8, -30.0f, 0, false },
                     Scenario { "processor, 8 bands, compressing (peak), overlap Blend", 8, -30.0f, 0, false, true },
                     Scenario { "processor, 8 bands, compressing (peak), character 50%", 8, -30.0f, 0, false, false, 50.0f },
                     Scenario { "processor, 8 bands, compressing (blend, link 50%)", 8, -30.0f, 2, true },
                     Scenario { "processor, 8 bands, below threshold", 8, 12.0f, 0, false },
                     Scenario { "processor, 1 band, compressing (peak)", 1, -30.0f, 0, false },
                     Scenario { "linear phase Short, 8 bands, compressing (peak)", 8, -30.0f, 0, false, false, 0.0f, 0 },
                     Scenario { "linear phase Medium, 8 bands, compressing (peak)", 8, -30.0f, 0, false, false, 0.0f, 1 },
                     Scenario { "linear phase Long, 8 bands, compressing (peak)", 8, -30.0f, 0, false, false, 0.0f, 2 },
                     Scenario { "linear phase Medium, 8 bands, compressing, Left/Right", 8, -30.0f, 0, false, false, 0.0f, 1, 1 },
                     Scenario { "linear phase Medium, 8 bands, compressing, Mid/Side", 8, -30.0f, 0, false, false, 0.0f, 1, 4 },
                     Scenario { "linear phase Medium, 8 bands, below threshold", 8, 12.0f, 0, false, false, 0.0f, 1 },
                     Scenario { "processor, 0 bands (pass-through)", 0, 0.0f, 0, false } })
    {
        if (! wanted (sc.name)) continue;
        tl::PluginProcessor p;
        for (int i = 0; i < sc.bands; ++i) p.getBandSlots().createBand();
        for (int s = 1; s <= sc.bands; ++s)
        {
            setBand (p, s, "thresh", sc.thresh);
            setBand (p, s, "detMode", (float) sc.detMode);
            if (sc.linkHalf) setBand (p, s, "link", 50.0f);
            if (sc.character > 0.0f) setBand (p, s, "character", sc.character);
            if (sc.stereoMode != 0) setBand (p, s, "stereoMode", (float) sc.stereoMode);
        }
        if (sc.overlapBlend) setGlobal (p, "overlapMode", 1.0f);
        if (sc.lpLength >= 0)
        {
            setGlobal (p, "quality", 1.0f);
            setGlobal (p, "lpLength", (float) sc.lpLength);
        }
        p.setRateAndBufferSizeDetails (fs, block);
        p.prepareToPlay (fs, block);
        juce::MidiBuffer midi;
        report (sc.name, measure ([&] (juce::AudioBuffer<float>& b) { p.processBlock (b, midi); }, fs, block, 2, seconds, repeats));
    }

    // Oversampling: the colour stage and the output clipper, no bands, at each factor.
    for (int stages = 1; stages <= 3; ++stages)
        for (const bool clipper : { false, true })
        {
            if (! wanted (clipper ? "processor, 0 bands, hard clipper" : "processor, 0 bands, warm FET colour")) continue;
            tl::PluginProcessor p;
            if (clipper)
            {
                setGlobal (p, "clipMode", 2.0f);
                setGlobal (p, "clipCeiling", -6.0f);
                setGlobal (p, "clipOversampling", (float) stages);
            }
            else
            {
                setGlobal (p, "charModel", 1.0f);
                setGlobal (p, "charDrive", 50.0f);
                setGlobal (p, "oversampling", (float) stages);
            }
            p.setRateAndBufferSizeDetails (fs, block);
            p.prepareToPlay (fs, block);
            juce::MidiBuffer midi;
            const auto name = juce::String ("processor, 0 bands, ") + (clipper ? "hard clipper" : "warm FET colour")
                              + " at " + juce::String (1 << stages) + "x";
            report (name.toRawUTF8(), measure ([&] (juce::AudioBuffer<float>& b) { p.processBlock (b, midi); }, fs, block, 2, seconds, repeats));
        }

    if (wanted ("engine only, 8 bands, compressing (peak)"))
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

    // The linear-phase stage alone, stereo, 8 bands whose gains change at every hop (the cost of
    // compressing: a new kernel and two convolutions per hop).
    for (int length = 0; length < 3; ++length)
    {
        const auto name = juce::String ("linear-phase stage only, ") + (length == 0 ? "Short" : length == 1 ? "Medium" : "Long")
                          + ", 8 bands, new gains every hop";
        if (! wanted (name.toRawUTF8())) continue;
        auto st = std::make_unique<tl::dsp::LinearPhaseStage>();
        st->prepare (fs, length, tl::dsp::EngineKind::A3);
        tl::dsp::LinearPhaseSnapshot snap;
        snap.numChannels = 2;
        for (int i = 0; i < tl::dsp::kMaxBands; ++i)
        {
            snap.bands[i].active = true;
            snap.bands[i].lo = (float) std::log2 (tl::schema::slotDefaultLo (i + 1));
            snap.bands[i].hi = (float) std::log2 (tl::schema::slotDefaultHi (i + 1));
        }
        juce::AudioBuffer<float> out (2, block);
        int counter = 0;
        report (name.toRawUTF8(), measure ([&] (juce::AudioBuffer<float>& b)
        {
            if (st->sampleWanted (b.getNumSamples()) >= 0)
            {
                ++counter;
                for (auto& band : snap.bands)
                    band.gain[0] = band.gain[1] = 0.5f + 0.001f * (float) (counter % 200);
                st->pendingSnapshot() = snap;
            }
            const float* in[2] { b.getReadPointer (0), b.getReadPointer (1) };
            float* o[2] { out.getWritePointer (0), out.getWritePointer (1) };
            st->process (in, o, nullptr, 2, b.getNumSamples());
            b.copyFrom (0, 0, out, 0, 0, b.getNumSamples());
        }, fs, block, 2, seconds, repeats));
    }
    return 0;
}
