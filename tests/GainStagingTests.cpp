// Gain staging through the plugin processor: per-band makeup and mix, global input gain,
// output gain and mix; ramps, bypass behaviour and block-layout independence.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <memory>

#include "dsp/Ramp.h"
#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
struct Rig
{
    std::unique_ptr<PluginProcessor> proc = std::make_unique<PluginProcessor>();
    OfflineRenderer renderer { *proc };
    double fs;
    int channels;

    Rig (double sampleRate, int numChannels = 1, int bands = 1) : fs (sampleRate), channels (numChannels)
    {
        for (int i = 0; i < bands; ++i) proc->getBandSlots().createBand();
        REQUIRE (renderer.prepare (fs, 512, { channels, 0 }));
    }

    void band (int slot, const char* suffix, float v) { setBand (*proc, slot, suffix, v); }
    void global (const char* paramId, float v)        { setPlain (*proc, paramId, v); }
    void restart() { REQUIRE (renderer.prepare (fs, 512, { channels, 0 })); }

    juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& in, std::vector<int> blocks = { 128 },
                                     bool bypassed = false, const OfflineRenderer::BlockHook& hook = {})
    {
        return renderer.render (in, nullptr, blocks, bypassed, hook);
    }

    float grDb (int slot, int ch = 0) const { return proc->getGainReductionMeter().getGainReductionDb (slot, ch); }
};

juce::AudioBuffer<float> sineBuffer (double f, double fs, int n, double amplitude, int channels = 1)
{
    juce::AudioBuffer<float> b (channels, n);
    const auto s = sine (f, fs, n, amplitude);
    for (int c = 0; c < channels; ++c) b.copyFrom (c, 0, s.data(), n);
    return b;
}

double rmsOf (const juce::AudioBuffer<float>& b, int ch, int start, int end)
{
    double acc = 0.0;
    for (int i = start; i < end; ++i) acc += (double) b.getSample (ch, i) * b.getSample (ch, i);
    return std::sqrt (acc / (end - start));
}

double gainDb (const juce::AudioBuffer<float>& out, const juce::AudioBuffer<float>& in, int start, int end, int ch = 0)
{
    return dbOf (rmsOf (out, ch, start, end) / rmsOf (in, ch, start, end));
}

double maxAbsDiff (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int start = 0)
{
    double m = 0.0;
    for (int c = 0; c < a.getNumChannels(); ++c)
        for (int i = start; i < a.getNumSamples(); ++i)
            m = std::max (m, std::abs ((double) a.getSample (c, i) - (double) b.getSample (c, i)));
    return m;
}

constexpr double kFs = 48000.0;
}

TEST_CASE ("Ramp: reaches the target exactly after its length and holds it", "[dsp][gain]")
{
    dsp::LinearRamp r;
    r.prepare (48000.0, 0.020);
    CHECK (r.value() == 0.0f);
    CHECK_FALSE (r.isRamping());

    r.setTarget (6.0f);
    CHECK (r.isRamping());
    float previous = r.value();
    for (int i = 0; i < 959; ++i)
    {
        const float v = r.tick();
        CHECK (v > previous);          // strictly monotonic on the way up
        CHECK (v < 6.0f);
        previous = v;
    }
    CHECK (r.tick() == 6.0f);          // sample 960 lands exactly
    CHECK_FALSE (r.isRamping());
    CHECK (r.tick() == 6.0f);

    r.setTarget (6.0f);                // same target: no new ramp
    CHECK_FALSE (r.isRamping());
    r.setTarget (std::numeric_limits<float>::quiet_NaN());   // non-finite targets are ignored
    CHECK_FALSE (r.isRamping());
    CHECK (r.targetValue() == 6.0f);

    dsp::GainRamp g;
    g.prepare (48000.0);
    CHECK (g.isUnity());
    CHECK (g.tick() == 1.0f);
    g.setTargetDb (-6.0f);
    CHECK_FALSE (g.isUnity());
    float last = 1.0f;
    for (int i = 0; i < 960; ++i) last = g.tick();
    CHECK_THAT ((double) last, WithinRel (std::pow (10.0, -6.0 / 20.0), 1e-6));
    g.setTargetDb (0.0f);
    for (int i = 0; i < 960; ++i) last = g.tick();
    CHECK (last == 1.0f);              // back to exactly unity
    CHECK (g.isUnity());

    CHECK (dsp::dbToLinear (0.0f) == 1.0f);
    CHECK (dsp::dbToLinear (-0.0f) == 1.0f);
}

TEST_CASE ("Makeup raises the band without showing as gain reduction", "[dsp][gain]")
{
    Rig rig (kFs);
    rig.band (1, "fLo", 500.0f);
    rig.band (1, "fHi", 2000.0f);
    rig.band (1, "thresh", 12.0f);               // never compresses
    const double fc = digitalCentre (500.0, 2000.0, kFs);
    const int n = 48000;

    for (float makeup : { 6.0f, -6.0f, 24.0f, -24.0f })
    {
        INFO ("makeup " << makeup << " dB");
        rig.band (1, "makeup", makeup);
        rig.restart();

        const auto centre = sineBuffer (fc, kFs, n, 0.05);
        CHECK_THAT (gainDb (rig.render (centre), centre, n / 2, n), WithinAbs ((double) makeup, 0.02));
        CHECK (rig.grDb (1) == 0.0f);            // makeup is not gain reduction

        rig.restart();
        const auto far = sineBuffer (30.0, kFs, n, 0.05);
        // Far below the band the A3 bell has little effect (its skirts widen with large boosts).
        CHECK (std::abs (gainDb (rig.render (far), far, n / 2, n)) < (std::abs (makeup) > 12.0f ? 2.5 : 0.1));
    }
}

TEST_CASE ("Makeup combines with compression: applied gain = compression gain * makeup", "[dsp][gain]")
{
    const int n = 48000;
    const double fc = digitalCentre (500.0, 2000.0, kFs);
    const auto in = sineBuffer (fc, kFs, n, 0.5);

    Rig plainRig (kFs);
    plainRig.band (1, "fLo", 500.0f);  plainRig.band (1, "fHi", 2000.0f);
    plainRig.band (1, "thresh", -30.0f); plainRig.band (1, "ratio", 4.0f);
    plainRig.restart();
    const double compressed = gainDb (plainRig.render (in), in, n / 2, n);
    const float meterPlain = plainRig.grDb (1);
    REQUIRE (compressed < -6.0);

    Rig rig (kFs);
    rig.band (1, "fLo", 500.0f);  rig.band (1, "fHi", 2000.0f);
    rig.band (1, "thresh", -30.0f); rig.band (1, "ratio", 4.0f);
    rig.band (1, "makeup", 9.0f);
    rig.restart();
    CHECK_THAT (gainDb (rig.render (in), in, n / 2, n), WithinAbs (compressed + 9.0, 0.02));

    // The detector is feed-forward on the input, so the GR read-out is unchanged by makeup.
    CHECK (rig.grDb (1) == meterPlain);
}

TEST_CASE ("Band mix blends the band's change; 0 % is bit-identical to the input", "[dsp][gain]")
{
    const auto in = makeNoise (2, 48000, 21u, 0.5f);
    const auto configure = [] (Rig& r, float mix)
    {
        r.band (1, "thresh", -40.0f); r.band (1, "ratio", 8.0f); r.band (1, "makeup", 4.0f);
        r.band (1, "fLo", 200.0f);    r.band (1, "fHi", 4000.0f);
        r.band (1, "mix", mix);
        r.restart();
    };

    Rig wet (kFs, 2);  configure (wet, 100.0f);
    Rig half (kFs, 2); configure (half, 50.0f);
    Rig dry (kFs, 2);  configure (dry, 0.0f);

    const auto outWet = wet.render (in);
    const auto outHalf = half.render (in);
    const auto outDry = dry.render (in);

    CHECK (bitIdentical (outDry, in));
    CHECK_FALSE (bitIdentical (outWet, in));

    // For a single A3 band the band mix is an exact parallel blend: out = in + m (wet - in).
    juce::AudioBuffer<float> expected (2, in.getNumSamples());
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < in.getNumSamples(); ++i)
            expected.setSample (c, i, in.getSample (c, i) + 0.5f * (outWet.getSample (c, i) - in.getSample (c, i)));
    CHECK (maxAbsDiff (outHalf, expected) < 2.0e-6);

    // The meter keeps showing the compressor's gain reduction, whatever the mix.
    CHECK (dry.grDb (1) == wet.grDb (1));
    CHECK (wet.grDb (1) < -3.0f);
}

TEST_CASE ("Band bypass removes makeup and mix with the activity ramp", "[dsp][gain]")
{
    Rig rig (kFs);
    rig.band (1, "thresh", -40.0f);
    rig.band (1, "makeup", 12.0f);
    rig.band (1, "mix", 70.0f);
    rig.band (1, "bypass", 1.0f);
    rig.restart();

    const auto in = makeNoise (1, 9600, 5u, 0.5f);
    CHECK (bitIdentical (rig.render (in), in));          // bypassed from the start: untouched

    rig.band (1, "bypass", 0.0f);
    rig.render (in);
    rig.band (1, "bypass", 1.0f);
    const auto out = rig.render (in);
    const int ramp = (int) std::ceil (dsp::kActivityRampSeconds * kFs) + 128;   // + one block of parameter latency
    for (int i = ramp; i < in.getNumSamples(); ++i)
        REQUIRE (out.getSample (0, i) == in.getSample (0, i));
}

TEST_CASE ("Input gain acts before the detector, output gain after it", "[dsp][gain]")
{
    const int n = 48000;

    SECTION ("no bands: pure gain, exact when settled")
    {
        Rig rig (kFs, 2, 0);
        rig.global ("inGain", 6.0f);
        rig.global ("outGain", -9.0f);
        rig.restart();
        const auto in = makeNoise (2, n, 31u, 0.25f);
        const auto out = rig.render (in, { 64, 500, 3 });
        const float gi = dsp::dbToLinear (6.0f), go = dsp::dbToLinear (-9.0f);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                REQUIRE (out.getSample (c, i) == (in.getSample (c, i) * gi) * go);   // prepare starts on the values
    }

    SECTION ("input gain shifts the level the compressor sees")
    {
        const double fc = digitalCentre (500.0, 2000.0, kFs);
        const auto in = sineBuffer (fc, kFs, n, 0.1);

        Rig a (kFs);
        a.band (1, "fLo", 500.0f); a.band (1, "fHi", 2000.0f);
        a.band (1, "thresh", -30.0f); a.band (1, "ratio", 4.0f); a.band (1, "knee", 0.0f);
        a.global ("inGain", 6.0f);
        a.restart();
        a.render (in);

        Rig b (kFs);
        b.band (1, "fLo", 500.0f); b.band (1, "fHi", 2000.0f);
        b.band (1, "thresh", -36.0f); b.band (1, "ratio", 4.0f); b.band (1, "knee", 0.0f);
        b.restart();
        b.render (in);

        // +6 dB into threshold T is the same over-threshold amount as 0 dB into T - 6.
        CHECK_THAT ((double) a.grDb (1), WithinAbs ((double) b.grDb (1), 0.01));
        CHECK (a.grDb (1) < -3.0f);
    }

    SECTION ("output gain does not change the gain reduction")
    {
        const auto in = makeNoise (1, n, 8u, 0.5f);
        Rig a (kFs); a.band (1, "thresh", -40.0f); a.restart();
        Rig b (kFs); b.band (1, "thresh", -40.0f); b.global ("outGain", 12.0f); b.restart();
        const auto outA = a.render (in);
        const auto outB = b.render (in);
        CHECK (a.grDb (1) == b.grDb (1));
        const float g = dsp::dbToLinear (12.0f);
        for (int i = 0; i < n; ++i)
            REQUIRE (outB.getSample (0, i) == outA.getSample (0, i) * g);
    }
}

TEST_CASE ("Global mix: 0 % is the gained input exactly, 50 % is the parallel blend", "[dsp][gain]")
{
    const auto in = makeNoise (2, 48000, 77u, 0.5f);
    const auto configure = [] (Rig& r, float mix)
    {
        for (int s = 1; s <= 3; ++s) { r.band (s, "thresh", -42.0f); r.band (s, "ratio", 6.0f); }
        r.global ("inGain", 3.0f);
        r.global ("mix", mix);
        r.restart();
    };

    Rig wet (kFs, 2, 3);  configure (wet, 100.0f);
    Rig half (kFs, 2, 3); configure (half, 50.0f);
    Rig dry (kFs, 2, 3);  configure (dry, 0.0f);
    const auto outWet = wet.render (in);
    const auto outHalf = half.render (in);
    const auto outDry = dry.render (in);

    const float gi = dsp::dbToLinear (3.0f);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < in.getNumSamples(); ++i)
            REQUIRE (outDry.getSample (c, i) == in.getSample (c, i) * gi);

    juce::AudioBuffer<float> expected (2, in.getNumSamples());
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < in.getNumSamples(); ++i)
        {
            const float d = in.getSample (c, i) * gi;
            expected.setSample (c, i, d + 0.5f * (outWet.getSample (c, i) - d));
        }
    CHECK (bitIdentical (outHalf, expected));
    CHECK_FALSE (bitIdentical (outWet, outDry));
    CHECK (half.grDb (2) == wet.grDb (2));       // the bands compress the same; only the blend differs
}

TEST_CASE ("Gain changes are ramped over 20 ms without steps", "[dsp][gain]")
{
    const int n = 9600;
    juce::AudioBuffer<float> dc (1, n);
    for (int i = 0; i < n; ++i) dc.setSample (0, i, 0.25f);
    const int rampLength = (int) std::lround (dsp::kGainRampSeconds * kFs);

    struct Case { const char* id; float from, to; bool band; };
    for (const Case& c : { Case { "outGain", 0.0f, 12.0f, false }, Case { "inGain", 0.0f, -18.0f, false },
                           Case { "outGain", 6.0f, -24.0f, false } })
    {
        INFO (c.id << " " << c.from << " -> " << c.to);
        Rig rig (kFs, 1, 0);
        rig.global (c.id, c.from);
        rig.restart();

        const int changeBlock = 10, block = 128;
        const auto out = rig.render (dc, { block }, false, [&] (int b, PluginProcessor& p)
        {
            if (b == changeBlock) setPlain (p, c.id, c.to);
        });

        const int start = changeBlock * block;
        const float before = out.getSample (0, start - 1);
        const float after = out.getSample (0, start + rampLength);
        CHECK (before != after);

        // Settled exactly after the ramp, constant before it.
        for (int i = 1; i < start; ++i)               REQUIRE (out.getSample (0, i) == before);
        for (int i = start + rampLength; i < n; ++i)  REQUIRE (out.getSample (0, i) == after);

        // Monotonic, and no step larger than a small multiple of the average step.
        const double average = std::abs ((double) after - before) / rampLength;
        double largest = 0.0;
        for (int i = start; i < start + rampLength; ++i)
        {
            const double d = (double) out.getSample (0, i) - out.getSample (0, i - 1);
            CHECK (d * ((double) after - before) >= 0.0);
            largest = std::max (largest, std::abs (d));
        }
        CHECK (largest < 6.0 * average);              // dB-linear ramp: steps scale with the level
        CHECK (largest > 0.0);
    }

    // Band makeup, band mix and global mix act on the band's change (output - input here, since
    // nothing compresses). A hard switch would put the full change on the first sample; a ramp
    // starts near zero and ends exactly on the new value.
    const double fc = digitalCentre (500.0, 2000.0, kFs);
    const auto in = sineBuffer (fc, kFs, 24000, 0.1);
    const int changeAt = 40 * 128;

    struct BandCase { const char* what; float makeupBefore, makeupAfter, bandMix, globalMixAfter; };
    for (const BandCase& c : { BandCase { "makeup 0 -> 12 dB", 0.0f, 12.0f, 100.0f, 100.0f },
                               BandCase { "makeup 12 -> 0 dB", 12.0f, 0.0f, 100.0f, 100.0f },
                               BandCase { "band mix 100 -> 0 %", 12.0f, 12.0f, 0.0f, 100.0f },
                               BandCase { "global mix 100 -> 0 %", 12.0f, 12.0f, 100.0f, 0.0f } })
    {
        INFO (c.what);
        Rig rig (kFs);
        rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f); rig.band (1, "thresh", 12.0f);
        rig.band (1, "makeup", c.makeupBefore);
        rig.restart();

        const auto steady = rig.render (in);            // what the output would be without the change
        rig.restart();
        const auto out = rig.render (in, { 128 }, false, [&c] (int b, PluginProcessor& p)
        {
            if (b != 40) return;
            setBand (p, 1, "makeup", c.makeupAfter);
            setBand (p, 1, "mix", c.bandMix);
            setPlain (p, "mix", c.globalMixAfter);
        });

        double early = 0.0, late = 0.0;
        for (int i = changeAt; i < changeAt + 48; ++i)              // first millisecond
            early = std::max (early, std::abs ((double) out.getSample (0, i) - steady.getSample (0, i)));
        for (int i = 20000; i < 24000; ++i)
            late = std::max (late, std::abs ((double) out.getSample (0, i) - steady.getSample (0, i)));

        CHECK (late > 0.05);                 // the change is large once settled ...
        CHECK (early < 0.12 * late);         // ... and has barely started after 1 ms of a 20 ms ramp
        for (int i = 0; i < changeAt; ++i)   // nothing moves before the change
            REQUIRE (out.getSample (0, i) == steady.getSample (0, i));

        if (c.bandMix == 0.0f || c.globalMixAfter == 0.0f)
            for (int i = changeAt + rampLength; i < 24000; ++i)     // fully dry: exactly the input
                REQUIRE (out.getSample (0, i) == in.getSample (0, i));
    }
}

TEST_CASE ("Bypass returns the input exactly even with non-unity gains", "[dsp][gain]")
{
    const auto in = makeNoise (2, 9600, 41u, 0.5f);
    const int settle = (int) std::lround (dsp::kGainRampSeconds * kFs) + 256;

    SECTION ("host bypass")
    {
        Rig rig (kFs, 2, 2);
        rig.band (1, "thresh", -40.0f); rig.band (1, "makeup", 6.0f);
        rig.global ("inGain", 9.0f); rig.global ("outGain", -4.0f); rig.global ("mix", 60.0f);
        rig.restart();
        CHECK_FALSE (bitIdentical (rig.render (in), in));

        const auto bypassed = rig.render (in, { 128 }, true);
        for (int c = 0; c < 2; ++c)
            for (int i = settle; i < in.getNumSamples(); ++i)
                REQUIRE (bypassed.getSample (c, i) == in.getSample (c, i));

        // No click going in: the first bypassed samples continue from the processed level.
        CHECK_FALSE (bypassed.getSample (0, 1) == in.getSample (0, 1));

        const auto back = rig.render (in);
        CHECK_FALSE (bitIdentical (back, in));
    }

    SECTION ("plugin bypass parameter")
    {
        Rig rig (kFs, 2, 1);
        rig.band (1, "thresh", -40.0f);
        rig.global ("inGain", -12.0f); rig.global ("outGain", 5.0f);
        rig.global ("bypass", 1.0f);
        rig.restart();
        rig.render (in);
        const auto out = rig.render (in);
        CHECK (bitIdentical (out, in));
    }
}

TEST_CASE ("Static gain settings are independent of the block layout", "[dsp][gain]")
{
    const auto in = makeNoise (2, 24000, 61u, 0.5f);
    const auto renderWith = [&in] (std::vector<int> blocks)
    {
        Rig rig (kFs, 2, 3);
        for (int s = 1; s <= 3; ++s)
        {
            rig.band (s, "thresh", -40.0f);
            rig.band (s, "makeup", 2.0f * (float) s);
            rig.band (s, "mix", 100.0f - 20.0f * (float) s);
        }
        rig.global ("inGain", 4.0f); rig.global ("outGain", -3.0f); rig.global ("mix", 80.0f);
        rig.restart();
        return rig.render (in, std::move (blocks));
    };

    const auto reference = renderWith ({ 512 });
    CHECK (bitIdentical (reference, renderWith ({ 1 })));
    CHECK (bitIdentical (reference, renderWith ({ 64, 7, 300, 255, 257 })));
    CHECK_FALSE (bitIdentical (reference, in));
}
