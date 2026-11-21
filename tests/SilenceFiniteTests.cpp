#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <limits>

#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;

TEST_CASE ("Silence in gives exact zeros out", "[silence]")
{
    for (int ch : { 1, 2 })
        for (int block : { 1, 64, 1024 })
        {
            PluginProcessor p;
            OfflineRenderer r (p);
            REQUIRE (r.prepare (48000.0, 1024, { ch, 0 }));
            juce::AudioBuffer<float> in (ch, 4096);
            in.clear();
            const auto out = r.render (in, nullptr, { block });
            for (int c = 0; c < ch; ++c)
                for (int i = 0; i < out.getNumSamples(); ++i)
                    REQUIRE (out.getSample (c, i) == 0.0f);
        }
}

TEST_CASE ("NaN and Inf input never reach the output", "[finite]")
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    for (int ch : { 1, 2 })
        for (bool bypassed : { false, true })
        {
            PluginProcessor p;
            OfflineRenderer r (p);
            REQUIRE (r.prepare (44100.0, 512, { ch, 0 }));

            auto in = makeNoise (ch, 2000, 5u, 0.5f);
            const std::vector<std::pair<int, float>> bad { { 0, nan }, { 1, inf }, { 2, -inf }, { 500, nan }, { 1999, -inf },
                                                           { 1000, std::bit_cast<float> (0x7fc00001u) }, { 1001, std::bit_cast<float> (0xffffffffu) } };
            for (int c = 0; c < ch; ++c)
                for (auto& [i, v] : bad) in.setSample (c, i, v);

            CHECK_FALSE (p.getFiniteGuard().wasTriggered());
            const auto out = r.render (in, nullptr, { 1, 100, 7, 512 }, bypassed);

            for (int c = 0; c < ch; ++c)
                for (int i = 0; i < out.getNumSamples(); ++i)
                {
                    REQUIRE (std::isfinite (out.getSample (c, i)));
                    const bool wasBad = ! std::isfinite (in.getSample (c, i));
                    if (wasBad) REQUIRE (out.getSample (c, i) == 0.0f);
                    else        REQUIRE (out.getSample (c, i) == in.getSample (c, i));   // finite samples untouched
                }

            CHECK (p.getFiniteGuard().exchangeTriggered());
            CHECK_FALSE (p.getFiniteGuard().wasTriggered());
        }
}
