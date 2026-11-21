#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;

TEST_CASE ("Pass-through is bit-exact for all rates, channel counts and block sizes", "[passthrough]")
{
    const double rate = GENERATE (44100.0, 48000.0, 88200.0, 96000.0, 192000.0);
    const int channels = GENERATE (1, 2);
    const int block = GENERATE (1, 16, 32, 64, 128, 256, 512, 1024, -1);   // -1 = irregular
    INFO ("rate " << rate << " channels " << channels << " block " << block);

    PluginProcessor p;
    OfflineRenderer r (p);
    REQUIRE (r.prepare (rate, 1024, { channels, 0 }));
    CHECK (p.getLatencySamples() == 0);

    const auto in = makeNoise (channels, block == 1 ? 3000 : 5000, 7u + (unsigned) channels);
    const std::vector<int> seq = block > 0 ? std::vector<int> { block } : std::vector<int> { 1, 7, 33, 1024, 3, 511, 64 };

    CHECK (bitIdentical (in, r.render (in, nullptr, seq)));
    CHECK (bitIdentical (in, r.render (in, nullptr, seq, true)));   // processBlockBypassed
    CHECK (p.getLatencySamples() == 0);
}

TEST_CASE ("Output is independent of block-size sequence", "[blocksize]")
{
    const double rate = GENERATE (44100.0, 96000.0);
    PluginProcessor p;
    OfflineRenderer r (p);
    REQUIRE (r.prepare (rate, 1024, { 2, 2 }));

    const auto in = makeNoise (2, 9000, 99u);
    const auto sc = makeNoise (2, 9000, 5u);
    const auto reference = r.render (in, &sc, { 512 });

    for (auto& seq : std::vector<std::vector<int>> { { 1 }, { 16 }, { 1, 2, 3, 5, 8, 13, 21, 1000 }, { 1024 }, { 127, 129 } })
        CHECK (bitIdentical (reference, r.render (in, &sc, seq)));

    // Same under parameter automation that does not depend on the block layout.
    auto change = [] (int, PluginProcessor&) {};
    CHECK (bitIdentical (reference, r.render (in, &sc, { 77 }, false, change)));
}
