// Coloration ("character") stage and its oversampling.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <complex>
#include <memory>

#include "dsp/Character.h"
#include "dsp/Lookahead.h"
#include "dsp/Oversampler.h"
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

    explicit Rig (int numChannels = 1, int bands = 0) : channels (numChannels)
    {
        for (int i = 0; i < bands; ++i) proc->getBandSlots().createBand();
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

juce::AudioBuffer<float> sineBuffer (double f, int n, double amplitude, int channels = 1)
{
    juce::AudioBuffer<float> b (channels, n);
    const auto s = sine (f, kFs, n, amplitude);
    for (int c = 0; c < channels; ++c) b.copyFrom (c, 0, s.data(), n);
    return b;
}

/** Level in dB (re full scale sine amplitude 1) of the component at f, Hann-windowed DTFT. */
double componentDb (const juce::AudioBuffer<float>& b, double f, int start, int end)
{
    double re = 0.0, im = 0.0, wsum = 0.0;
    const int n = end - start;
    for (int i = 0; i < n; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos (2.0 * dsp::kPi * i / n);
        const double ph = 2.0 * dsp::kPi * f * (double) (start + i) / kFs;
        re += w * b.getSample (0, start + i) * std::cos (ph);
        im += w * b.getSample (0, start + i) * std::sin (ph);
        wsum += w;
    }
    return 20.0 * std::log10 (2.0 * std::sqrt (re * re + im * im) / wsum + 1.0e-15);
}

bool isDelayedCopy (const juce::AudioBuffer<float>& out, const juce::AudioBuffer<float>& in, int delay, int start = 0)
{
    for (int c = 0; c < out.getNumChannels(); ++c)
        for (int i = start; i < out.getNumSamples(); ++i)
            if (out.getSample (c, i) != (i >= delay ? in.getSample (c, i - delay) : 0.0f)) return false;
    return true;
}

double stageResponseDb (const dsp::HalfbandStage& s, double normalisedFrequency)   // cycles per sample at the high rate
{
    std::complex<double> h = 0.0;
    for (int k = 0; k < s.taps(); ++k)
        h += (double) s.coefficient (k) * std::polar (1.0, -2.0 * dsp::kPi * normalisedFrequency * k);
    return 20.0 * std::log10 (std::abs (h) + 1.0e-15);
}
}

TEST_CASE ("Oversampling filters: pass band, stop band, half-band structure", "[dsp][oversampling]")
{
    dsp::Oversampler os;
    const int taps[3] { 95, 25, 17 };
    // Pass band edge and stop band start of each stage, in cycles per sample at its high rate.
    const double passEdge[3] { 0.215, 0.125, 0.0625 }, stopEdge[3] { 0.285, 0.375, 0.4375 };

    for (int s = 0; s < 3; ++s)
    {
        INFO ("stage " << s);
        const auto& stage = os.stage (s);
        REQUIRE (stage.taps() == taps[s]);

        for (double f = 0.0; f <= passEdge[s]; f += passEdge[s] / 40.0)
            REQUIRE (std::abs (stageResponseDb (stage, f)) < 0.01);
        for (double f = stopEdge[s]; f <= 0.5; f += (0.5 - stopEdge[s]) / 40.0)
            REQUIRE (stageResponseDb (stage, f) < -85.0);

        // Symmetric (linear phase), half-band zeros at even distances from the centre.
        const int centre = (taps[s] - 1) / 2;
        for (int k = 0; k < taps[s]; ++k)
        {
            REQUIRE (stage.coefficient (k) == stage.coefficient (taps[s] - 1 - k));
            if (k != centre && (k - centre) % 2 == 0) REQUIRE (stage.coefficient (k) == 0.0f);
        }
        CHECK_THAT (stageResponseDb (stage, 0.25), WithinAbs (-6.02, 0.05));
    }

    CHECK (dsp::oversamplingLatency (0) == 0);
    CHECK (dsp::oversamplingLatency (1) == 47);
    CHECK (dsp::oversamplingLatency (2) == 53);
    CHECK (dsp::oversamplingLatency (3) == 55);
}

TEST_CASE ("Oversampler round trip: exact latency, flat pass band, any block size", "[dsp][oversampling]")
{
    for (int stages = 1; stages <= 3; ++stages)
    {
        INFO ("stages " << stages);
        const int latency = dsp::oversamplingLatency (stages);
        auto os = std::make_unique<dsp::Oversampler>();
        const auto identity = [] (float*, int) {};

        // Impulse: the round trip peaks exactly at the latency with (almost) unit height.
        std::vector<float> x (256, 0.0f);
        x[0] = 1.0f;
        os->process (x.data(), 256, stages, identity);
        int peak = 0;
        for (int i = 1; i < 256; ++i) if (std::abs (x[(size_t) i]) > std::abs (x[(size_t) peak])) peak = i;
        CHECK (peak == latency);
        CHECK_THAT ((double) x[(size_t) peak], WithinAbs (1.0, 0.05));     // band-limited: the top of the spectrum is filtered off

        // Sines through the round trip equal the delayed input (1 kHz to 18 kHz at 48 kHz).
        for (double f : { 1000.0, 10000.0, 18000.0 })
        {
            os->reset();
            const auto s = sine (f, kFs, 4096, 0.5);
            auto y = s;
            for (int pos = 0; pos < 4096; pos += 100)                      // odd block sizes on purpose
                os->process (y.data() + pos, std::min (100, 4096 - pos), stages, identity);
            double worst = 0.0;
            for (int i = 512; i < 4096; ++i)
                worst = std::max (worst, std::abs ((double) y[(size_t) i] - s[(size_t) (i - latency)]));
            INFO ("f " << f << " worst error " << worst);
            CHECK (worst < 0.5 * 2.0e-3);                                  // below -54 dB re the signal (0.01 dB ripple squared)
        }

        // The shaper really sees the oversampled block.
        os->reset();
        std::vector<float> z (64, 0.25f);
        int seen = 0;
        os->process (z.data(), 64, stages, [&seen] (float*, int count) { seen = count; });
        CHECK (seen == 64 << stages);
    }
}

TEST_CASE ("Warm FET curve: unity small-signal gain, bounded, asymmetric", "[dsp][character]")
{
    dsp::WarmFetCurve curve;
    CHECK_FALSE (curve.set (0.0f));                                       // identity: the stage is skipped
    for (float drive : { 0.1f, 0.5f, 1.0f })
    {
        INFO ("drive " << drive);
        REQUIRE (curve.set (drive));
        CHECK (curve (0.0f) == 0.0f);                                     // no DC step for silence
        CHECK_THAT ((double) ((curve (1.0e-3f) - curve (-1.0e-3f)) / 2.0e-3f), WithinAbs (1.0, 2.0e-3));   // slope 1 at the origin

        float previous = curve (-4.0f);
        for (float x = -4.0f; x <= 4.0f; x += 0.01f)
        {
            const float y = curve (x);
            REQUIRE (y >= previous);                                      // monotonic
            REQUIRE (std::abs (y) <= 1.2f * std::abs (x) + 1.0e-6f);      // the negative half expands a little (asymmetry), no more
            previous = y;
        }
        CHECK (std::abs (curve (0.5f)) < std::abs (curve (-0.5f)));       // asymmetric (even harmonics)
    }
    REQUIRE (curve.set (1.0f));
    CHECK (curve (10.0f) < 1.0f);                                         // saturates
}

TEST_CASE ("Character: harmonics grow with drive, small signals pass unchanged, no DC", "[dsp][character]")
{
    const int n = 48000;
    const auto measure = [&] (float drive, double amplitude, double& h1, double& h2, double& h3, double& dc)
    {
        Rig rig;
        rig.global ("charModel", 1.0f);
        rig.global ("charDrive", drive);
        rig.global ("oversampling", 2.0f);                                // 4x: the harmonics are clean
        rig.restart();
        const auto out = rig.render (sineBuffer (1000.0, n, amplitude));
        h1 = componentDb (out, 1000.0, n / 2, n);
        h2 = componentDb (out, 2000.0, n / 2, n);
        h3 = componentDb (out, 3000.0, n / 2, n);
        double sum = 0.0;
        for (int i = n / 2; i < n; ++i) sum += out.getSample (0, i);
        dc = sum / (n / 2);
    };

    double h1, h2, h3, dc;
    measure (50.0f, 0.25, h1, h2, h3, dc);
    const double fundamental = 20.0 * std::log10 (0.25);
    CHECK (h1 < fundamental);                                             // peaks are rounded: slightly quieter
    CHECK (h1 > fundamental - 1.5);
    CHECK (h2 > h1 - 40.0);                                               // clear second harmonic (asymmetry)
    CHECK (h3 > h1 - 50.0);                                               // and third (tanh)
    CHECK (std::abs (dc) < 1.0e-4);                                       // DC removed

    double g1, g2, g3, gdc;
    measure (100.0f, 0.25, g1, g2, g3, gdc);
    CHECK (g2 - g1 > h2 - h1 + 3.0);                                      // more drive, more harmonics
    CHECK (g3 - g1 > h3 - h1 + 3.0);

    // A very quiet signal is passed with unity gain and negligible distortion.
    double q1, q2, q3, qdc;
    measure (100.0f, 0.001, q1, q2, q3, qdc);
    CHECK_THAT (q1, WithinAbs (-60.0, 0.05));
    CHECK (q2 < q1 - 50.0);
}

TEST_CASE ("Oversampling removes the aliasing of the coloration", "[dsp][character][oversampling]")
{
    // 9 kHz at -6 dBFS, full drive, 48 kHz. Harmonics above 24 kHz fold back when the curve runs
    // at the base rate: the 4th (36 kHz) lands on 12 kHz, the 5th (45 kHz) on 3 kHz.
    const int n = 48000;
    const auto aliases = [&] (float oversampling, double& at3k, double& at12k, double& fundamental)
    {
        Rig rig;
        rig.global ("charModel", 1.0f);
        rig.global ("charDrive", 100.0f);
        rig.global ("oversampling", oversampling);
        rig.restart();
        const auto out = rig.render (sineBuffer (9000.0, n, 0.5));
        at3k = componentDb (out, 3000.0, n / 2, n);
        at12k = componentDb (out, 12000.0, n / 2, n);
        fundamental = componentDb (out, 9000.0, n / 2, n);
    };

    double off3, off12, offF, x2_3, x2_12, x2F, x4_3, x4_12, x4F, x8_3, x8_12, x8F;
    aliases (0.0f, off3, off12, offF);
    aliases (1.0f, x2_3, x2_12, x2F);
    aliases (2.0f, x4_3, x4_12, x4F);
    aliases (3.0f, x8_3, x8_12, x8F);
    INFO ("alias at 3 kHz: off " << off3 << ", 2x " << x2_3 << ", 4x " << x4_3 << ", 8x " << x8_3 << " dB");
    INFO ("alias at 12 kHz: off " << off12 << ", 2x " << x2_12 << ", 4x " << x4_12 << ", 8x " << x8_12 << " dB");

    CHECK (off3 > -70.0);                                                 // without oversampling the aliases are there
    CHECK (off12 > -60.0);
    CHECK (x2_12 < off12 - 40.0);                                         // 2x: the 4th harmonic (36 kHz) no longer folds
    CHECK (x4_3 < off3 - 40.0);                                           // 4x: nor does the 5th (45 kHz)
    CHECK (x4_12 < off12 - 40.0);
    CHECK (x8_3 < off3 - 40.0);
    CHECK_THAT (x4F, WithinAbs (offF, 0.3));                              // the tone itself is the same
    CHECK_THAT (x8F, WithinAbs (x4F, 0.05));
}

TEST_CASE ("Oversampling latency: reported, exact on the clean path, dry stays aligned", "[dsp][character][oversampling]")
{
    const auto in = makeNoise (2, 9600, 13u, 0.5f);
    const int expected[4] { 0, 47, 53, 55 };

    for (int choice = 0; choice <= 3; ++choice)
    {
        INFO ("oversampling choice " << choice);
        Rig rig (2);
        rig.global ("oversampling", (float) choice);
        CHECK (rig.proc->getLatencySamples() == expected[choice]);       // reported as soon as it is chosen
        rig.restart();

        // Clean model: an exact delay, whatever the block layout; no allocation.
        test::resetAllocationCount();
        const auto out = rig.render (in, { 64, 300, 1, 512 });
        CHECK (test::allocationCount() == 0);
        CHECK (isDelayedCopy (out, in, expected[choice]));

        // Global mix at 50 %: dry and wet are the same delayed signal.
        rig.global ("mix", 50.0f);
        rig.restart();
        CHECK (isDelayedCopy (rig.render (in), in, expected[choice]));

        // Bypass keeps the delay: after the 10 ms fade out of the colour the output is the
        // delayed input exactly.
        rig.global ("mix", 100.0f);
        rig.global ("charModel", 1.0f);
        rig.global ("charDrive", 80.0f);
        rig.restart();
        CHECK (isDelayedCopy (rig.render (in, { 128 }, true), in, expected[choice], 480 + 256));
        CHECK_FALSE (isDelayedCopy (rig.render (in), in, expected[choice], 2400));   // not bypassed: coloured
    }

    // Lookahead and oversampling add up.
    Rig rig (2, 1);
    setBand (*rig.proc, 1, "thresh", 12.0f);
    setBand (*rig.proc, 1, "lookahead", 5.0f);
    rig.global ("oversampling", 2.0f);
    CHECK (rig.proc->getLatencySamples() == 240 + 53);
    rig.restart();
    CHECK (isDelayedCopy (rig.render (in), in, 293));
}

TEST_CASE ("Character changes while running are click-free; static settings ignore the block layout", "[dsp][character]")
{
    const int n = 24000, block = 128;
    const auto in = sineBuffer (300.0, n, 0.5);
    const float inputStep = 0.5f * (float) (2.0 * dsp::kPi * 300.0 / kFs);

    const auto largestStep = [] (const juce::AudioBuffer<float>& b)
    {
        float m = 0.0f;
        for (int i = 1; i < b.getNumSamples(); ++i) m = std::max (m, std::abs (b.getSample (0, i) - b.getSample (0, i - 1)));
        return m;
    };

    SECTION ("switching the model, the drive and the oversampling factor")
    {
        Rig rig;
        rig.global ("charDrive", 100.0f);
        rig.restart();
        const auto out = rig.render (in, { block }, false, [] (int b, PluginProcessor& p)
        {
            if (b == 20)  setPlain (p, "charModel", 1.0f);       // colour in
            if (b == 50)  setPlain (p, "oversampling", 2.0f);    // 4x: latency 0 -> 53
            if (b == 90)  setPlain (p, "charDrive", 20.0f);
            if (b == 120) setPlain (p, "oversampling", 0.0f);    // back to none
            if (b == 150) setPlain (p, "charModel", 0.0f);       // clean again
        });

        CHECK (largestStep (out) < 2.0f * inputStep);
        for (int i = 0; i < 20 * block; ++i) REQUIRE (out.getSample (0, i) == in.getSample (0, i));
        for (int i = 150 * block + 4000; i < n; ++i) REQUIRE (out.getSample (0, i) == in.getSample (0, i));   // exactly clean again
        CHECK (rig.proc->getLatencySamples() == 0);
    }

    SECTION ("static coloured settings are bit-identical across block layouts")
    {
        const auto noise = makeNoise (2, 12000, 71u, 0.5f);
        const auto renderWith = [&noise] (std::vector<int> blocks)
        {
            Rig rig (2, 2);
            setBand (*rig.proc, 1, "thresh", -40.0f);
            rig.global ("charModel", 1.0f);
            rig.global ("charDrive", 60.0f);
            rig.global ("oversampling", 3.0f);
            rig.global ("mix", 70.0f);
            rig.restart();
            return rig.render (noise, std::move (blocks));
        };
        const auto reference = renderWith ({ 512 });
        CHECK (bitIdentical (reference, renderWith ({ 1 })));
        CHECK (bitIdentical (reference, renderWith ({ 64, 7, 300, 255, 257 })));
    }

    SECTION ("drive 0 or the clean model with no oversampling touches nothing")
    {
        const auto noise = makeNoise (2, 9600, 3u, 0.5f);
        Rig a (2);
        a.global ("charModel", 1.0f);                             // warm, but drive 0
        a.restart();
        CHECK (bitIdentical (a.render (noise), noise));
        Rig b (2);
        b.global ("charDrive", 100.0f);                           // drive, but clean model
        b.restart();
        CHECK (bitIdentical (b.render (noise), noise));
    }
}
