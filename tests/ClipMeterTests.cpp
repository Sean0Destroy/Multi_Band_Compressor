// The output clipper's meter (dsp::ClipMeter): what goes in and out of the clipper, column by
// column, for the editor's clip scope and "Shaved" readout. Lock-free, no allocation.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cmath>
#include <memory>

#include "dsp/ClipMeter.h"
#include "support/AllocationCounter.h"
#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kFs = 48000.0;

struct ClipRig
{
    std::unique_ptr<PluginProcessor> proc = std::make_unique<PluginProcessor>();
    OfflineRenderer renderer { *proc };
    std::uint32_t cursor = 0;

    ClipRig() { REQUIRE (renderer.prepare (kFs, 512, { 2, 0 })); }

    juce::AudioBuffer<float> run (double amplitude, int n = 24000)
    {
        juce::AudioBuffer<float> b (2, n);
        const auto s = sine (997.0, kFs, n, amplitude);
        for (int c = 0; c < 2; ++c) b.copyFrom (c, 0, s.data(), n);
        resetAllocationCount();
        auto out = renderer.render (b, nullptr, { 256, 101, 512 }, false, {});
        CHECK (allocationCount() == 0);
        return out;
    }

    /** The newest `count` columns (fewer if fewer were written). */
    std::vector<dsp::ClipMeter::Column> latest (int count)
    {
        std::array<dsp::ClipMeter::Column, dsp::ClipMeter::kRingSize> buf {};
        const int got = proc->getClipMeter().readColumns (cursor, buf.data(), (int) buf.size());
        const int first = std::max (0, got - count);
        return { buf.begin() + first, buf.begin() + got };
    }
};

float peakOf (const juce::AudioBuffer<float>& b, int start)
{
    float m = 0.0f;
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = start; i < b.getNumSamples(); ++i)
            m = std::max (m, std::abs (b.getSample (c, i)));
    return m;
}
}

TEST_CASE ("Clip meter: columns of about 1/120 s, read once, no tearing on a lapped reader", "[clip][meter]")
{
    dsp::ClipMeter m;
    m.prepare (kFs);
    CHECK (m.getColumnLength() == 400);

    for (int i = 0; i < 10; ++i) m.add (0.5f, 0.25f, true, 200);   // 2000 samples = 5 columns
    std::uint32_t cursor = 0;
    std::array<dsp::ClipMeter::Column, 32> buf {};
    REQUIRE (m.readColumns (cursor, buf.data(), 32) == 5);
    CHECK_THAT (buf[0].shavedDb(), WithinAbs (6.0206, 1e-3));
    CHECK (m.readColumns (cursor, buf.data(), 32) == 0);           // nothing new

    // A reader that fell far behind only gets the newest columns.
    for (int i = 0; i < 3000; ++i) m.add (0.1f, 0.1f, false, 400);
    const int got = m.readColumns (cursor, buf.data(), 32);
    CHECK (got == 32);
    CHECK_FALSE (buf[31].clipperOn);
    CHECK (buf[31].shavedDb() == 0.0f);

    // Non-finite input never reaches the reader.
    m.add (std::numeric_limits<float>::quiet_NaN(), 0.1f, true, 400);
    REQUIRE (m.readColumns (cursor, buf.data(), 32) == 1);
    CHECK (buf[0].inPeak == 0.0f);
}

TEST_CASE ("Clip meter: shaved dB is what the clipper takes off the peak", "[clip][meter]")
{
    for (float os : { 0.0f, 2.0f })
        for (float mode : { 1.0f, 2.0f })   // Soft, Hard
        {
            CAPTURE (os, mode);
            ClipRig rig;
            setPlain (*rig.proc, "clipMode", mode);
            setPlain (*rig.proc, "clipCeiling", -6.0f);
            setPlain (*rig.proc, "clipOversampling", os);

            const auto out = rig.run (1.0);
            const auto cols = rig.latest (30);
            REQUIRE (cols.size() == 30);

            const float ceiling = std::pow (10.0f, -6.0f / 20.0f);
            for (const auto& c : cols)
            {
                CHECK (c.clipperOn);
                CHECK (c.outPeak <= ceiling + 1e-5f);
                CHECK (c.inPeak > ceiling);
                CHECK (c.shavedDb() > 0.5f);
                CHECK_THAT (c.shavedDb(), WithinAbs (20.0f * std::log10 (c.inPeak / c.outPeak), 1e-4));
            }
            // The meter's output is the plugin's output (the clipper is the last stage).
            CHECK_THAT (cols.back().outPeak, WithinAbs (peakOf (out, out.getNumSamples() - 2000), 0.02));
        }
}

TEST_CASE ("Clip meter: nothing shaved below the hard ceiling, and none with the clipper off", "[clip][meter]")
{
    {
        ClipRig rig;
        setPlain (*rig.proc, "clipMode", 2.0f);
        setPlain (*rig.proc, "clipCeiling", -0.3f);
        rig.run (0.25);
        for (const auto& c : rig.latest (20))
        {
            CHECK (c.clipperOn);
            CHECK (c.shavedDb() == 0.0f);
        }
    }
    {
        ClipRig rig;
        setPlain (*rig.proc, "clipMode", 0.0f);
        const auto out = rig.run (1.0);
        const auto cols = rig.latest (20);
        REQUIRE (cols.size() == 20);
        for (const auto& c : cols)
        {
            CHECK_FALSE (c.clipperOn);
            CHECK (c.shavedDb() == 0.0f);
            CHECK (c.inPeak == c.outPeak);
            CHECK (c.inPeak > 0.1f);
        }
    }
}
