// Engine integration through the processor: neutral reconstruction, block-size independence,
// lifecycle ramps, metering, non-finite/denormal handling and real-time safety.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <random>

#include "support/AllocationCounter.h"
#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;
using Catch::Matchers::WithinAbs;

namespace
{
void allBands (PluginProcessor& p, const char* suffix, float v)
{
    for (int s = 1; s <= 8; ++s) setBand (p, s, suffix, v);
}

void createAll (PluginProcessor& p)
{
    for (int i = 0; i < 8; ++i) REQUIRE (p.getBandSlots().createBand() == i + 1);
}

double maxAbsDiff (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    double m = 0.0;
    for (int c = 0; c < a.getNumChannels(); ++c)
        for (int i = 0; i < a.getNumSamples(); ++i)
            m = std::max (m, (double) std::abs (a.getSample (c, i) - b.getSample (c, i)));
    return m;
}
} // namespace

TEST_CASE ("Neutral reconstruction: 8 used bands below threshold are bit-exact", "[dsp][engine][neutral]")
{
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        PluginProcessor p;
        createAll (p);
        allBands (p, "thresh", 12.0f);   // above the signal (and its knee)
        OfflineRenderer r (p);
        REQUIRE (r.prepare (fs, 1024, { 2, 0 }));
        const auto in = makeNoise (2, 20000, 31u, 0.5f);
        INFO ("fs " << fs);

        for (auto& seq : std::vector<std::vector<int>> { { 1 }, { 64 }, { 1024 }, { 1, 7, 33, 1024, 3, 511, 64 } })
            CHECK (bitIdentical (in, r.render (in, nullptr, seq)));

        // Also while edges, detector settings, bypass and the global bypass are automated per block,
        // including reversed and coincident edges and edges outside the runtime range.
        std::mt19937 rng (17);
        std::uniform_real_distribution<float> u (0.0f, 1.0f);
        const auto automated = r.render (in, nullptr, { 1, 64, 300, 17 }, false, [&] (int, PluginProcessor& proc)
        {
            for (int s = 1; s <= 8; ++s)
            {
                for (auto* id : { "fLo", "fHi", "ratio", "knee", "attack", "release", "range", "detMode", "detBlend", "rmsTime", "link", "bypass" })
                    if (u (rng) < 0.3f) proc.getState().getParameter (schema::bandParamId (s, id))->setValueNotifyingHost (u (rng));
            }
            if (u (rng) < 0.1f) proc.getState().getParameter ("bypass")->setValueNotifyingHost (u (rng) < 0.5f ? 0.0f : 1.0f);
        });
        CHECK (bitIdentical (in, automated));
    }
}

TEST_CASE ("Neutral reconstruction survives band create/delete between blocks", "[dsp][engine][neutral]")
{
    PluginProcessor p;
    OfflineRenderer r (p);
    REQUIRE (r.prepare (48000.0, 512, { 2, 0 }));
    const auto in = makeNoise (2, 40000, 8u, 0.5f);
    const auto out = r.render (in, nullptr, { 256 }, false, [] (int block, PluginProcessor& proc)
    {
        auto& slots = proc.getBandSlots();
        if (block % 5 == 0 && slots.usedSlots().size() < 8)
        {
            const int s = slots.createBand();
            setBand (proc, s, "thresh", 12.0f);
        }
        if (block % 7 == 3 && ! slots.usedSlots().empty()) slots.deleteBand (slots.usedSlots().front());
    });
    CHECK (bitIdentical (in, out));
}

TEST_CASE ("Host bypass (processBlockBypassed) is bit-exact while compressing", "[dsp][engine][neutral]")
{
    PluginProcessor p;
    createAll (p);
    allBands (p, "thresh", -40.0f);
    OfflineRenderer r (p);
    REQUIRE (r.prepare (48000.0, 512, { 2, 0 }));
    const auto in = makeNoise (2, 20000, 3u, 0.8f);
    CHECK (bitIdentical (in, r.render (in, nullptr, { 100 }, true)));
    CHECK_FALSE (bitIdentical (in, r.render (in, nullptr, { 100 }, false)));   // and it does compress otherwise
}

TEST_CASE ("Host bypass switches without a click: 10 ms ramp, then bit-exact", "[dsp][engine][neutral]")
{
    const double fs = 48000.0;
    PluginProcessor p;
    REQUIRE (p.getBandSlots().createBand() == 1);
    setBand (p, 1, "fLo", 500.0f);  setBand (p, 1, "fHi", 2000.0f);
    setBand (p, 1, "thresh", -60.0f); setBand (p, 1, "ratio", 100.0f); setBand (p, 1, "range", 24.0f);
    p.setRateAndBufferSizeDetails (fs, 128);
    p.prepareToPlay (fs, 128);

    const int n = 48000, block = 128;
    const auto x = sine (1000.0, fs, n, 0.5);
    juce::AudioBuffer<float> buf (2, n);
    buf.copyFrom (0, 0, x.data(), n);
    buf.copyFrom (1, 0, x.data(), n);
    const juce::AudioBuffer<float> in (buf);

    const int enter = 150 * block, leave = 300 * block;   // 24 dB GR fully engaged before each switch
    juce::MidiBuffer midi;
    for (int pos = 0; pos < n; pos += block)
    {
        juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), 2, pos, block);
        if (pos >= enter && pos < leave) p.processBlockBypassed (view, midi);
        else                             p.processBlock (view, midi);
    }

    double maxStepIn = 0.0, maxStepOut = 0.0;
    for (int i = 1; i < n; ++i)
    {
        maxStepIn = std::max (maxStepIn, (double) std::abs (x[(size_t) i] - x[(size_t) i - 1]));
        if (i > 4800)   // after the initial ramp-in
            maxStepOut = std::max (maxStepOut, (double) std::abs (buf.getSample (0, i) - buf.getSample (0, i - 1)));
    }
    INFO ("max |dy| " << maxStepOut << " vs input slew " << maxStepIn);
    CHECK (maxStepOut <= maxStepIn * 1.05);   // an instant switch from -24 dB GR gave 0.41 vs 0.065

    // Compressing before, exact pass-through after the 10 ms ramp, compressing again after leaving.
    CHECK (std::abs (buf.getSample (0, enter - 12)) < 0.1f);
    for (int c = 0; c < 2; ++c)
        for (int i = enter + 480; i < leave; ++i)
            REQUIRE (buf.getSample (c, i) == in.getSample (c, i));
    CHECK (buf.getSample (0, enter + 100) != in.getSample (0, enter + 100));   // still ramping
    double late = 0.0;
    for (int i = leave + 960; i < leave + 1920; ++i) late = std::max (late, (double) std::abs (buf.getSample (0, i)));
    CHECK (late < 0.1);
}

TEST_CASE ("Block-size independence", "[dsp][engine][blocksize]")
{
    for (double fs : { 44100.0, 96000.0 })
    {
        PluginProcessor p;
        createAll (p);
        allBands (p, "thresh", -30.0f);
        setBand (p, 3, "detMode", 1.0f);
        setBand (p, 5, "detMode", 2.0f);
        setBand (p, 2, "link", 30.0f);
        OfflineRenderer r (p);
        REQUIRE (r.prepare (fs, 1024, { 2, 0 }));
        const auto in = makeNoise (2, 30000, 99u, 0.6f);
        const auto reference = r.render (in, nullptr, { 512 });
        CHECK_FALSE (bitIdentical (in, reference));   // compression active
        REQUIRE (r.prepare (fs, 1024, { 2, 0 }));
        CHECK (bitIdentical (reference, r.render (in, nullptr, { 512 })));   // prepare resets everything

        INFO ("fs " << fs);
        for (auto& seq : std::vector<std::vector<int>> { { 1 }, { 16 }, { 1, 2, 3, 5, 8, 13, 21, 1000 }, { 1024 }, { 127, 129 }, { 15, 17 } })
        {
            REQUIRE (r.prepare (fs, 1024, { 2, 0 }));   // same start state for every layout
            CHECK (bitIdentical (reference, r.render (in, nullptr, seq)));
        }
    }
}

TEST_CASE ("Blocks larger than the prepared size are chunked internally", "[dsp][engine][blocksize]")
{
    PluginProcessor a, b;
    for (auto* p : { &a, &b }) { createAll (*p); allBands (*p, "thresh", -30.0f); }
    OfflineRenderer ra (a);
    REQUIRE (ra.prepare (48000.0, 4096, { 2, 0 }));
    const auto in = makeNoise (2, 4096 * 3, 12u, 0.6f);
    const auto reference = ra.render (in, nullptr, { 4096 });

    b.setRateAndBufferSizeDetails (48000.0, 64);
    b.prepareToPlay (48000.0, 64);              // host promised 64, then delivers 4096
    juce::AudioBuffer<float> buf (in);
    juce::MidiBuffer midi;
    for (int pos = 0; pos < in.getNumSamples(); pos += 4096)
    {
        juce::AudioBuffer<float> view (buf.getArrayOfWritePointers(), 2, pos, 4096);
        AudioCallbackScope scope;
        b.processBlock (view, midi);
    }
    CHECK (bitIdentical (reference, buf));
}

TEST_CASE ("Per-block automation: behaviour depends on the block layout (documented)", "[dsp][engine][blocksize]")
{
    // Parameters are read once per host block, so automated values take effect at block
    // boundaries: different block layouts give (small) differences. Measured and reported.
    std::vector<std::vector<double>> rows;
    juce::AudioBuffer<float> ref;
    const auto in = makeNoise (2, 48000, 41u, 0.6f);
    for (int block : { 32, 64, 128, 256, 512 })
    {
        PluginProcessor p;
        createAll (p);
        OfflineRenderer r (p);
        REQUIRE (r.prepare (48000.0, 512, { 2, 0 }));
        int pos = 0;
        const auto out = r.render (in, nullptr, { block }, false, [&] (int, PluginProcessor& proc)
        {
            // Same automation curve in time: threshold sweeps -40 .. 0 dB over 1 s.
            allBands (proc, "thresh", -40.0f + 40.0f * (float) pos / 48000.0f);
            pos += block;
        });
        if (block == 32) { ref = out; continue; }
        const double d = maxAbsDiff (ref, out);
        INFO ("block " << block << " max diff vs block 32: " << dbOf (d) << " dBFS");
        CHECK (d > 0.0);
        CHECK (dbOf (d) < -20.0);
        rows.push_back ({ (double) block, dbOf (d) });
    }
    measure::writeCsv ("automation_block_layout.csv", { "block_size", "max_abs_diff_vs_block32_dbfs" }, rows);
}

TEST_CASE ("Band lifecycle: 10 ms activity ramps for bypass, create and delete", "[dsp][engine][lifecycle]")
{
    const double fs = 48000.0;
    PluginProcessor p;
    REQUIRE (p.getBandSlots().createBand() == 1);
    setBand (p, 1, "thresh", -80.0f); setBand (p, 1, "ratio", 100.0f); setBand (p, 1, "range", 12.0f);
    setBand (p, 1, "fLo", 500.0f);    setBand (p, 1, "fHi", 2000.0f);  setBand (p, 1, "attack", 0.01f);
    OfflineRenderer r (p);
    REQUIRE (r.prepare (fs, 512, { 1, 0 }));

    const int n = 48000;
    juce::AudioBuffer<float> in (1, n);
    const auto x = sine (1000.0, fs, n, 0.5);
    in.copyFrom (0, 0, x.data(), n);

    std::vector<float> g ((size_t) n);
    dsp::GainTrace trace;
    trace.capacity = n;
    trace.data[0][0] = g.data();
    p.getEngine().setGainTrace (&trace);

    const auto out = r.render (in, nullptr, { 480 }, false, [] (int block, PluginProcessor& proc)
    {
        if (block == 30) setBand (proc, 1, "bypass", 1.0f);
        if (block == 60) setBand (proc, 1, "bypass", 0.0f);
        if (block == 80) proc.getBandSlots().deleteBand (1);
    });
    p.getEngine().setGainTrace (nullptr);

    // Fully engaged GR before the bypass, exact unity after the 10 ms ramp, back to GR after un-bypass.
    CHECK_THAT (dbOf (g[30 * 480 - 1]), WithinAbs (-12.0, 0.01));
    // Linear ramp: halfway through the 10 ms ramp, gEff - 1 is half of the engaged value.
    const double engaged = g[30 * 480 - 1] - 1.0;
    CHECK_THAT (g[30 * 480 + 239] - 1.0, WithinAbs (engaged * 0.5, 0.01));
    CHECK (g[30 * 480 + 480] == 1.0f);
    CHECK (g[60 * 480 - 1] == 1.0f);
    CHECK_THAT (dbOf (g[60 * 480 + 479]), WithinAbs (-12.0, 0.01));

    // Deleted band: ramps out, then the output is exactly the input.
    for (int i = 80 * 480 + 480; i < n; ++i)
        REQUIRE (out.getSample (0, i) == in.getSample (0, i));
    // While bypassed (after the ramp), exact pass-through too.
    for (int i = 30 * 480 + 480; i < 60 * 480; ++i)
        REQUIRE (out.getSample (0, i) == in.getSample (0, i));

    // Creating a band again ramps in from neutral (no jump at the first sample).
    REQUIRE (p.getBandSlots().createBand() == 2);
    setBand (p, 2, "thresh", -80.0f); setBand (p, 2, "ratio", 100.0f); setBand (p, 2, "fLo", 500.0f); setBand (p, 2, "fHi", 2000.0f);
    std::vector<float> g2 ((size_t) n);
    dsp::GainTrace t2;
    t2.capacity = n;
    t2.data[1][0] = g2.data();
    p.getEngine().setGainTrace (&t2);
    r.render (in, nullptr, { 480 });
    p.getEngine().setGainTrace (nullptr);
    CHECK (g2[0] > 0.99f);
    CHECK (dbOf (g2[1000]) < -10.0);
}

TEST_CASE ("Band re-used during its fade-out continues without reset", "[dsp][engine][lifecycle]")
{
    // Spec amendment: a slot that becomes used again while still ramping out keeps its states and
    // ramps up from its current activity (a reset there would jump gEff to 1 and click).
    const double fs = 48000.0;
    dsp::A3RegionEngine e;
    e.prepare (fs, 512);
    dsp::EngineSettings s;
    s.usedMask = 1u;
    s.bands[0].fLoHz = 500.0f;  s.bands[0].fHiHz = 2000.0f;
    s.bands[0].thresholdDb = -60.0f; s.bands[0].ratio = 100.0f; s.bands[0].rangeDb = 24.0f;
    s.bands[0].attackMs = 1.0f;

    const int n = 9600, block = 120;
    auto x = sine (1000.0, fs, n, 0.5);
    const auto in = x;
    std::vector<float> g ((size_t) n);
    dsp::GainTrace trace;
    trace.capacity = n;
    trace.data[0][0] = g.data();
    e.setGainTrace (&trace);

    const int unuse = 40 * block, reuse = 42 * block;   // 240 samples (5 ms) into the 10 ms fade-out
    for (int pos = 0; pos < n; pos += block)
    {
        s.usedMask = (pos >= unuse && pos < reuse) ? 0u : 1u;
        e.setSettings (s);
        float* ch[1] { x.data() + pos };
        e.process (ch, 1, block);
    }

    const double engaged = g[(size_t) unuse - 1] - 1.0;   // about -0.94 (24 dB)
    CHECK (engaged < -0.9);
    CHECK_THAT (g[(size_t) reuse - 1] - 1.0, WithinAbs (engaged * 0.5, 0.01));   // halfway out, not reset
    double maxStep = 0.0;
    for (int i = unuse; i < reuse + 960; ++i) maxStep = std::max (maxStep, (double) std::abs (g[(size_t) i] - g[(size_t) i - 1]));
    CHECK (maxStep <= std::abs (engaged) / 480.0 * 1.05);                          // only ramp-sized steps
    CHECK_THAT (g[(size_t) reuse + 240] - 1.0, WithinAbs (engaged, 0.01));        // back up after the rest of the ramp

    double maxStepIn = 0.0, maxStepOut = 0.0;
    for (int i = unuse; i < reuse + 960; ++i)
    {
        maxStepIn = std::max (maxStepIn, (double) std::abs (in[(size_t) i] - in[(size_t) i - 1]));
        maxStepOut = std::max (maxStepOut, (double) std::abs (x[(size_t) i] - x[(size_t) i - 1]));
    }
    CHECK (maxStepOut <= maxStepIn);
}

TEST_CASE ("Gain-reduction meter", "[dsp][engine][meter]")
{
    const double fs = 48000.0;
    PluginProcessor p;
    REQUIRE (p.getBandSlots().createBand() == 1);
    REQUIRE (p.getBandSlots().createBand() == 2);
    setBand (p, 1, "fLo", 500.0f); setBand (p, 1, "fHi", 2000.0f);
    setBand (p, 1, "thresh", -80.0f); setBand (p, 1, "ratio", 100.0f); setBand (p, 1, "range", 9.0f);
    setBand (p, 2, "thresh", 12.0f);
    OfflineRenderer r (p);
    REQUIRE (r.prepare (fs, 512, { 2, 0 }));
    juce::AudioBuffer<float> in (2, 24000);
    const auto x = sine (1000.0, fs, 24000, 0.5);
    in.copyFrom (0, 0, x.data(), 24000);
    in.copyFrom (1, 0, x.data(), 24000);
    r.render (in, nullptr, { 256 });

    const auto& m = p.getGainReductionMeter();
    CHECK_THAT (m.getGainReductionDb (1, 0), WithinAbs (-9.0, 0.01));
    CHECK_THAT (m.getGainReductionDb (1, 1), WithinAbs (-9.0, 0.01));
    CHECK (m.getGainReductionDb (2, 0) == 0.0f);   // below threshold
    CHECK (m.getGainReductionDb (5, 0) == 0.0f);   // unused
    CHECK (m.getGainReductionDb (0, 0) == 0.0f);   // out of range arguments
    CHECK (m.getGainReductionDb (9, 1) == 0.0f);
    CHECK (p.getLatencySamples() == 0);
}

TEST_CASE ("Non-finite input with active compression is replaced by zeros before the DSP", "[dsp][engine][finite]")
{
    const float nan = std::numeric_limits<float>::quiet_NaN(), inf = std::numeric_limits<float>::infinity();
    auto setup = [] (PluginProcessor& p) { createAll (p); allBands (p, "thresh", -30.0f); };

    PluginProcessor a, b;
    setup (a); setup (b);
    OfflineRenderer ra (a), rb (b);
    REQUIRE (ra.prepare (48000.0, 512, { 2, 0 }));
    REQUIRE (rb.prepare (48000.0, 512, { 2, 0 }));

    auto bad = makeNoise (2, 20000, 6u, 0.7f);
    auto zeroed = bad;
    for (int c = 0; c < 2; ++c)
        for (int i : { 0, 1, 1000, 1001, 5000, 12345, 19999 })
        {
            bad.setSample (c, i, (i % 2) ? nan : (i % 3 ? inf : -inf));
            zeroed.setSample (c, i, 0.0f);
        }

    const auto outBad = ra.render (bad, nullptr, { 1, 100, 7, 512 });
    const auto outRef = rb.render (zeroed, nullptr, { 1, 100, 7, 512 });
    CHECK (bitIdentical (outBad, outRef));
    CHECK (a.getFiniteGuard().exchangeTriggered());
    CHECK (a.getEngine().getStateResetCount() == 0u);
}

TEST_CASE ("Non-finite internal state resets the band and recovers", "[dsp][engine][finite]")
{
    // An infinite external gain on band 1 overflows its output; band 2 (cascade) then sees a
    // non-finite input and its state becomes non-finite -> band 2 is reset; output guard cleans up.
    dsp::A3RegionEngine e;
    e.prepare (48000.0, 512);
    dsp::EngineSettings s;
    s.usedMask = 0x3u;
    s.bands[0].fLoHz = 500.0f;  s.bands[0].fHiHz = 2000.0f;  s.bands[0].thresholdDb = 12.0f;
    s.bands[1].fLoHz = 800.0f;  s.bands[1].fHiHz = 3000.0f;  s.bands[1].thresholdDb = 12.0f;
    e.setSettings (s);

    const float infGain = std::numeric_limits<float>::infinity();
    dsp::ExternalGains ext;
    ext.data[0] = &infGain;
    auto x = sine (1000.0, 48000.0, 4096, 0.5);
    float* ch[1] { x.data() };
    e.setExternalGains (&ext);
    e.process (ch, 1, 256);
    e.setExternalGains (nullptr);
    CHECK (e.getStateResetCount() >= 1u);

    auto y = sine (1000.0, 48000.0, 48000, 0.5);
    const auto ref = y;
    float* ch2[1] { y.data() };
    e.process (ch2, 1, 48000);
    for (size_t i = 0; i < y.size(); ++i) REQUIRE (std::isfinite (y[i]));
    CHECK (std::memcmp (y.data() + 24000, ref.data() + 24000, sizeof (float) * 24000) == 0);   // neutral again
}

TEST_CASE ("Denormals: silence after compression decays to exact zeros", "[dsp][engine][denormal]")
{
    for (double fs : { 44100.0, 192000.0 })
    {
        PluginProcessor p;
        createAll (p);
        allBands (p, "thresh", -40.0f);
        allBands (p, "release", 5000.0f);   // GR still releasing while the filters ring out
        OfflineRenderer r (p);
        REQUIRE (r.prepare (fs, 512, { 2, 0 }));
        const int n = (int) (3.0 * fs);
        auto in = makeNoise (2, n, 2u, 0.8f);
        for (int c = 0; c < 2; ++c)
            for (int i = (int) (0.5 * fs); i < n; ++i)
                in.setSample (c, i, i % 1000 == 0 ? 1.0e-39f : 0.0f);   // silence with denormal specks
        const auto out = r.render (in, nullptr, { 256 });

        INFO ("fs " << fs);
        for (int c = 0; c < 2; ++c)
            for (int i = (int) (2.5 * fs); i < n; ++i)
            {
                const float v = out.getSample (c, i);
                REQUIRE (std::isfinite (v));
                REQUIRE ((v == 0.0f || std::abs (v) >= std::numeric_limits<float>::min() || i % 1000 == 0));
            }
        // GR is still active (long release), yet the region states have flushed to zero.
        CHECK (p.getGainReductionMeter().getGainReductionDb (1, 0) < -1.0f);
        CHECK (p.getEngine().getStateResetCount() == 0u);
    }
}

TEST_CASE ("No audio-thread heap operations with the engine active", "[dsp][engine][allocation]")
{
    for (double rate : { 44100.0, 192000.0 })
        for (auto layout : { Layout { 2, 2 }, Layout { 1, 0 }, Layout { 2, 0 } })
        {
            PluginProcessor p;
            createAll (p);
            allBands (p, "thresh", -30.0f);
            OfflineRenderer r (p);
            REQUIRE (r.prepare (rate, 1024, layout));

            const auto in = makeNoise (layout.mainChannels, 12000, 1u);
            const auto sc = makeNoise (juce::jmax (1, layout.sidechainChannels), 12000, 2u);
            const auto* scp = layout.sidechainChannels > 0 ? &sc : nullptr;

            std::mt19937 rng (1);
            std::uniform_real_distribution<float> u (0.0f, 1.0f);
            auto automate = [&] (int block, PluginProcessor& proc)
            {
                for (int s = 1; s <= 8; ++s)
                    for (auto* id : { "fLo", "fHi", "thresh", "ratio", "knee", "attack", "release", "range", "detMode", "detBlend", "rmsTime", "link", "bypass" })
                        if (u (rng) < 0.2f) proc.getState().getParameter (schema::bandParamId (s, id))->setValueNotifyingHost (u (rng));
                proc.getState().getParameter ("bypass")->setValueNotifyingHost (block % 13 == 0 ? 1.0f : 0.0f);
                auto& slots = proc.getBandSlots();   // topology changes on the message thread, between blocks
                if (block % 11 == 5 && ! slots.usedSlots().empty()) slots.deleteBand (slots.usedSlots().back());
                if (block % 11 == 9) slots.createBand();
            };

            resetAllocationCount();
            for (auto& seq : std::vector<std::vector<int>> { { 1 }, { 64 }, { 1, 3, 1024, 17, 255 } })
            {
                r.render (in, scp, seq, false, automate);
                r.render (in, scp, seq, true, automate);
            }
            CHECK (allocationCount() == 0);
        }
}
