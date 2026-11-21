#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <limits>

#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;
using Set = juce::AudioChannelSet;

namespace
{
bool allZero (const juce::AudioBuffer<float>& b)
{
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! (b.getSample (c, i) == 0.0f)) return false;
    return true;
}
}

TEST_CASE ("Bus layout support matrix", "[sidechain][layout]")
{
    PluginProcessor p;
    CHECK (p.getBusCount (true) == 2);
    CHECK (p.getBusCount (false) == 1);
    CHECK (p.getBus (true, 1)->getName() == "Sidechain");
    CHECK_FALSE (p.getBus (true, 1)->isEnabled());                       // not active by default
    CHECK (p.getChannelLayoutOfBus (true, 0) == Set::stereo());          // stereo default
    CHECK (p.getBus (true, 1)->getDefaultLayout() == Set::stereo());

    const std::vector<Set> mains { Set::disabled(), Set::mono(), Set::stereo(), Set::createLCR(), Set::create5point1() };
    const std::vector<Set> scs   { Set::disabled(), Set::mono(), Set::stereo(), Set::createLCR(), Set::create5point1() };

    for (auto& in : mains)
        for (auto& out : mains)
            for (auto& sc : scs)
            {
                juce::AudioProcessor::BusesLayout l;
                l.inputBuses.add (in); l.inputBuses.add (sc); l.outputBuses.add (out);

                const bool mainOk = in == out && (in == Set::mono() || in == Set::stereo());
                const bool scOk = sc.isDisabled() || sc == Set::mono() || sc == Set::stereo();
                INFO ("in " << in.getDescription() << " out " << out.getDescription() << " sc " << sc.getDescription());
                CHECK (p.checkBusesLayoutSupported (l) == (mainOk && scOk));
            }

    // setBusesLayout agrees for the accepted combinations.
    for (int m : { 1, 2 })
        for (int s : { 0, 1, 2 })
        {
            PluginProcessor q;
            OfflineRenderer r (q);
            CHECK (r.prepare (48000.0, 256, { m, s }));
        }
}

TEST_CASE ("Sidechain never reaches the output", "[sidechain]")
{
    const int mainCh = GENERATE (1, 2);
    const int scCh = GENERATE (1, 2);
    const int block = GENERATE (1, 64, 1024);
    INFO ("main " << mainCh << " sc " << scCh << " block " << block);

    PluginProcessor p;
    OfflineRenderer r (p);
    REQUIRE (r.prepare (48000.0, 1024, { mainCh, scCh }));
    CHECK (p.getBus (true, 1)->isEnabled());

    const int n = 4096;
    juce::AudioBuffer<float> silence (mainCh, n);
    silence.clear();

    auto sc = makeNoise (scCh, n, 3u);                       // full-scale noise
    for (int ch = 0; ch < scCh; ++ch)
    {
        sc.setSample (ch, 10, std::numeric_limits<float>::quiet_NaN());
        sc.setSample (ch, 11, std::numeric_limits<float>::infinity());
        sc.setSample (ch, 12, -std::numeric_limits<float>::infinity());
        sc.setSample (ch, 13, 1.0f);
        sc.setSample (ch, 14, -1.0f);
    }

    CHECK (allZero (r.render (silence, &sc, { block })));
    CHECK (allZero (r.render (silence, &sc, { block }, true)));   // bypassed path too
}

TEST_CASE ("Sidechain disabled path and re-prepare after toggling", "[sidechain]")
{
    PluginProcessor p;
    OfflineRenderer r (p);
    const auto in = makeNoise (2, 3000, 11u);
    const auto sc = makeNoise (2, 3000, 12u, 1.0f);

    REQUIRE (r.prepare (44100.0, 512, { 2, 0 }));
    CHECK_FALSE (p.getBus (true, 1)->isEnabled());
    CHECK (bitIdentical (in, r.render (in, nullptr, { 100 })));

    REQUIRE (r.prepare (48000.0, 512, { 2, 2 }));
    CHECK (p.getBus (true, 1)->isEnabled());
    CHECK (bitIdentical (in, r.render (in, &sc, { 100 })));

    REQUIRE (r.prepare (96000.0, 256, { 2, 1 }));
    CHECK (bitIdentical (in, r.render (in, &sc, { 33 })));

    REQUIRE (r.prepare (44100.0, 512, { 2, 0 }));
    CHECK_FALSE (p.getBus (true, 1)->isEnabled());
    CHECK (bitIdentical (in, r.render (in, nullptr, { 100 })));
    CHECK (p.getLatencySamples() == 0);

    // Main mono with stereo sidechain, then back.
    REQUIRE (r.prepare (44100.0, 512, { 1, 2 }));
    const auto mono = makeNoise (1, 2000, 21u);
    CHECK (bitIdentical (mono, r.render (mono, &sc, { 50 })));
}
