// Overlap mode: Stack (band gains add up) and Blend (each band's gain is reduced by its
// neighbours' skirts at its centre, so stacked bands land near their own command).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <cstdio>
#include <memory>

#include "dsp/A3RegionEngine.h"
#include "dsp/APrimeRegionEngine.h"
#include "support/AllocationCounter.h"
#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;
using Catch::Matchers::WithinAbs;
using Kind = dsp::EngineKind;

namespace
{
constexpr double kFs = 48000.0;

std::unique_ptr<dsp::IBandEngine> makeEngine (Kind kind)
{
    std::unique_ptr<dsp::IBandEngine> e;
    if (kind == Kind::APrime) e = std::make_unique<dsp::APrimeRegionEngine>();
    else                      e = std::make_unique<dsp::A3RegionEngine>();
    e->prepare (kFs, 512);
    return e;
}

/** Impulse response with fixed band gains (the measurement hook), after `settle` samples. */
std::vector<float> response (Kind kind, int overlapMode, const std::vector<RegionSpec>& regions, int length = 1 << 15)
{
    auto engine = makeEngine (kind);
    dsp::EngineSettings s;
    s.overlapMode = overlapMode;
    std::vector<float> gains (regions.size());
    dsp::ExternalGains ext;
    for (size_t i = 0; i < regions.size(); ++i)
    {
        auto& b = s.bands[i];
        b.fLoHz = (float) regions[i].fLo; b.fHiHz = (float) regions[i].fHi; b.thresholdDb = 12.0f;
        s.usedMask |= 1u << i;
        gains[i] = (float) dbToGain (regions[i].gainDb);
        ext.data[i] = &gains[i];
    }
    engine->setSettings (s);
    engine->setExternalGains (&ext);
    std::vector<float> silence (2 * 48000, 0.0f);
    float* quiet[1] { silence.data() };
    engine->process (quiet, 1, (int) silence.size());
    std::vector<float> x ((size_t) length, 0.0f);
    x[0] = 1.0f;
    float* chans[1] { x.data() };
    engine->process (chans, 1, length);
    return x;
}

struct Range { double lo = 0.0, hi = -1000.0; };
Range rangeDb (const std::vector<float>& h, double fLo, double fHi)
{
    const MeasuredResponse m (h, kFs);
    Range r { 1000.0, -1000.0 };
    for (double f = fLo; f <= fHi; f *= 1.01)
    {
        const double d = magDb (m.exact (f));
        r.lo = std::min (r.lo, d);
        r.hi = std::max (r.hi, d);
    }
    return r;
}

std::vector<RegionSpec> fourBands (double gainDb)
{
    return { { 120, 400, gainDb, 1 }, { 400, 1000, gainDb, 2 }, { 1000, 2500, gainDb, 3 }, { 2500, 5000, gainDb, 4 } };
}
}

TEST_CASE ("Overlap Blend: stacked bands land on their own command, never deeper", "[dsp][overlap]")
{
    for (Kind k : { Kind::A3, Kind::APrime })
        for (double g : { -3.0, -6.0, -12.0, -24.0 })
        {
            INFO ((k == Kind::APrime ? "A-prime" : "A3") << ", four adjacent bands at " << g << " dB");
            const auto stack = rangeDb (response (k, 0, fourBands (g)), 200.0, 3500.0);
            const auto blend = rangeDb (response (k, 1, fourBands (g)), 200.0, 3500.0);
            INFO ("Stack " << stack.lo << " .. " << stack.hi << " dB, Blend " << blend.lo << " .. " << blend.hi << " dB");
            CHECK (stack.lo < g - 0.5);                                  // the problem: deeper than commanded
            CHECK (blend.lo > g - 0.1);                                  // Blend: never deeper than the command
            CHECK (blend.hi - blend.lo < stack.hi - stack.lo);           // and flatter

            // At each band centre the response is the command.
            const MeasuredResponse m (response (k, 1, fourBands (g)), kFs);
            for (const auto& b : fourBands (g))
                CHECK_THAT (magDb (m.exact (digitalCentre (b.fLo, b.fHi, kFs))), WithinAbs (g, 0.1));
        }

    // Boosts likewise (makeup on adjacent bands).
    for (Kind k : { Kind::A3, Kind::APrime })
    {
        const auto blend = rangeDb (response (k, 1, fourBands (6.0)), 200.0, 3500.0);
        CHECK (blend.hi < 6.0 + 0.15);
    }
}

TEST_CASE ("Overlap Blend: no boost from a cut, overlapping bands share, mixed signs unchanged", "[dsp][overlap]")
{
    for (Kind k : { Kind::A3, Kind::APrime })
    {
        INFO ((k == Kind::APrime ? "A-prime" : "A3"));
        // A band that is not compressing next to one that is: never above unity anywhere.
        const auto r = rangeDb (response (k, 1, { { 250, 1000, -12, 1 }, { 1000, 4000, 0, 2 } }), 20.0, 20000.0);
        CHECK (r.hi < 1.0e-3);
        CHECK_THAT (r.lo, WithinAbs (-12.0, 0.05));

        // Two bands on the same range: the deeper command wins instead of the sum.
        const MeasuredResponse same (response (k, 1, { { 500, 2000, -6, 1 }, { 500, 2000, -6, 2 } }), kFs);
        CHECK_THAT (magDb (same.exact (digitalCentre (500, 2000, kFs))), WithinAbs (-6.0, 0.05));
        const MeasuredResponse stacked (response (k, 0, { { 500, 2000, -6, 1 }, { 500, 2000, -6, 2 } }), kFs);
        CHECK_THAT (magDb (stacked.exact (digitalCentre (500, 2000, kFs))), WithinAbs (-12.0, 0.05));

        // A cut next to a boost: neither is changed (a cut is never deepened, a boost never raised).
        const auto mixedStack = response (k, 0, { { 250, 1000, -6, 1 }, { 1000, 4000, 6, 2 } });
        const auto mixedBlend = response (k, 1, { { 250, 1000, -6, 1 }, { 1000, 4000, 6, 2 } });
        double diff = 0.0;
        for (size_t i = 0; i < mixedStack.size(); ++i) diff = std::max (diff, (double) std::abs (mixedStack[i] - mixedBlend[i]));
        CHECK (diff < 1.0e-6);
    }
}

namespace
{
struct Rig
{
    std::unique_ptr<PluginProcessor> proc = std::make_unique<PluginProcessor>();
    OfflineRenderer renderer { *proc };
    int channels;

    Rig (int numChannels, int bands) : channels (numChannels)
    {
        for (int i = 0; i < bands; ++i) proc->getBandSlots().createBand();
        REQUIRE (renderer.prepare (kFs, 512, { channels, 0 }));
    }
    void restart() { REQUIRE (renderer.prepare (kFs, 512, { channels, 0 })); }
    juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& in, std::vector<int> blocks = { 128 },
                                     bool bypassed = false, const OfflineRenderer::BlockHook& hook = {})
    {
        return renderer.render (in, nullptr, blocks, bypassed, hook);
    }
};

void compressAll (PluginProcessor& p, int bands)
{
    for (int b = 1; b <= bands; ++b)
    {
        setBand (p, b, "thresh", -40.0f);
        setBand (p, b, "ratio", 4.0f);
        setBand (p, b, "attack", 5.0f);
        setBand (p, b, "release", 80.0f);
    }
}
}

TEST_CASE ("Overlap Blend: one band is untouched, and Stack is the default", "[dsp][overlap]")
{
    const auto in = makeNoise (2, 24000, 11u, 0.5f);
    Rig stack (2, 1), blend (2, 1);
    compressAll (*stack.proc, 1);
    compressAll (*blend.proc, 1);
    CHECK ((int) blend.proc->getState().getRawParameterValue ("overlapMode")->load() == 0);
    setPlain (*blend.proc, "overlapMode", 1.0f);
    stack.restart(); blend.restart();
    const auto a = stack.render (in), b = blend.render (in);
    CHECK (bitIdentical (a, b));                    // nothing to share with: no change at all
}

TEST_CASE ("Overlap Blend: switching while compressing is click-free, finite and allocation-free", "[dsp][overlap][transition]")
{
    const int n = 48000;
    for (Kind k : { Kind::A3, Kind::APrime })
        for (int channels : { 1, 2 })
        {
            INFO ((k == Kind::APrime ? "A-prime" : "A3") << ", channels " << channels);
            Rig rig (channels, 8);
            rig.proc->setEngineKind (k);
            compressAll (*rig.proc, 8);
            if (channels == 2)
            {
                setBand (*rig.proc, 3, "stereoMode", 4.0f);           // one band in mid/side
                setBand (*rig.proc, 5, "link", 0.0f);
            }
            rig.restart();

            // A low tone (no fast changes of its own) plus quiet noise to keep every band compressing.
            juce::AudioBuffer<float> in (channels, n);
            const auto tone = sine (150.0, kFs, n, 0.5);
            const auto noise = makeNoise (channels, n, 3u, 0.05f);
            for (int c = 0; c < channels; ++c)
                for (int i = 0; i < n; ++i)
                    in.setSample (c, i, (float) tone[(size_t) i] + noise.getSample (c, i));

            const auto reference = rig.render (in);                   // Stack throughout
            setPlain (*rig.proc, "overlapMode", 1.0f);
            rig.restart();
            const auto blendReference = rig.render (in);              // Blend throughout
            setPlain (*rig.proc, "overlapMode", 0.0f);
            rig.restart();
            test::resetAllocationCount();
            const auto out = rig.render (in, { 128 }, false, [] (int b, PluginProcessor& p)
            {
                if (b == 100) setPlain (p, "overlapMode", 1.0f);
                if (b == 250) setPlain (p, "overlapMode", 0.0f);
                if (b == 300) setPlain (p, "overlapMode", 1.0f);
            });
            CHECK (test::allocationCount() == 0);

            // The change is spread over 20 ms: no sample-to-sample step larger than the larger of
            // the two modes' own steps there (plus a little), and the output stays finite.
            double worstExtra = 0.0;
            for (int c = 0; c < channels; ++c)
                for (int i = 1; i < n; ++i)
                {
                    REQUIRE (std::isfinite (out.getSample (c, i)));
                    const double step = std::abs (out.getSample (c, i) - out.getSample (c, i - 1));
                    const double refStep = std::max (std::abs (reference.getSample (c, i) - reference.getSample (c, i - 1)),
                                                     std::abs (blendReference.getSample (c, i) - blendReference.getSample (c, i - 1)));
                    worstExtra = std::max (worstExtra, step - refStep);
                }
            INFO ("largest extra step " << worstExtra);
            CHECK (worstExtra < 0.002);

            // Before the first switch the output is Stack's, bit for bit.
            for (int c = 0; c < channels; ++c)
                for (int i = 0; i < 100 * 128; ++i)
                    REQUIRE (out.getSample (c, i) == reference.getSample (c, i));

            // Blend is less deep than Stack once settled (adjacent bands compressing together).
            double eStack = 0.0, eBlend = 0.0;
            for (int i = 200 * 128; i < 250 * 128; ++i)
            {
                eStack += (double) reference.getSample (0, i) * reference.getSample (0, i);
                eBlend += (double) out.getSample (0, i) * out.getSample (0, i);
            }
            CHECK (eBlend > eStack);
        }
}

TEST_CASE ("Overlap Blend: an engine switch and host bypass while blending stay clean", "[dsp][overlap][transition]")
{
    const int n = 48000;
    Rig rig (2, 6);
    compressAll (*rig.proc, 6);
    setPlain (*rig.proc, "overlapMode", 1.0f);
    rig.restart();
    const auto in = makeNoise (2, n, 9u, 0.3f);

    test::resetAllocationCount();
    const auto out = rig.render (in, { 128 }, false, [] (int b, PluginProcessor& p)
    {
        if (b == 120) p.setEngineKind (Kind::A3);
        if (b == 240) p.setEngineKind (Kind::APrime);
    });
    CHECK (test::allocationCount() == 0);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
            REQUIRE (std::isfinite (out.getSample (c, i)));
    CHECK (out.getMagnitude (0, n) < 2.0f);

    // Host bypass: once the bypass fade is over, the output is the input.
    rig.restart();
    const auto bypassed = rig.render (in, { 256 }, true);
    const int latency = rig.proc->getLatencySamples();
    for (int c = 0; c < 2; ++c)
        for (int i = 4800; i < n; ++i)
            REQUIRE (bypassed.getSample (c, i) == in.getSample (c, i - latency));
}
