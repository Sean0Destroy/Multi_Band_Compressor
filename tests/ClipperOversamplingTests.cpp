// The output clipper's own oversampling (clipOversampling): latency, ceiling, transparency below
// the ceiling, less aliasing, and click-free changes (clipper on/off, factor change, bypass).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>

#include "dsp/Lookahead.h"
#include "dsp/OutputStage.h"
#include "dsp/Ramp.h"
#include "support/AllocationCounter.h"
#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kFs = 48000.0;

struct Rig
{
    std::unique_ptr<PluginProcessor> proc = std::make_unique<PluginProcessor>();
    OfflineRenderer renderer { *proc };
    int channels;

    explicit Rig (int numChannels = 2) : channels (numChannels)
    {
        REQUIRE (renderer.prepare (kFs, 512, { channels, 0 }));
    }

    void global (const char* id, float v) { setPlain (*proc, id, v); }
    void restart() { REQUIRE (renderer.prepare (kFs, 512, { channels, 0 })); }

    juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& in, std::vector<int> blocks = { 256 },
                                     bool bypassed = false, const OfflineRenderer::BlockHook& hook = {})
    {
        return renderer.render (in, nullptr, blocks, bypassed, hook);
    }
};

juce::AudioBuffer<float> sineBuffer (double f, int n, double amplitude, int channels = 2)
{
    juce::AudioBuffer<float> b (channels, n);
    const auto s = sine (f, kFs, n, amplitude);
    for (int c = 0; c < channels; ++c) b.copyFrom (c, 0, s.data(), n);
    return b;
}

float peakOf (const juce::AudioBuffer<float>& b, int start, int end)
{
    float m = 0.0f;
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = start; i < end; ++i)
            m = std::max (m, std::abs (b.getSample (c, i)));
    return m;
}

float largestStep (const juce::AudioBuffer<float>& b, int start, int end)
{
    float m = 0.0f;
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = std::max (1, start); i < end; ++i)
            m = std::max (m, std::abs (b.getSample (c, i) - b.getSample (c, i - 1)));
    return m;
}

/** Amplitude of the component at `f` (an exact bin of the window [start, start + length)). */
double amplitudeAt (const juce::AudioBuffer<float>& b, int channel, int start, int length, double f)
{
    double re = 0.0, im = 0.0;
    for (int i = 0; i < length; ++i)
    {
        const double phase = 2.0 * dsp::kPi * f * (double) i / kFs;
        re += (double) b.getSample (channel, start + i) * std::cos (phase);
        im -= (double) b.getSample (channel, start + i) * std::sin (phase);
    }
    return 2.0 * std::sqrt (re * re + im * im) / (double) length;
}
}

TEST_CASE ("Clip oversampling: its own latency, clipper on or off, added to the colour's", "[dsp][clip][oversampling]")
{
    const int n = 9600;
    const auto in = makeNoise (2, n, 5u, 0.25f);

    for (int stages = 1; stages <= 3; ++stages)
    {
        INFO ("clip oversampling stages " << stages);
        Rig rig;
        rig.global ("clipOversampling", (float) stages);
        rig.restart();
        const int latency = rig.proc->getLatencySamples();
        CHECK (latency == dsp::oversamplingLatency (stages));

        // Clipper off: the output is the input, delayed by exactly the reported latency.
        const auto out = rig.render (in, { 64, 300, 1 });
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < n; ++i)
                REQUIRE (out.getSample (ch, i) == (i >= latency ? in.getSample (ch, i - latency) : 0.0f));

        // Switching the clipper on does not change the latency.
        rig.global ("clipMode", 2.0f);
        CHECK (rig.proc->getLatencySamples() == latency);

        // The colour's oversampling adds its own.
        rig.global ("oversampling", 2.0f);
        CHECK (rig.proc->getLatencySamples() == latency + dsp::oversamplingLatency (2));
    }

    Rig rig;
    CHECK (rig.proc->getLatencySamples() == 0);       // off by default
}

TEST_CASE ("Clip oversampling: the ceiling holds exactly, and the signal below it passes in time", "[dsp][clip][oversampling]")
{
    const int n = 24000;
    const float ceiling = dsp::dbToLinear (-6.0f);

    for (const float mode : { 1.0f, 2.0f })
        for (int stages = 1; stages <= 3; ++stages)
        {
            INFO ((mode == 2.0f ? "hard" : "soft") << ", stages " << stages);
            Rig rig;
            rig.global ("clipMode", mode);
            rig.global ("clipCeiling", -6.0f);
            rig.global ("clipOversampling", (float) stages);
            rig.restart();
            const int latency = rig.proc->getLatencySamples();

            // Loud and bright: every sample stays at or below the ceiling, and the clipper works.
            const auto loud = rig.render (sineBuffer (7100.0, n, 1.5), { 64, 300, 1 });
            CHECK (peakOf (loud, 0, n) <= ceiling);
            CHECK (peakOf (loud, 4800, n) > 0.9f * ceiling);

            // Quiet (below half the ceiling): the clip curves are linear there, so what comes out
            // is the input, delayed by the latency, through the oversampling filters only.
            rig.restart();
            const auto quietIn = sineBuffer (1000.0, n, 0.2);
            const auto quiet = rig.render (quietIn);
            double worst = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 4800; i < n; ++i)
                    worst = std::max (worst, std::abs ((double) quiet.getSample (ch, i) - quietIn.getSample (ch, i - latency)));
            CHECK (worst < 1.0e-3);
        }
}

TEST_CASE ("Clip oversampling: hard clipping a bright tone aliases less", "[dsp][clip][oversampling]")
{
    // A 5 kHz tone clipped hard: its odd harmonics above 24 kHz fold back to 3, 7 and 13 kHz
    // (45, 55 and 35 kHz). At 48 kHz those are pure aliasing. (The fold of 25 kHz to 23 kHz is
    // not measured: 25 kHz is in the transition band of the first half-band filter.)
    // The final clamp that keeps the ceiling exact clips the filters' small overshoot at the base
    // rate, which brings some aliasing back; measured: about 10 dB less aliasing with the clamp,
    // 25 to 60 dB less without it (peaks then up to about 1.1 dB over the ceiling).
    const int n = 36000, start = 12000, length = 24000;     // 2 Hz bins: every component is exact
    const auto in = sineBuffer (5000.0, n, 2.0, 1);

    const auto aliasDb = [&] (int stages)
    {
        Rig rig (1);
        rig.global ("clipMode", 2.0f);
        rig.global ("clipCeiling", -6.0f);
        rig.global ("clipOversampling", (float) stages);
        rig.restart();
        const auto out = rig.render (in);
        double energy = 0.0;
        for (double f : { 3000.0, 7000.0, 13000.0 })
            energy += std::pow (amplitudeAt (out, 0, start, length, f), 2.0);
        const double fundamental = amplitudeAt (out, 0, start, length, 5000.0);
        return 10.0 * std::log10 (energy / (fundamental * fundamental) + 1.0e-30);
    };

    const double base = aliasDb (0), x2 = aliasDb (1), x4 = aliasDb (2), x8 = aliasDb (3);
    INFO ("aliasing relative to the fundamental: base " << base << " dB, 2x " << x2 << ", 4x " << x4 << ", 8x " << x8);
    CHECK (base > -25.0);            // the problem is real at the base rate
    CHECK (x2 < base - 8.0);
    CHECK (x4 < base - 8.0);
    CHECK (x8 < base - 8.0);
}

TEST_CASE ("Clip oversampling: switching the clipper, the factor and bypass is click-free", "[dsp][clip][oversampling][transition]")
{
    const int n = 48000, block = 128;
    const auto in = sineBuffer (220.0, n, 1.0);
    const float ceiling = dsp::dbToLinear (-6.0f);
    const float sineStep = (float) (2.0 * dsp::kPi * 220.0 / kFs);    // largest step of the unclipped tone

    SECTION ("the clipper on and off: it fades over 20 ms and the latency stays")
    {
        Rig rig;
        rig.global ("clipCeiling", -6.0f);
        rig.global ("clipOversampling", 2.0f);
        rig.restart();
        const int latency = rig.proc->getLatencySamples();
        bool latencyChanged = false;
        const auto out = rig.render (in, { block }, false, [&] (int b, PluginProcessor& p)
        {
            if (b == 60)  setPlain (p, "clipMode", 2.0f);
            if (b == 200) setPlain (p, "clipMode", 0.0f);
            latencyChanged = latencyChanged || p.getLatencySamples() != latency;
        });
        CHECK_FALSE (latencyChanged);
        CHECK (largestStep (out, latency + 1, n) < 1.05f * sineStep);
        CHECK (peakOf (out, 120 * block, 200 * block) <= ceiling);                // on and settled
        for (int i = 260 * block; i < n; ++i)                                     // off again: the delayed input
            REQUIRE (out.getSample (0, i) == in.getSample (0, i - latency));
    }

    SECTION ("a factor change while clipping: no click, and the ceiling holds throughout")
    {
        Rig rig;
        rig.global ("clipMode", 2.0f);
        rig.global ("clipCeiling", -6.0f);
        rig.global ("clipOversampling", 1.0f);
        rig.restart();
        const auto out = rig.render (in, { block }, false, [] (int b, PluginProcessor& p)
        {
            if (b == 60)  setPlain (p, "clipOversampling", 3.0f);
            if (b == 160) setPlain (p, "clipOversampling", 0.0f);
            if (b == 260) setPlain (p, "clipOversampling", 2.0f);
        });
        CHECK (peakOf (out, 0, n) <= ceiling);
        // (The latency crossfade of a factor change blends two copies of the tone 8 to 55 samples
        // apart, which steepens it by a few per cent for 10 ms: not a click.)
        CHECK (largestStep (out, 100, n) < 1.1f * sineStep);
        CHECK (peakOf (out, 340 * block, n) > 0.99f * ceiling);                   // still clipping at the end
    }

    SECTION ("host bypass fades the clipper out and keeps the delay")
    {
        Rig rig;
        rig.global ("clipMode", 2.0f);
        rig.global ("clipCeiling", -6.0f);
        rig.global ("clipOversampling", 3.0f);
        rig.restart();
        const int latency = rig.proc->getLatencySamples();
        REQUIRE (latency == 55);
        const auto out = rig.render (in, { 256 }, true);
        for (int i = 960 + 256 + latency; i < n; ++i)
            REQUIRE (out.getSample (0, i) == in.getSample (0, i - latency));
    }
}

TEST_CASE ("Clip oversampling: no audio-thread allocations", "[allocation][clip][oversampling]")
{
    PluginProcessor p;
    OfflineRenderer r (p);
    REQUIRE (r.prepare (44100.0, 1024, { 2, 0 }));
    setPlain (p, "clipMode", 2.0f);
    setPlain (p, "clipOversampling", 3.0f);
    const auto in = makeNoise (2, 6000, 3u, 1.5f);

    auto automate = [] (int b, PluginProcessor& proc)
    {
        auto& s = proc.getState();
        s.getParameter ("clipMode")->setValueNotifyingHost ((b % 3) / 2.0f);
        s.getParameter ("clipCeiling")->setValueNotifyingHost ((b % 4) / 4.0f);
        if (b % 5 == 0) s.getParameter ("clipOversampling")->setValueNotifyingHost ((b % 4) / 3.0f);
    };

    resetAllocationCount();
    for (auto& seq : std::vector<std::vector<int>> { { 1 }, { 64 }, { 1, 3, 1024, 17, 255 } })
    {
        r.render (in, nullptr, seq, false, automate);
        r.render (in, nullptr, seq, true, automate);
    }
    CHECK (allocationCount() == 0);
}
