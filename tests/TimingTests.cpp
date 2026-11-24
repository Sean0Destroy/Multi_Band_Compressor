// Timing character: frequency-dependent release, adaptive release, transient preservation.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>

#include "dsp/DynamicsCore.h"
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

    Rig()
    {
        proc->getBandSlots().createBand();
        REQUIRE (renderer.prepare (kFs, 512, { 1, 0 }));
    }

    void band (const char* suffix, float v) { setBand (*proc, 1, suffix, v); }
    void restart() { REQUIRE (renderer.prepare (kFs, 512, { 1, 0 })); }

    /** Renders and returns the band's applied gain in dB (<= 0), one value per sample. */
    std::vector<double> gainReduction (const juce::AudioBuffer<float>& in, juce::AudioBuffer<float>* out = nullptr)
    {
        const int n = in.getNumSamples();
        std::vector<float> trace ((size_t) n, 1.0f);
        dsp::GainTrace tr;
        tr.data[0][0] = trace.data();
        tr.capacity = n;
        proc->getEngine().setGainTrace (&tr);
        auto rendered = renderer.render (in, nullptr, { 128 });
        proc->getEngine().setGainTrace (nullptr);
        if (out != nullptr) *out = rendered;

        std::vector<double> db ((size_t) n);
        for (int i = 0; i < n; ++i) db[(size_t) i] = 20.0 * std::log10 ((double) trace[(size_t) i]);
        return db;
    }
};

/** Tone at f that is on from 0 to `onSamples` (2 ms fades), then silence. */
juce::AudioBuffer<float> burst (double f, double amplitude, int onSamples, int total)
{
    juce::AudioBuffer<float> b (1, total);
    b.clear();
    const auto s = sine (f, kFs, onSamples, amplitude);
    b.copyFrom (0, 0, s.data(), onSamples);
    const int fade = 96;
    for (int i = 0; i < fade && i < onSamples; ++i)
    {
        const float w = (float) (0.5 - 0.5 * std::cos (dsp::kPi * i / fade));
        b.setSample (0, i, b.getSample (0, i) * w);
        b.setSample (0, onSamples - 1 - i, b.getSample (0, onSamples - 1 - i) * w);
    }
    return b;
}

/** Seconds after `from` until the reduction has recovered to `fraction` of its value at `from`. */
double recoveryTime (const std::vector<double>& grDb, int from, double fraction)
{
    const double start = grDb[(size_t) from];
    for (int i = from; i < (int) grDb.size(); ++i)
        if (grDb[(size_t) i] >= start * fraction)
            return (double) (i - from) / kFs;
    return 1.0e9;
}
}

TEST_CASE ("Frequency-dependent release: formula and measured recovery", "[dsp][timing]")
{
    CHECK (dsp::frequencyScaledReleaseMs (150.0f, 100.0f, 400.0f, 0.0f) == 150.0f);          // off: untouched
    CHECK_THAT ((double) dsp::frequencyScaledReleaseMs (150.0f, 500.0f, 2000.0f, 100.0f), WithinRel (150.0, 1e-6));   // centre 1 kHz
    CHECK_THAT ((double) dsp::frequencyScaledReleaseMs (100.0f, 50.0f, 200.0f, 100.0f), WithinRel (100.0 * std::sqrt (10.0), 1e-5));
    CHECK_THAT ((double) dsp::frequencyScaledReleaseMs (100.0f, 5000.0f, 20000.0f, 100.0f), WithinRel (100.0 / std::sqrt (10.0), 1e-5));
    CHECK_THAT ((double) dsp::frequencyScaledReleaseMs (100.0f, 50.0f, 200.0f, 50.0f), WithinRel (100.0 * std::pow (10.0, 0.25), 1e-5));
    CHECK (dsp::frequencyScaledReleaseMs (5000.0f, 10.0f, 12.0f, 100.0f) <= 20000.0f);       // bounded
    CHECK (dsp::frequencyScaledReleaseMs (5.0f, 20000.0f, 30000.0f, 100.0f) >= 1.0f);

    // Measured: a low band (centre 100 Hz) recovers sqrt(10) times slower at 100 %.
    const int on = 48000, total = 3 * 48000;
    const auto measure = [&] (float lo, float hi, float amount)
    {
        Rig rig;
        rig.band ("fLo", lo); rig.band ("fHi", hi);
        rig.band ("thresh", -40.0f); rig.band ("ratio", 4.0f); rig.band ("knee", 0.0f);
        rig.band ("attack", 1.0f); rig.band ("release", 100.0f);
        rig.band ("freqRel", amount);
        rig.restart();
        const double fc = digitalCentre ((double) lo, (double) hi, kFs);
        const auto gr = rig.gainReduction (burst (fc, 0.5, on, total));
        REQUIRE (gr[(size_t) on - 200] < -6.0);
        // From 30 ms after the tone stops (the detector filter has rung out) to 1/e of that value.
        return recoveryTime (gr, on + 1440, 1.0 / std::exp (1.0));
    };

    const double lowPlain = measure (50.0f, 200.0f, 0.0f), lowScaled = measure (50.0f, 200.0f, 100.0f);
    CHECK_THAT (lowPlain, WithinRel (0.100, 0.08));
    CHECK_THAT (lowScaled / lowPlain, WithinRel (std::sqrt (10.0), 0.08));

    const double highPlain = measure (5000.0f, 20000.0f, 0.0f), highScaled = measure (5000.0f, 20000.0f, 100.0f);
    CHECK_THAT (highScaled / highPlain, WithinRel (1.0 / std::sqrt (10.0), 0.08));
}

TEST_CASE ("Adaptive release: brief peaks recover at the set release, sustained reduction slowly", "[dsp][timing]")
{
    const double fc = digitalCentre (500.0, 2000.0, kFs);
    const auto run = [&] (float adaptive, int onSamples, int total)
    {
        Rig rig;
        rig.band ("fLo", 500.0f); rig.band ("fHi", 2000.0f);
        rig.band ("thresh", -40.0f); rig.band ("ratio", 4.0f); rig.band ("knee", 0.0f);
        rig.band ("attack", 1.0f); rig.band ("release", 100.0f);
        rig.band ("adaptRel", adaptive);
        rig.restart();
        return rig.gainReduction (burst (fc, 0.5, onSamples, total));
    };

    const int shortOn = 2400 /* 50 ms */, longOn = 4 * 48000, tail = 4 * 48000;

    // Time from the end of the tone until 70 % of the reduction present at that moment is gone.
    const auto timeToRecover = [] (const std::vector<double>& gr, int from) { return recoveryTime (gr, from, 0.3); };

    const auto shortPlain = run (0.0f, shortOn, shortOn + tail), shortAdaptive = run (100.0f, shortOn, shortOn + tail);
    const auto longPlain = run (0.0f, longOn, longOn + tail), longAdaptive = run (100.0f, longOn, longOn + tail);

    const double tShortPlain = timeToRecover (shortPlain, shortOn), tShortAdaptive = timeToRecover (shortAdaptive, shortOn);
    const double tLongPlain = timeToRecover (longPlain, longOn), tLongAdaptive = timeToRecover (longAdaptive, longOn);
    INFO ("70 % recovery: short " << tShortPlain << " / " << tShortAdaptive << " s, long " << tLongPlain << " / " << tLongAdaptive << " s");

    // Without it the recovery does not depend on how long the tone lasted.
    CHECK_THAT (tLongPlain, WithinRel (tShortPlain, 0.10));
    // With it a 50 ms peak recovers almost as fast as before (only a small remainder lingers);
    // after 4 s of reduction the whole recovery is several times slower.
    CHECK (tShortAdaptive < 1.3 * tShortPlain);
    CHECK (tLongAdaptive > 4.0 * tLongPlain);
    CHECK (tLongAdaptive < 2.0);                                          // but it does recover (slow time 0.8 s)

    // What lingers after the short peak is small: 0.5 s later less than 1.5 dB.
    CHECK (shortAdaptive[(size_t) shortOn + 24000] > -1.5);
    CHECK (shortAdaptive[(size_t) shortOn + 24000] < shortPlain[(size_t) shortOn + 24000]);

    // It only ever slows the recovery: never more reduction than the plain compressor reached,
    // and identical while the tone is on at steady state.
    double deepestPlain = 0.0, deepestAdaptive = 0.0;
    for (size_t i = 0; i < longPlain.size(); ++i)
    {
        deepestPlain = std::min (deepestPlain, longPlain[i]);
        deepestAdaptive = std::min (deepestAdaptive, longAdaptive[i]);
        REQUIRE (longAdaptive[i] <= longPlain[i] + 1.0e-4);
    }
    CHECK_THAT (deepestAdaptive, WithinAbs (deepestPlain, 1.0e-3));
    CHECK_THAT (longAdaptive[(size_t) longOn - 4800], WithinAbs (longPlain[(size_t) longOn - 4800], 1.0e-3));

    // Half amount sits between the two.
    const auto longHalf = run (50.0f, longOn, longOn + tail);
    const double tLongHalf = timeToRecover (longHalf, longOn);
    CHECK (tLongHalf > tLongPlain);
    CHECK (tLongHalf < tLongAdaptive);
}

TEST_CASE ("Transient preservation: onsets are compressed less, sustained level is unchanged", "[dsp][timing]")
{
    const double fc = digitalCentre (500.0, 2000.0, kFs);
    const int total = 48000;
    const auto in = burst (fc, 0.5, total, total);                        // 2 ms fade-in, then steady

    const auto run = [&] (float amount, juce::AudioBuffer<float>& out)
    {
        Rig rig;
        rig.band ("fLo", 500.0f); rig.band ("fHi", 2000.0f);
        rig.band ("thresh", -40.0f); rig.band ("ratio", 8.0f); rig.band ("knee", 0.0f);
        rig.band ("attack", 0.05f); rig.band ("release", 100.0f);
        rig.band ("transient", amount);
        rig.restart();
        return rig.gainReduction (in, &out);
    };

    juce::AudioBuffer<float> outPlain, outPreserved;
    const auto plain = run (0.0f, outPlain), preserved = run (100.0f, outPreserved);

    // During the first 10 ms the reduction is clearly smaller with preservation on ...
    double earlyPlain = 0.0, earlyPreserved = 0.0;
    for (int i = 96; i < 480; ++i) { earlyPlain += plain[(size_t) i]; earlyPreserved += preserved[(size_t) i]; }
    earlyPlain /= 384.0; earlyPreserved /= 384.0;
    INFO ("mean reduction 2..10 ms: plain " << earlyPlain << " dB, preserved " << earlyPreserved << " dB");
    CHECK (earlyPlain < -6.0);
    CHECK (earlyPreserved > 0.6 * earlyPlain);                            // at least 40 % less reduction on the onset

    // ... so the onset comes out louder ...
    float peakPlain = 0.0f, peakPreserved = 0.0f;
    for (int i = 0; i < 720; ++i)
    {
        peakPlain = std::max (peakPlain, std::abs (outPlain.getSample (0, i)));
        peakPreserved = std::max (peakPreserved, std::abs (outPreserved.getSample (0, i)));
    }
    CHECK (peakPreserved > 1.15f * peakPlain);
    CHECK (peakPreserved > 0.9f * 0.5f);                                  // the onset passes almost untouched

    // ... while the sustained part is compressed exactly as before, with no added gain ripple.
    double minPlain = 0.0, maxPlain = -200.0, minPreserved = 0.0, maxPreserved = -200.0;
    for (int i = total - 9600; i < total - 200; ++i)
    {
        minPlain = std::min (minPlain, plain[(size_t) i]);         maxPlain = std::max (maxPlain, plain[(size_t) i]);
        minPreserved = std::min (minPreserved, preserved[(size_t) i]); maxPreserved = std::max (maxPreserved, preserved[(size_t) i]);
    }
    CHECK_THAT (minPreserved, WithinAbs (minPlain, 0.15));
    CHECK ((maxPreserved - minPreserved) < (maxPlain - minPlain) + 0.15);

    // It never adds reduction.
    for (int i = 0; i < total; ++i)
        REQUIRE (preserved[(size_t) i] >= plain[(size_t) i] - 0.5);
}

TEST_CASE ("Timing features at 0 % are bit-identical to the plain compressor", "[dsp][timing]")
{
    // Loud for a second, then quiet for 1.5 s: an onset, sustained reduction and a full recovery.
    auto in = makeNoise (1, 120000, 5u, 0.5f);
    in.applyGain (0, 48000, 72000, 0.02f);
    Rig plainRig;
    plainRig.band ("thresh", -40.0f);
    plainRig.restart();
    const auto expected = plainRig.renderer.render (in, nullptr, { 128 });

    // Switching each feature on changes the result; back to 0 % it is the same again.
    for (auto* feature : { "adaptRel", "freqRel", "transient" })
    {
        INFO (feature);
        Rig rig;
        rig.band ("thresh", -40.0f);
        rig.band (feature, 100.0f);
        rig.restart();
        CHECK_FALSE (bitIdentical (rig.renderer.render (in, nullptr, { 128 }), expected));
        rig.band (feature, 0.0f);
        rig.restart();
        CHECK (bitIdentical (rig.renderer.render (in, nullptr, { 128 }), expected));
    }
}
