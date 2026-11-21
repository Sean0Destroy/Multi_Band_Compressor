#include <catch2/catch_test_macros.hpp>

#include "support/AllocationCounter.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;

TEST_CASE ("Allocation counter works (positive control)", "[allocation]")
{
    resetAllocationCount();
    {
        AudioCallbackScope s;
        auto* p = new int (3);
        delete p;
    }
    CHECK (allocationCount() >= 1);

    resetAllocationCount();
    auto* q = new int (4);   // outside the scope: not counted
    delete q;
    CHECK (allocationCount() == 0);
}

TEST_CASE ("No audio-thread allocations", "[allocation]")
{
    for (double rate : { 44100.0, 96000.0 })
        for (auto layout : { Layout { 2, 2 }, Layout { 1, 0 }, Layout { 2, 0 }, Layout { 1, 1 } })
        {
            PluginProcessor p;
            OfflineRenderer r (p);
            REQUIRE (r.prepare (rate, 1024, layout));

            const auto in = makeNoise (layout.mainChannels, 6000, 1u);
            const auto sc = makeNoise (juce::jmax (1, layout.sidechainChannels), 6000, 2u);
            const auto* scp = layout.sidechainChannels > 0 ? &sc : nullptr;

            auto automate = [] (int block, PluginProcessor& proc)
            {
                auto& s = proc.getState();
                s.getParameter ("inGain")->setValueNotifyingHost ((block % 7) / 7.0f);
                s.getParameter ("b1_thresh")->setValueNotifyingHost ((block % 5) / 5.0f);
                s.getParameter ("bypass")->setValueNotifyingHost ((float) (block % 2));
            };

            resetAllocationCount();
            for (auto& seq : std::vector<std::vector<int>> { { 1 }, { 64 }, { 1, 3, 1024, 17, 255 } })
            {
                r.render (in, scp, seq, false, automate);
                r.render (in, scp, seq, true, automate);
            }

            // The renderer's own buffers are allocated outside the callback scope; only processBlock counts.
            CHECK (allocationCount() == 0);
        }
}
