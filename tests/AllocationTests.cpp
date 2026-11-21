#include <cstdlib>
#include <catch2/catch_test_macros.hpp>

#include "support/AllocationCounter.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;

#if defined (__GLIBC__)
TEST_CASE ("Allocation counter detects malloc and free (positive control)", "[allocation]")
{
    // Volatile function pointers stop the optimiser from eliding the malloc/free pair.
    void* (* volatile doMalloc) (std::size_t) = std::malloc;
    void  (* volatile doFree) (void*) = std::free;

    resetAllocationCount();
    {
        AudioCallbackScope s;
        void* p = doMalloc (64);
        doFree (p);
    }
    CHECK (allocationCount() >= 2);   // one allocation + one deallocation
}
#endif

TEST_CASE ("Allocation counter works (positive control)", "[allocation]")
{
    resetAllocationCount();
    {
        AudioCallbackScope s;
        // Call the replaceable allocation function directly: unlike a new-expression,
        // a direct call may not be elided by the optimiser (C++14 allocation elision),
        // so this control stays meaningful in Release builds.
        void* p = ::operator new (sizeof (int));
        ::operator delete (p);
    }
    CHECK (allocationCount() >= 1);

    resetAllocationCount();
    void* q = ::operator new (sizeof (int));   // outside the scope: not counted
    ::operator delete (q);
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
