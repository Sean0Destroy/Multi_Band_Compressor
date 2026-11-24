// Per-band lookahead: latency reporting, exact delay, gain alignment, changes while running.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>

#include "dsp/BandEngineBase.h"
#include "dsp/Lookahead.h"
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
    double fs;

    explicit Rig (int numChannels = 1, int bands = 1, double sampleRate = kFs) : channels (numChannels), fs (sampleRate)
    {
        for (int i = 0; i < bands; ++i) proc->getBandSlots().createBand();
        REQUIRE (renderer.prepare (fs, 512, { channels, 0 }));
    }

    void band (int slot, const char* suffix, float v) { setBand (*proc, slot, suffix, v); }
    void global (const char* id, float v)             { setPlain (*proc, id, v); }
    void restart() { REQUIRE (renderer.prepare (fs, 512, { channels, 0 })); }

    juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& in, std::vector<int> blocks = { 128 },
                                     bool bypassed = false, const OfflineRenderer::BlockHook& hook = {})
    {
        return renderer.render (in, nullptr, blocks, bypassed, hook);
    }
};

/** True if out[i] == in[i - delay] for every i >= start (and silence before the delay). */
bool isDelayedCopy (const juce::AudioBuffer<float>& out, const juce::AudioBuffer<float>& in, int delay, int start = 0)
{
    for (int c = 0; c < out.getNumChannels(); ++c)
        for (int i = start; i < out.getNumSamples(); ++i)
        {
            const float expected = i >= delay ? in.getSample (c, i - delay) : 0.0f;
            if (out.getSample (c, i) != expected) return false;
        }
    return true;
}

float peakIn (const juce::AudioBuffer<float>& b, int start, int end)
{
    float m = 0.0f;
    for (int i = start; i < end; ++i) m = std::max (m, std::abs (b.getSample (0, i)));
    return m;
}
}

TEST_CASE ("Lookahead helpers: sample counts and the delay line", "[dsp][lookahead]")
{
    CHECK (dsp::lookaheadSamples (0.0, 48000.0) == 0);
    CHECK (dsp::lookaheadSamples (5.0, 48000.0) == 240);
    CHECK (dsp::lookaheadSamples (10.0, 192000.0) == 1920);
    CHECK (dsp::lookaheadSamples (25.0, 48000.0) == 480);                 // clamped to 10 ms
    CHECK (dsp::lookaheadSamples (-1.0, 48000.0) == 0);
    CHECK (dsp::lookaheadSamples (10.0, 384000.0) == dsp::kLookaheadRing - 1);   // limited by the buffer
    CHECK (dsp::lookaheadSamples (std::nan (""), 48000.0) == 0);

    dsp::EngineSettings s;
    CHECK (dsp::lookaheadLatencySamples (s, 48000.0) == 0);
    s.bands[2].lookaheadMs = 4.0f;                                       // slot not in use: ignored
    CHECK (dsp::lookaheadLatencySamples (s, 48000.0) == 0);
    s.usedMask = 0b101;
    s.bands[0].lookaheadMs = 1.0f;
    CHECK (dsp::lookaheadLatencySamples (s, 48000.0) == 192);            // the largest of the used bands
    s.constantLatency = true;
    CHECK (dsp::lookaheadLatencySamples (s, 48000.0) == 480);

    auto line = std::make_unique<dsp::FadingDelayLine>();
    float x[8] { 1, 2, 3, 4, 5, 6, 7, 8 };
    line->process (x, 8, 0, 3, 3, nullptr);
    CHECK (x[0] == 0.0f); CHECK (x[2] == 0.0f); CHECK (x[3] == 1.0f); CHECK (x[7] == 5.0f);
    float y[4] { 9, 10, 11, 12 };
    line->process (y, 4, 8, 3, 3, nullptr);                              // continues across calls
    CHECK (y[0] == 6.0f); CHECK (y[3] == 9.0f);
    float z[4] { 13, 14, 15, 16 };
    const float t[4] { 0.0f, 0.5f, 1.0f, 1.0f };
    line->process (z, 4, 12, 3, 0, t);                                   // crossfade from 3 samples to 0
    CHECK (z[0] == 10.0f); CHECK (z[1] == 0.5f * (11.0f + 14.0f)); CHECK (z[2] == 15.0f); CHECK (z[3] == 16.0f);
}

TEST_CASE ("Lookahead delays the audio exactly and reports it as latency", "[dsp][lookahead]")
{
    const auto in = makeNoise (2, 9600, 3u, 0.5f);

    SECTION ("default: no latency")
    {
        Rig rig (2);
        CHECK (rig.proc->getLatencySamples() == 0);
        rig.band (1, "thresh", 12.0f);
        rig.restart();
        CHECK (bitIdentical (rig.render (in), in));
    }

    SECTION ("a neutral band with 5 ms lookahead is a pure 240-sample delay")
    {
        Rig rig (2);
        rig.band (1, "thresh", 12.0f);
        rig.band (1, "lookahead", 5.0f);
        CHECK (rig.proc->getLatencySamples() == 240);                    // reported as soon as the parameter changes
        rig.restart();
        test::resetAllocationCount();
        const auto out = rig.render (in, { 64, 300, 1, 512 });
        CHECK (test::allocationCount() == 0);
        CHECK (isDelayedCopy (out, in, 240));

        // Host bypass and the plugin bypass keep the same delay (the host compensates for it).
        rig.restart();
        CHECK (isDelayedCopy (rig.render (in, { 128 }, true), in, 240));
        rig.global ("bypass", 1.0f);
        rig.restart();
        CHECK (isDelayedCopy (rig.render (in), in, 240));
        CHECK (rig.proc->getLatencySamples() == 240);
    }

    SECTION ("latency follows the largest lookahead of the used bands, the sample rate and constant latency")
    {
        Rig rig (2, 3);
        rig.band (1, "lookahead", 2.0f);
        rig.band (2, "lookahead", 7.5f);
        CHECK (rig.proc->getLatencySamples() == 360);
        REQUIRE (rig.proc->getBandSlots().deleteBand (2));               // removing the band with the largest value
        CHECK (rig.proc->getLatencySamples() == 96);
        REQUIRE (rig.proc->getBandSlots().undoLast());
        CHECK (rig.proc->getLatencySamples() == 360);

        rig.global ("constantLatency", 1.0f);
        CHECK (rig.proc->getLatencySamples() == 480);
        rig.band (2, "lookahead", 0.0f);
        CHECK (rig.proc->getLatencySamples() == 480);                    // does not move any more
        rig.global ("constantLatency", 0.0f);
        CHECK (rig.proc->getLatencySamples() == 96);

        Rig other (2, 1, 44100.0);
        other.band (1, "lookahead", 5.0f);
        other.restart();
        CHECK (other.proc->getLatencySamples() == 221);                  // 220.5 rounded

        // State restore brings the latency with it.
        juce::MemoryBlock state;
        rig.band (1, "lookahead", 9.0f);
        rig.proc->getStateInformation (state);
        Rig restored (2, 0);
        restored.proc->setStateInformation (state.getData(), (int) state.getSize());
        CHECK (restored.proc->getLatencySamples() == 432);
    }

    SECTION ("global mix and bypass stay aligned with the delayed signal")
    {
        Rig rig (2);
        rig.band (1, "thresh", 12.0f);
        rig.band (1, "lookahead", 3.0f);
        rig.global ("mix", 50.0f);
        rig.restart();
        CHECK (isDelayedCopy (rig.render (in), in, 144));                // dry and wet are the same delayed signal
    }
}

TEST_CASE ("Lookahead lets the gain reduction arrive before the transient", "[dsp][lookahead]")
{
    // Silence, then a loud in-band tone that fades in over 2 ms (a hard edge would be a broadband
    // click, most of which lies outside the band and is rightly left alone). Attack 2 ms.
    const int n = 24000, onset = 12000, fadeIn = 96;
    const double fc = digitalCentre (500.0, 2000.0, kFs);
    juce::AudioBuffer<float> in (1, n);
    in.clear();
    const auto s = sine (fc, kFs, n - onset, 0.8);
    in.copyFrom (0, onset, s.data(), n - onset);
    for (int i = 0; i < fadeIn; ++i)
        in.setSample (0, onset + i, in.getSample (0, onset + i) * (float) (0.5 - 0.5 * std::cos (dsp::kPi * i / fadeIn)));

    const auto run = [&] (float lookaheadMs, int& latency)
    {
        Rig rig;
        rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
        rig.band (1, "thresh", -30.0f); rig.band (1, "ratio", 100.0f); rig.band (1, "knee", 0.0f);
        rig.band (1, "attack", 2.0f); rig.band (1, "release", 200.0f);
        rig.band (1, "lookahead", lookaheadMs);
        rig.restart();
        latency = rig.proc->getLatencySamples();
        return rig.render (in);
    };

    int latency0 = -1, latency5 = -1;
    const auto without = run (0.0f, latency0);
    const auto with = run (5.0f, latency5);
    REQUIRE (latency0 == 0);
    REQUIRE (latency5 == 240);

    // First 6 ms of the burst as it leaves the plugin (compensated for the latency).
    const float firstWithout = peakIn (without, onset, onset + 288);
    const float firstWith = peakIn (with, onset + 240, onset + 240 + 288);
    const float settled = peakIn (with, n - 2400, n);
    INFO ("first peak without " << firstWithout << ", with " << firstWith << ", settled " << settled);
    CHECK (firstWithout > 3.0f * settled);                               // the attack lets the onset through
    CHECK (firstWith < 0.5f * firstWithout);                             // with lookahead it is already turned down

    // Before the burst arrives nothing is audible (the gain moves early, on silence).
    CHECK (peakIn (with, 0, onset + 240) < 1.0e-6f);
    // Once settled both are the same (the lookahead only shifts the gain in time).
    CHECK_THAT ((double) peakIn (without, n - 2400, n), WithinAbs ((double) settled, 0.002));
}

TEST_CASE ("Each band's gain leads the audio by its own lookahead", "[dsp][lookahead]")
{
    // Two bands compressing a noise burst; record what each band's filter applies.
    const int n = 14400;
    const auto in = makeNoise (1, n, 9u, 0.6f);

    const auto traces = [&] (float lookahead1, float lookahead2, int& latency)
    {
        Rig rig (1, 2);
        rig.band (1, "fLo", 100.0f);  rig.band (1, "fHi", 1000.0f);  rig.band (1, "thresh", -40.0f);
        rig.band (2, "fLo", 1000.0f); rig.band (2, "fHi", 8000.0f);  rig.band (2, "thresh", -40.0f);
        rig.band (1, "lookahead", lookahead1);
        rig.band (2, "lookahead", lookahead2);
        rig.restart();
        latency = rig.proc->getLatencySamples();

        std::vector<std::vector<float>> t (2, std::vector<float> ((size_t) n, 1.0f));
        dsp::GainTrace trace;
        trace.data[0][0] = t[0].data();
        trace.data[1][0] = t[1].data();
        trace.capacity = n;
        rig.proc->getEngine().setGainTrace (&trace);
        rig.render (in);
        rig.proc->getEngine().setGainTrace (nullptr);
        return t;
    };

    int latency = -1;
    const auto reference = traces (0.0f, 0.0f, latency);
    REQUIRE (latency == 0);
    const auto shifted = traces (2.0f, 5.0f, latency);
    REQUIRE (latency == 240);

    // Band 2 has the full lookahead: its gain is not delayed at all (it leads by 240 samples).
    // Band 1 leads by 96 samples, so its gain is delayed by 240 - 96 = 144.
    // After the 10 ms activity ramp (ignored at the start) the traces are the same values, shifted.
    bool band2Aligned = true, band1Aligned = true;
    for (int i = 1000; i < n; ++i)
    {
        band2Aligned = band2Aligned && shifted[1][(size_t) i] == reference[1][(size_t) i];
        band1Aligned = band1Aligned && shifted[0][(size_t) i] == reference[0][(size_t) (i - 144)];
    }
    CHECK (band2Aligned);
    CHECK (band1Aligned);
    CHECK (reference[0][5000] < 0.9f);                                   // the bands really compress
}

TEST_CASE ("Changing the lookahead while running crossfades without a step", "[dsp][lookahead]")
{
    const int n = 48000, block = 128, changeBlock = 100, changeAt = changeBlock * block;
    juce::AudioBuffer<float> in (1, n);
    const auto s = sine (300.0, kFs, n, 0.5);
    in.copyFrom (0, 0, s.data(), n);
    const float largestInputStep = 0.5f * (float) (2.0 * dsp::kPi * 300.0 / kFs) * 1.01f;

    struct Case { float from, to; };
    for (const Case& c : { Case { 0.0f, 5.0f }, Case { 5.0f, 1.0f }, Case { 4.0f, 0.0f }, Case { 2.0f, 10.0f } })
    {
        INFO ("lookahead " << c.from << " ms -> " << c.to << " ms");
        Rig rig;
        rig.band (1, "thresh", 12.0f);                                   // neutral band: the output is the delayed input
        rig.band (1, "lookahead", c.from);
        rig.restart();
        const int before = dsp::lookaheadSamples (c.from, kFs), after = dsp::lookaheadSamples (c.to, kFs);

        const auto out = rig.render (in, { block }, false, [&c] (int b, PluginProcessor& p)
        {
            if (b == changeBlock) setBand (p, 1, "lookahead", c.to);
        });
        CHECK (rig.proc->getLatencySamples() == after);

        // Exact old delay before the change, exact new delay once the fade is over.
        for (int i = std::max (before, 1); i < changeAt; ++i)
            REQUIRE (out.getSample (0, i) == in.getSample (0, i - before));
        const int settled = changeAt + after + (int) std::lround (dsp::kLookaheadFadeSeconds * kFs) + 2 * block;
        for (int i = settled; i < n; ++i)
            REQUIRE (out.getSample (0, i) == in.getSample (0, i - after));

        // In between: a blend of two delayed copies, so no sample jumps by more than a sine
        // of that frequency can (plus the slow crossfade), and it never exceeds the input peak.
        float largestStep = 0.0f, peak = 0.0f;
        for (int i = changeAt; i < settled; ++i)
        {
            largestStep = std::max (largestStep, std::abs (out.getSample (0, i) - out.getSample (0, i - 1)));
            peak = std::max (peak, std::abs (out.getSample (0, i)));
        }
        CHECK (largestStep < 1.5f * largestInputStep);
        CHECK (peak <= 0.5f + 1.0e-6f);
    }
}

TEST_CASE ("Static lookahead settings do not depend on the block layout", "[dsp][lookahead]")
{
    const auto in = makeNoise (2, 24000, 61u, 0.5f);
    const auto renderWith = [&in] (std::vector<int> blocks)
    {
        Rig rig (2, 3);
        for (int b = 1; b <= 3; ++b)
        {
            rig.band (b, "thresh", -40.0f);
            rig.band (b, "lookahead", 1.5f * (float) b);
        }
        rig.global ("mix", 70.0f);
        rig.global ("autoGain", 1.0f);
        rig.restart();
        return rig.render (in, std::move (blocks));
    };

    const auto reference = renderWith ({ 512 });
    CHECK (bitIdentical (reference, renderWith ({ 1 })));
    CHECK (bitIdentical (reference, renderWith ({ 64, 7, 300, 255, 257 })));
    CHECK_FALSE (bitIdentical (reference, in));
}
