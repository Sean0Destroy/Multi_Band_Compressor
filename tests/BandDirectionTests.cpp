// Per-band dynamics direction: Down compresses above the threshold (the default, unchanged);
// Up expands upward above the threshold by the mirror image of what Down would cut, capped by
// the same Range.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>

#include "ParameterSchema.h"
#include "dsp/DspConstants.h"
#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kFs = 48000.0;

/** Steady-state gain (dB) of one band on a 1 kHz tone of `levelDb` (peak, dBFS) in a
    500 Hz .. 2 kHz band with a hard knee. */
double bandGainDb (bool up, float thresholdDb, float ratio, float rangeDb, double levelDb)
{
    auto proc = std::make_unique<PluginProcessor>();
    OfflineRenderer renderer { *proc };
    proc->getBandSlots().createBand();
    setBand (*proc, 1, "fLo", 500.0f);
    setBand (*proc, 1, "fHi", 2000.0f);
    setBand (*proc, 1, "thresh", thresholdDb);
    setBand (*proc, 1, "ratio", ratio);
    setBand (*proc, 1, "knee", 0.0f);
    setBand (*proc, 1, "range", rangeDb);
    setBand (*proc, 1, "attack", 1.0f);
    setBand (*proc, 1, "release", 50.0f);
    setBand (*proc, 1, schema::band::dynDir, up ? 1.0f : 0.0f);
    REQUIRE (renderer.prepare (kFs, 512, { 1, 0 }));

    const int n = 48000, start = 24000, length = 24000;
    juce::AudioBuffer<float> in (1, n);
    const auto s = sine (1000.0, kFs, n, std::pow (10.0, levelDb / 20.0));
    for (int i = 0; i < n; ++i) in.setSample (0, i, s[(size_t) i]);
    const auto out = renderer.render (in, nullptr, { 128 }, false, {});

    double ei = 0.0, eo = 0.0;
    for (int i = start; i < start + length; ++i)
    {
        ei += (double) in.getSample (0, i) * in.getSample (0, i);
        eo += (double) out.getSample (0, i) * out.getSample (0, i);
    }
    return 10.0 * std::log10 (eo / ei);
}
}

TEST_CASE ("Band direction: Up is the mirror image of Down, capped by the same Range", "[dsp][direction]")
{
    // 12 dB over a -30 dB threshold at 4:1 asks for 9 dB; Range 24 does not limit it.
    const double down = bandGainDb (false, -30.0f, 4.0f, 24.0f, -18.0);
    const double up   = bandGainDb (true,  -30.0f, 4.0f, 24.0f, -18.0);
    CHECK (down < -6.0);
    CHECK (up > 6.0);
    CHECK_THAT (up, WithinAbs (-down, 0.05));

    // Range caps both directions at the same size.
    CHECK_THAT (bandGainDb (false, -30.0f, 4.0f, 3.0f, -18.0), WithinAbs (-3.0, 0.15));
    CHECK_THAT (bandGainDb (true,  -30.0f, 4.0f, 3.0f, -18.0), WithinAbs (3.0, 0.15));

    // Below the threshold neither direction does anything.
    CHECK_THAT (bandGainDb (true, -10.0f, 4.0f, 24.0f, -30.0), WithinAbs (0.0, 0.01));
}

TEST_CASE ("Band direction: the parameter defaults to Down and old projects load Down", "[ids][direction]")
{
    const auto& specs = schema::allParams();
    for (const auto& s : specs)
        if (s.id == schema::bandParamId (1, schema::band::dynDir))
        {
            CHECK (s.kind == schema::Kind::Choice);
            CHECK (s.defaultValue == 0.0f);
            CHECK (s.choices == std::vector<std::string> { "Down", "Up" });
            CHECK (s.versionHint == 2);
        }
}
