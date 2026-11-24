// Per-band Width: the band's filter reaches further (or less far) around its centre while the
// Low/High Freq parameters, the handles and the detector stay where they are set.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <memory>

#include "ParameterSchema.h"
#include "dsp/DspConstants.h"
#include "dsp/IBandEngine.h"
#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;
using Catch::Matchers::WithinAbs;
using Kind = dsp::EngineKind;

namespace
{
constexpr double kFs = 48000.0;

/** Steady-state gain (dB) a single band applies to a tone at `f`, with a static band gain of
    `makeupDb` and no compression, at the given Width. */
double bandGainDb (Kind kind, float widthPercent, double f, float makeupDb = 12.0f)
{
    auto proc = std::make_unique<PluginProcessor>();
    OfflineRenderer renderer { *proc };
    proc->getBandSlots().createBand();
    proc->setEngineKind (kind);
    setBand (*proc, 1, "fLo", 400.0f);
    setBand (*proc, 1, "fHi", 1600.0f);
    setBand (*proc, 1, "thresh", 0.0f);          // nothing reaches the threshold: makeup alone
    setBand (*proc, 1, "makeup", makeupDb);
    setBand (*proc, 1, schema::band::width, widthPercent);
    REQUIRE (renderer.prepare (kFs, 512, { 1, 0 }));

    const int n = 48000, start = 24000, length = 24000;     // settled half; 2 Hz bins
    juce::AudioBuffer<float> in (1, n);
    const auto s = sine (f, kFs, n, 0.05);
    for (int i = 0; i < n; ++i) in.setSample (0, i, s[(size_t) i]);

    const auto out = renderer.render (in, nullptr, { 128 }, false, {});
    double ri = 0.0, ii = 0.0, ro = 0.0, io = 0.0;
    for (int i = 0; i < length; ++i)
    {
        const double ph = 2.0 * dsp::kPi * f * (double) i / kFs;
        ri += in.getSample (0, start + i) * std::cos (ph);   ii -= in.getSample (0, start + i) * std::sin (ph);
        ro += out.getSample (0, start + i) * std::cos (ph);  io -= out.getSample (0, start + i) * std::sin (ph);
    }
    return 20.0 * std::log10 (std::sqrt (ro * ro + io * io) / std::sqrt (ri * ri + ii * ii));
}

const char* name (Kind k) { return k == Kind::APrime ? "A-prime" : "A3"; }
}

TEST_CASE ("Band width: the edges scale about the band's log-centre", "[dsp][bandwidth]")
{
    // 100 % hands the edges back untouched (bit-identical to a project without Width).
    const auto same = dsp::widenedEdges (123.456f, 7890.12f, 100.0f);
    CHECK (same.loHz == 123.456f);
    CHECK (same.hiHz == 7890.12f);

    // 400..1600 Hz is two octaves around 800 Hz.
    const auto wide = dsp::widenedEdges (400.0f, 1600.0f, 200.0f);
    CHECK_THAT (wide.loHz, WithinAbs (200.0, 0.01));
    CHECK_THAT (wide.hiHz, WithinAbs (3200.0, 0.05));

    const auto narrow = dsp::widenedEdges (400.0f, 1600.0f, 50.0f);
    CHECK_THAT (narrow.loHz, WithinAbs (565.685, 0.01));
    CHECK_THAT (narrow.hiHz, WithinAbs (1131.371, 0.01));

    // Swapped edges are sorted; the range is clamped to the parameter's 25..400 %.
    const auto swapped = dsp::widenedEdges (1600.0f, 400.0f, 200.0f);
    CHECK_THAT (swapped.loHz, WithinAbs (200.0, 0.01));
    const auto clamped = dsp::widenedEdges (400.0f, 1600.0f, 10000.0f);
    CHECK_THAT (clamped.loHz, WithinAbs (50.0, 0.01));      // 400 %: 8 octaves around 800 Hz
}

TEST_CASE ("Band width: the parameter is appended in schema v2 and defaults to 100 %", "[ids][bandwidth]")
{
    const auto& specs = schema::allParams();
    const auto it = std::find_if (specs.begin(), specs.end(), [] (const auto& s) { return s.id == schema::bandParamId (1, schema::band::width); });
    REQUIRE (it != specs.end());
    CHECK (it->defaultValue == 100.0f);
    CHECK (it->minValue == 25.0f);
    CHECK (it->maxValue == 400.0f);
    CHECK (it->versionHint == 2);

    // Every schema v1 parameter keeps version hint 1 (Width, Direction and Low Latency are v2).
    for (const auto& s : specs)
        if (s.id.find (schema::band::width) == std::string::npos && s.id.find (schema::band::dynDir) == std::string::npos
            && s.id != schema::global::lowLatency)
            CHECK (s.versionHint == 1);
}

TEST_CASE ("Band width: wider reaches further out, narrower less, the centre stays", "[dsp][bandwidth]")
{
    for (auto kind : { Kind::APrime, Kind::A3 })
    {
        INFO (name (kind));
        const double centre100 = bandGainDb (kind, 100.0f, 800.0);
        const double centre200 = bandGainDb (kind, 200.0f, 800.0);
        const double centre50  = bandGainDb (kind,  50.0f, 800.0);
        CHECK_THAT (centre100, WithinAbs (12.0, 1.0));
        CHECK_THAT (centre200, WithinAbs (12.0, 1.0));
        CHECK_THAT (centre50,  WithinAbs (12.0, 1.5));

        // An octave below the low edge: the wider band lifts it a lot more.
        const double out100 = bandGainDb (kind, 100.0f, 200.0);
        const double out200 = bandGainDb (kind, 200.0f, 200.0);
        CHECK (out200 > out100 + 3.0);

        // At the low edge itself: the narrower band lifts it less.
        const double edge100 = bandGainDb (kind, 100.0f, 400.0);
        const double edge50  = bandGainDb (kind,  50.0f, 400.0);
        CHECK (edge50 < edge100 - 2.0);
    }
}
