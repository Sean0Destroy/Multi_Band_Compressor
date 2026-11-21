// Output stage: K-weighting filter, automatic gain (loudness match) and the clipper.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <complex>
#include <memory>

#include "dsp/KWeighting.h"
#include "dsp/OutputStage.h"
#include "dsp/Ramp.h"
#include "support/AllocationCounter.h"
#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
constexpr double kFs = 48000.0;

struct Rig
{
    std::unique_ptr<PluginProcessor> proc = std::make_unique<PluginProcessor>();
    OfflineRenderer renderer { *proc };
    int channels;

    explicit Rig (int numChannels = 2, int bands = 0) : channels (numChannels)
    {
        for (int i = 0; i < bands; ++i) proc->getBandSlots().createBand();
        REQUIRE (renderer.prepare (kFs, 512, { channels, 0 }));
    }

    void band (int slot, const char* suffix, float v) { setBand (*proc, slot, suffix, v); }
    void global (const char* id, float v)             { setPlain (*proc, id, v); }
    void restart() { REQUIRE (renderer.prepare (kFs, 512, { channels, 0 })); }

    juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& in, std::vector<int> blocks = { 256 }, bool bypassed = false)
    {
        return renderer.render (in, nullptr, blocks, bypassed);
    }
    float autoGainDb() const { return proc->getEngine().getAutoGainDb(); }
};

double biquadDb (const dsp::BiquadCoeffs& c, double f, double fs)
{
    const std::complex<double> z1 = std::polar (1.0, -2.0 * dsp::kPi * f / fs), z2 = z1 * z1;
    return 20.0 * std::log10 (std::abs ((c.b0 + c.b1 * z1 + c.b2 * z2) / (1.0 + c.a1 * z1 + c.a2 * z2)));
}

double kWeightDb (double f, double fs)
{
    return biquadDb (dsp::kWeightingShelf (fs), f, fs) + biquadDb (dsp::kWeightingHighPass (fs), f, fs);
}

float peakOf (const juce::AudioBuffer<float>& b, int start = 0)
{
    float m = 0.0f;
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = start; i < b.getNumSamples(); ++i)
            m = std::max (m, std::abs (b.getSample (c, i)));
    return m;
}

double rmsDb (const juce::AudioBuffer<float>& b, int start, int end)
{
    double acc = 0.0;
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = start; i < end; ++i) acc += (double) b.getSample (c, i) * b.getSample (c, i);
    return 10.0 * std::log10 (acc / ((double) (end - start) * b.getNumChannels()) + 1.0e-30);
}

juce::AudioBuffer<float> sineBuffer (double f, int n, double amplitude, int channels = 2)
{
    juce::AudioBuffer<float> b (channels, n);
    const auto s = sine (f, kFs, n, amplitude);
    for (int c = 0; c < channels; ++c) b.copyFrom (c, 0, s.data(), n);
    return b;
}
}

TEST_CASE ("K-weighting: published 48 kHz coefficients and the same response at other rates", "[dsp][loudness]")
{
    const auto shelf = dsp::kWeightingShelf (48000.0);
    CHECK_THAT (shelf.b0, WithinAbs (1.53512485958697, 1e-9));
    CHECK_THAT (shelf.b1, WithinAbs (-2.69169618940638, 1e-9));
    CHECK_THAT (shelf.b2, WithinAbs (1.19839281085285, 1e-9));
    CHECK_THAT (shelf.a1, WithinAbs (-1.69065929318241, 1e-9));
    CHECK_THAT (shelf.a2, WithinAbs (0.73248077421585, 1e-9));

    const auto hp = dsp::kWeightingHighPass (48000.0);
    CHECK (hp.b0 == 1.0); CHECK (hp.b1 == -2.0); CHECK (hp.b2 == 1.0);
    CHECK_THAT (hp.a1, WithinAbs (-1.99004745483398, 1e-9));
    CHECK_THAT (hp.a2, WithinAbs (0.99007225036621, 1e-9));

    // Shape: +0.69 dB at 997 Hz (the reason for the -0.691 offset in LKFS), about +4 dB at the
    // top, the high-pass 6 dB down at its corner (Q 0.5).
    CHECK_THAT (kWeightDb (997.0, 48000.0), WithinAbs (0.691, 0.01));
    CHECK_THAT (kWeightDb (10000.0, 48000.0), WithinAbs (4.0, 0.1));
    CHECK_THAT (biquadDb (hp, 38.135, 48000.0), WithinAbs (-6.02, 0.05));

    for (double fs : { 44100.0, 88200.0, 96000.0, 192000.0 })
        for (double f : { 30.0, 100.0, 997.0, 4000.0, 10000.0 })
        {
            INFO ("fs " << fs << " f " << f);
            CHECK_THAT (kWeightDb (f, fs), WithinAbs (kWeightDb (f, 48000.0), 0.12));
        }

    // The filter itself (time domain) follows the coefficients.
    dsp::KWeightingFilter filter;
    const auto s = sine (997.0, 48000.0, 48000, 0.5);
    double acc = 0.0;
    for (int i = 0; i < 48000; ++i)
    {
        const double y = filter.tick (shelf, hp, (double) s[(size_t) i]);
        if (i >= 24000) acc += y * y;
    }
    CHECK_THAT (10.0 * std::log10 (acc / 24000.0), WithinAbs (10.0 * std::log10 (0.125) + 0.691, 0.01));
    CHECK (filter.isFinite());
}

TEST_CASE ("Clipper curves: hard and soft", "[dsp][clip]")
{
    const float c = 0.8f;
    CHECK (dsp::hardClip (0.5f, c) == 0.5f);
    CHECK (dsp::hardClip (0.8f, c) == 0.8f);
    CHECK (dsp::hardClip (3.0f, c) == 0.8f);
    CHECK (dsp::hardClip (-3.0f, c) == -0.8f);

    // Soft: exactly linear up to half the ceiling, monotonic, odd, bounded by the ceiling.
    float previous = 0.0f;
    for (int i = 0; i <= 4000; ++i)
    {
        const float x = (float) i * 0.001f;                     // 0 .. 4
        const float y = dsp::softClip (x, c);
        if (x <= dsp::kSoftClipKnee * c) REQUIRE (y == x);
        REQUIRE (y >= previous);
        REQUIRE (y <= c);
        REQUIRE (dsp::softClip (-x, c) == -y);
        previous = y;
    }
    CHECK (dsp::softClip (100.0f, c) == c);
    CHECK_THAT ((double) dsp::softClip (1.0e6f, c), WithinAbs ((double) c, 1e-6));

    // Slope is continuous at the knee (1 just above it) and compressive beyond.
    const float knee = dsp::kSoftClipKnee * c;
    const float slope = (dsp::softClip (knee + 1.0e-3f, c) - dsp::softClip (knee, c)) / 1.0e-3f;
    CHECK_THAT ((double) slope, WithinAbs (1.0, 2.0e-3));
    CHECK (dsp::softClip (c, c) < c);
    CHECK (dsp::softClip (c, c) > 0.85f * c);
}

TEST_CASE ("Clipper in the plugin: last stage, ceiling holds, untouched below it", "[dsp][clip]")
{
    const int n = 24000;
    const auto loud = sineBuffer (220.0, n, 1.0);
    const float ceiling = dsp::dbToLinear (-6.0f);

    SECTION ("off by default; hard clip limits exactly and leaves the rest alone")
    {
        Rig rig;
        CHECK (bitIdentical (rig.render (loud), loud));

        rig.global ("clipMode", 2.0f);
        rig.global ("clipCeiling", -6.0f);
        rig.restart();
        const auto out = rig.render (loud, { 64, 300, 1 });
        CHECK (peakOf (out) == ceiling);
        int unchanged = 0, clipped = 0;
        for (int i = 0; i < n; ++i)
        {
            const float x = loud.getSample (0, i), y = out.getSample (0, i);
            if (std::abs (x) <= ceiling) { REQUIRE (y == x); ++unchanged; }
            else                         { REQUIRE (std::abs (y) == ceiling); ++clipped; }
        }
        CHECK (unchanged > 1000);
        CHECK (clipped > 1000);
    }

    SECTION ("soft clip stays below the ceiling and is transparent below half of it")
    {
        Rig rig;
        rig.global ("clipMode", 1.0f);
        rig.global ("clipCeiling", -6.0f);
        rig.restart();
        const auto out = rig.render (loud);
        CHECK (peakOf (out) < ceiling);
        CHECK (peakOf (out) > 0.9f * ceiling);

        const auto quiet = sineBuffer (220.0, n, 0.2);          // below half the ceiling (0.25)
        rig.restart();
        CHECK (bitIdentical (rig.render (quiet), quiet));
    }

    SECTION ("the clipper comes after the output gain and after automatic gain")
    {
        Rig rig;
        rig.global ("clipMode", 2.0f);
        rig.global ("clipCeiling", -0.3f);
        rig.global ("outGain", 18.0f);
        rig.restart();
        const auto out = rig.render (sineBuffer (220.0, n, 0.5));
        CHECK (peakOf (out) == dsp::dbToLinear (-0.3f));
    }

    SECTION ("a ceiling change is ramped")
    {
        Rig rig (1);
        rig.global ("clipMode", 2.0f);
        rig.global ("clipCeiling", 0.0f);
        rig.restart();
        juce::AudioBuffer<float> dc (1, 9600);
        for (int i = 0; i < 9600; ++i) dc.setSample (0, i, 2.0f);
        const auto out = rig.renderer.render (dc, nullptr, { 128 }, false, [] (int block, PluginProcessor& p)
        {
            if (block == 10) setPlain (p, "clipCeiling", -6.0f);
        });
        CHECK (out.getSample (0, 1279) == 1.0f);
        float largestStep = 0.0f;
        for (int i = 1; i < 9600; ++i)
        {
            REQUIRE (out.getSample (0, i) <= out.getSample (0, i - 1));               // only ever goes down
            largestStep = std::max (largestStep, out.getSample (0, i - 1) - out.getSample (0, i));
        }
        CHECK (largestStep < 0.002f);                                                 // 0.5 over 960 samples
        CHECK_THAT ((double) out.getSample (0, 9599), WithinRel ((double) dsp::dbToLinear (-6.0f), 1e-6));
    }

    SECTION ("bypass switches the clipper off: the input comes back exactly, even above the ceiling")
    {
        Rig rig;
        rig.global ("clipMode", 2.0f);
        rig.global ("clipCeiling", -6.0f);
        rig.restart();
        const auto hostBypassed = rig.render (loud, { 256 }, true);
        CHECK (bitIdentical (hostBypassed, loud));

        rig.global ("bypass", 1.0f);
        rig.restart();
        CHECK (bitIdentical (rig.render (loud), loud));
    }
}

TEST_CASE ("Automatic gain restores the loudness the compression removed", "[dsp][autogain]")
{
    const int n = 6 * 48000;
    const auto in = makeNoise (2, n, 17u, 0.3f);
    const auto configure = [] (Rig& r, bool autoGain)
    {
        // One wide band, heavy compression: about 12 dB of broadband level loss.
        r.band (1, "fLo", 20.0f); r.band (1, "fHi", 20000.0f);
        r.band (1, "thresh", -50.0f); r.band (1, "ratio", 4.0f);
        r.global ("autoGain", autoGain ? 1.0f : 0.0f);
        r.restart();
    };

    Rig plainRig (2, 1); configure (plainRig, false);
    const auto compressed = plainRig.render (in);
    CHECK (plainRig.autoGainDb() == 0.0f);
    const double loss = rmsDb (compressed, 4 * 48000, n) - rmsDb (in, 4 * 48000, n);
    REQUIRE (loss < -4.0);

    Rig rig (2, 1); configure (rig, true);
    test::resetAllocationCount();
    const auto out = rig.render (in);
    CHECK (test::allocationCount() == 0);

    // After the 1 s integration has settled the output is as loud as the input (within 0.5 dB),
    // and the reported correction is the loss that was measured without it.
    CHECK_THAT (rmsDb (out, 4 * 48000, n) - rmsDb (in, 4 * 48000, n), WithinAbs (0.0, 0.5));
    CHECK_THAT ((double) rig.autoGainDb(), WithinAbs (-loss, 0.7));

    // It is slow: it corrects the average level, not the gain reduction itself, so the first
    // 50 ms are still far below the input.
    CHECK (rmsDb (out, 480, 2400) < rmsDb (in, 480, 2400) - 2.0);

    // The same render in another block layout is bit-identical (no dependence on host blocks).
    Rig other (2, 1); configure (other, true);
    CHECK (bitIdentical (other.render (in, { 1024, 17, 300 }), out));
}

TEST_CASE ("Automatic gain: limits, gating, reference after the input gain, bypass", "[dsp][autogain]")
{
    SECTION ("the correction is limited to +-12 dB")
    {
        // The follower itself: processed 20 dB below / above the reference.
        for (double scale : { 0.1, 10.0 })
        {
            dsp::AutoGain follower;
            follower.prepare (kFs);
            const auto noise = makeNoise (2, 4 * 48000, 5u, 0.2f);
            float last = 1.0f;
            for (int i = 0; i < noise.getNumSamples(); ++i)
            {
                const float reference[2] { noise.getSample (0, i), noise.getSample (1, i) };
                const float processed[2] { (float) scale * reference[0], (float) scale * reference[1] };
                last = follower.tick (reference, processed, 2, i % dsp::kCoeffGrid == 0);
            }
            INFO ("processed = " << scale << " x reference");
            CHECK_THAT (follower.gainDb(), WithinAbs (scale < 1.0 ? 12.0 : -12.0, 0.01));
            CHECK_THAT (20.0 * std::log10 ((double) last), WithinAbs (follower.gainDb(), 1e-4));
            CHECK (follower.isFinite());
        }

        // Through the plugin: a band boosted by 24 dB is pulled back by the full 12 dB.
        Rig boost (2, 1);
        boost.band (1, "fLo", 20.0f); boost.band (1, "fHi", 20000.0f);
        boost.band (1, "thresh", 12.0f); boost.band (1, "makeup", 24.0f);
        boost.global ("autoGain", 1.0f);
        boost.restart();
        boost.render (makeNoise (2, 5 * 48000, 6u, 0.05f));
        CHECK_THAT ((double) boost.autoGainDb(), WithinAbs (-12.0, 0.05));
    }

    SECTION ("silence holds the correction instead of driving it")
    {
        Rig rig (2, 1);
        rig.band (1, "fLo", 20.0f); rig.band (1, "fHi", 20000.0f);
        rig.band (1, "thresh", -50.0f); rig.band (1, "ratio", 4.0f);
        rig.global ("autoGain", 1.0f);
        rig.restart();

        rig.render (makeNoise (2, 4 * 48000, 21u, 0.3f));
        const float before = rig.autoGainDb();
        REQUIRE (before > 3.0f);

        juce::AudioBuffer<float> silence (2, 5 * 48000);
        silence.clear();
        const auto tail = rig.render (silence);
        CHECK (peakOf (tail, 3 * 48000) < 1.0e-6f);            // only the band filter's decaying tail, no gain run-away
        CHECK_THAT ((double) rig.autoGainDb(), WithinAbs ((double) before, 1.0));     // held, not at the +12 dB limit

        // When the programme returns there is no burst: the first 100 ms are no louder than
        // the settled level.
        const auto again = rig.render (makeNoise (2, 4 * 48000, 22u, 0.3f));
        CHECK (rmsDb (again, 0, 4800) < rmsDb (again, 3 * 48000, 4 * 48000) + 1.5);
    }

    SECTION ("the reference is the signal after the input gain: input gain alone is not 'corrected'")
    {
        Rig rig (2, 0);
        rig.global ("inGain", 9.0f);
        rig.global ("autoGain", 1.0f);
        rig.restart();
        const auto in = makeNoise (2, 3 * 48000, 31u, 0.1f);
        const auto out = rig.render (in);
        CHECK_THAT ((double) rig.autoGainDb(), WithinAbs (0.0, 0.01));
        CHECK_THAT (rmsDb (out, 48000, 3 * 48000) - rmsDb (in, 48000, 3 * 48000), WithinAbs (9.0, 0.02));
    }

    SECTION ("switching it off fades the correction out; bypass returns the input exactly")
    {
        Rig rig (2, 1);
        rig.band (1, "fLo", 20.0f); rig.band (1, "fHi", 20000.0f);
        rig.band (1, "thresh", -50.0f); rig.band (1, "ratio", 4.0f);
        rig.global ("autoGain", 1.0f);
        rig.restart();
        const auto in = makeNoise (2, 3 * 48000, 41u, 0.3f);
        rig.render (in);
        REQUIRE (rig.autoGainDb() > 3.0f);

        const auto bypassed = rig.render (in, { 256 }, true);
        const int settle = (int) std::lround (dsp::kGainRampSeconds * kFs) + 512;
        for (int c = 0; c < 2; ++c)
            for (int i = settle; i < bypassed.getNumSamples(); ++i)
                REQUIRE (bypassed.getSample (c, i) == in.getSample (c, i));
        CHECK (rig.autoGainDb() == 0.0f);

        rig.global ("autoGain", 0.0f);
        rig.render (in);
        CHECK (rig.autoGainDb() == 0.0f);
    }
}
