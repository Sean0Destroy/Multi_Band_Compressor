// Per-band character: the Warm FET curve on each band's own component after its gain.
// Off (0 %) changes nothing; on, it adds harmonics of what is in the band, leaves what is outside
// the band alone, has no latency, and switching it is click-free and allocation-free.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>

#include "dsp/DspConstants.h"
#include "support/AllocationCounter.h"
#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;
using Kind = dsp::EngineKind;

namespace
{
constexpr double kFs = 48000.0;

struct Rig
{
    std::unique_ptr<PluginProcessor> proc = std::make_unique<PluginProcessor>();
    OfflineRenderer renderer { *proc };
    int channels;

    Rig (int numChannels, Kind kind, float fLo = 60.0f, float fHi = 300.0f) : channels (numChannels)
    {
        proc->getBandSlots().createBand();
        proc->setEngineKind (kind);
        setBand (*proc, 1, "fLo", fLo);
        setBand (*proc, 1, "fHi", fHi);
        setBand (*proc, 1, "thresh", 0.0f);         // no compression: the character alone
        restart();
    }
    void restart() { REQUIRE (renderer.prepare (kFs, 512, { channels, 0 })); }
    juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& in, std::vector<int> blocks = { 128 },
                                     bool bypassed = false, const OfflineRenderer::BlockHook& hook = {})
    {
        return renderer.render (in, nullptr, blocks, bypassed, hook);
    }
};

juce::AudioBuffer<float> tones (int channels, int n, std::initializer_list<std::pair<double, double>> parts)
{
    juce::AudioBuffer<float> b (channels, n);
    b.clear();
    for (const auto& [f, a] : parts)
    {
        const auto s = sine (f, kFs, n, a);
        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < n; ++i)
                b.addSample (c, i, s[(size_t) i]);
    }
    return b;
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

double dB (double ratio) { return 20.0 * std::log10 (ratio + 1.0e-30); }

const char* name (Kind k) { return k == Kind::APrime ? "A-prime" : "A3"; }
}

TEST_CASE ("Band character: off is bit-identical, and the band's harmonics appear when it is on", "[dsp][bandcharacter]")
{
    const int n = 36000, start = 12000, length = 24000;          // 2 Hz bins
    const auto in = tones (2, n, { { 100.0, 0.7 }, { 5000.0, 0.1 } });

    for (Kind k : { Kind::A3, Kind::APrime })
    {
        INFO (name (k));
        Rig off (2, k), on (2, k);
        setBand (*on.proc, 1, "character", 100.0f);
        on.restart();
        CHECK (on.proc->getLatencySamples() == 0);

        Rig plain (2, k);
        const auto reference = plain.render (in);
        const auto a = off.render (in);
        CHECK (bitIdentical (a, reference));

        const auto b = on.render (in);
        const double fundamental = amplitudeAt (b, 0, start, length, 100.0);
        const double second = amplitudeAt (b, 0, start, length, 200.0);
        const double third = amplitudeAt (b, 0, start, length, 300.0);
        INFO ("2nd " << dB (second / fundamental) << " dB, 3rd " << dB (third / fundamental) << " dB");
        CHECK (dB (second / fundamental) > -40.0);              // even harmonics from the asymmetry
        CHECK (dB (third / fundamental) > -50.0);
        CHECK (dB (amplitudeAt (a, 0, start, length, 200.0) / fundamental) < -100.0);   // none when off

        // The tone outside the band passes as it did (within 0.1 dB), with no new component
        // between it and the band's harmonics that it would make if it were saturated too.
        const double outside = amplitudeAt (b, 0, start, length, 5000.0), outsideRef = amplitudeAt (a, 0, start, length, 5000.0);
        CHECK (std::abs (dB (outside / outsideRef)) < 0.1);
        CHECK (dB (amplitudeAt (b, 0, start, length, 10000.0) / outside) < -60.0);

        // A quiet signal in the band is (almost) untouched: the curve has unity small-signal gain.
        const auto quietIn = tones (2, n, { { 100.0, 0.001 } });
        Rig quietRig (2, k);
        setBand (*quietRig.proc, 1, "character", 100.0f);
        quietRig.restart();
        const auto quiet = quietRig.render (quietIn);
        const auto quietRef = plain.render (quietIn);
        double worst = 0.0;
        for (int i = start; i < n; ++i)
            worst = std::max (worst, (double) std::abs (quiet.getSample (0, i) - quietRef.getSample (0, i)));
        CHECK (dB (worst / 0.001) < -50.0);
    }
}

TEST_CASE ("Band character: on a Mid band the side is untouched", "[dsp][bandcharacter]")
{
    const int n = 24000;
    juce::AudioBuffer<float> in (2, n);
    const auto l = sine (100.0, kFs, n, 0.7), r = sine (150.0, kFs, n, 0.3);
    for (int i = 0; i < n; ++i) { in.setSample (0, i, l[(size_t) i]); in.setSample (1, i, r[(size_t) i]); }

    for (Kind k : { Kind::A3, Kind::APrime })
    {
        INFO (name (k));
        Rig rig (2, k);
        setBand (*rig.proc, 1, "stereoMode", 2.0f);             // Mid
        setBand (*rig.proc, 1, "character", 100.0f);
        rig.restart();
        const auto out = rig.render (in);
        double sideError = 0.0, midChange = 0.0;
        for (int i = 4800; i < n; ++i)
        {
            const double sIn = 0.5 * ((double) in.getSample (0, i) - in.getSample (1, i));
            const double sOut = 0.5 * ((double) out.getSample (0, i) - out.getSample (1, i));
            const double mIn = 0.5 * ((double) in.getSample (0, i) + in.getSample (1, i));
            const double mOut = 0.5 * ((double) out.getSample (0, i) + out.getSample (1, i));
            sideError = std::max (sideError, std::abs (sOut - sIn));
            midChange = std::max (midChange, std::abs (mOut - mIn));
        }
        CHECK (sideError < 1.0e-6);
        CHECK (midChange > 1.0e-3);
    }
}

TEST_CASE ("Band character: switching is click-free, finite and allocation-free, and it stops cleanly", "[dsp][bandcharacter][transition]")
{
    const int n = 72000;
    for (Kind k : { Kind::A3, Kind::APrime })
        for (int channels : { 1, 2 })
        {
            INFO (name (k) << ", channels " << channels);
            const auto in = tones (channels, n, { { 110.0, 0.6 }, { 2000.0, 0.05 } });

            Rig off (channels, k), on (channels, k);
            setBand (*on.proc, 1, "character", 100.0f);
            on.restart();
            const auto refOff = off.render (in), refOn = on.render (in);

            Rig rig (channels, k);
            test::resetAllocationCount();
            const auto out = rig.render (in, { 128 }, false, [] (int b, PluginProcessor& p)
            {
                if (b == 60)  setBand (p, 1, "character", 100.0f);
                if (b == 160) setBand (p, 1, "character", 0.0f);
            });
            CHECK (test::allocationCount() == 0);

            double worstExtra = 0.0;
            int worstAt = 0;
            for (int c = 0; c < channels; ++c)
                for (int i = 1; i < n; ++i)
                {
                    REQUIRE (std::isfinite (out.getSample (c, i)));
                    const double step = std::abs (out.getSample (c, i) - out.getSample (c, i - 1));
                    const double refStep = std::max (std::abs (refOff.getSample (c, i) - refOff.getSample (c, i - 1)),
                                                     std::abs (refOn.getSample (c, i) - refOn.getSample (c, i - 1)));
                    if (step - refStep > worstExtra) { worstExtra = step - refStep; worstAt = i; }
                }
            INFO ("largest extra step " << worstExtra << " at sample " << worstAt);
            CHECK (worstExtra < 0.002);

            // Before it is switched on, and once it is off again and its tail has gone, the output
            // is the plain band's, bit for bit (one band: nothing downstream remembers it).
            for (int c = 0; c < channels; ++c)
            {
                for (int i = 0; i < 60 * 128; ++i)
                    REQUIRE (out.getSample (c, i) == refOff.getSample (c, i));
                for (int i = 480 * 128; i < n; ++i)
                    REQUIRE (out.getSample (c, i) == refOff.getSample (c, i));
            }
        }
}

TEST_CASE ("Band character: host bypass and band bypass take it out; an engine switch keeps it clean", "[dsp][bandcharacter][transition]")
{
    const int n = 48000;
    const auto in = tones (2, n, { { 100.0, 0.7 } });

    Rig rig (2, Kind::APrime);
    setBand (*rig.proc, 1, "character", 100.0f);
    rig.restart();

    // Host bypass: after the fades the output is the input.
    const auto bypassed = rig.render (in, { 256 }, true);
    for (int c = 0; c < 2; ++c)
        for (int i = 9600; i < n; ++i)
            REQUIRE (bypassed.getSample (c, i) == in.getSample (c, i));

    // Band bypass: the character goes with the band.
    rig.restart();
    setBand (*rig.proc, 1, "bypass", 1.0f);
    const auto bandOff = rig.render (in);
    double worst = 0.0;
    for (int i = 24000; i < n; ++i) worst = std::max (worst, (double) std::abs (bandOff.getSample (0, i) - in.getSample (0, i)));
    CHECK (worst < 1.0e-6);
    setBand (*rig.proc, 1, "bypass", 0.0f);

    // Engine switch while coloured: finite, bounded, no allocations.
    rig.restart();
    test::resetAllocationCount();
    const auto out = rig.render (in, { 128 }, false, [] (int b, PluginProcessor& p)
    {
        if (b == 100) p.setEngineKind (Kind::A3);
        if (b == 250) p.setEngineKind (Kind::APrime);
    });
    CHECK (test::allocationCount() == 0);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
            REQUIRE (std::isfinite (out.getSample (c, i)));
    CHECK (out.getMagnitude (0, n) < 1.5f);
}
