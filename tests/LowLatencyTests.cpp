// Low Latency: one switch that overrides every source of delay (band lookahead, constant
// latency, the colour stage's and the clipper's oversampling) without changing their stored values.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <memory>

#include "dsp/DspConstants.h"
#include "dsp/Lookahead.h"
#include "dsp/Ramp.h"
#include "support/AllocationCounter.h"
#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;

namespace
{
constexpr double kFs = 48000.0;

struct Rig
{
    std::unique_ptr<PluginProcessor> proc = std::make_unique<PluginProcessor>();
    OfflineRenderer renderer { *proc };

    explicit Rig (int bands = 2)
    {
        for (int i = 0; i < bands; ++i) proc->getBandSlots().createBand();
        restart();
    }
    void restart() { REQUIRE (renderer.prepare (kFs, 512, { 1, 0 })); }
    float value (const char* id) const { return proc->getState().getRawParameterValue (id)->load(); }

    /** Every source of delay turned up, the bands neutral (threshold above any signal). */
    void everythingDelayed()
    {
        setBand (*proc, 1, "thresh", 12.0f);
        setBand (*proc, 2, "thresh", 12.0f);
        setBand (*proc, 1, "lookahead", 10.0f);
        setBand (*proc, 2, "lookahead", 3.0f);
        setPlain (*proc, "constantLatency", 1.0f);
        setPlain (*proc, "oversampling", 3.0f);
        setPlain (*proc, "clipOversampling", 3.0f);
    }
};

juce::AudioBuffer<float> sineBuffer (double hz, int n, double amplitude)
{
    juce::AudioBuffer<float> b (1, n);
    const auto s = sine (hz, kFs, n, amplitude);
    b.copyFrom (0, 0, s.data(), n);
    return b;
}
} // namespace

TEST_CASE ("Low Latency reports 0 samples and gives the stored settings back when it goes off", "[lowlatency]")
{
    Rig rig;
    CHECK (rig.value ("lowLatency") == 0.0f);                       // off by default
    rig.everythingDelayed();
    const int full = rig.proc->getLatencySamples();
    CHECK (full >= 480 + 55);                                       // 10 ms lookahead plus the oversampling filters

    setPlain (*rig.proc, "lowLatency", 1.0f);
    CHECK (rig.proc->getLatencySamples() == 0);                     // reported as soon as it is pressed
    CHECK (rig.value ("b1_lookahead") == 10.0f);                    // an override: nothing stored changes
    CHECK (rig.value ("b2_lookahead") == 3.0f);
    CHECK (rig.value ("constantLatency") == 1.0f);
    CHECK (rig.value ("oversampling") == 3.0f);
    CHECK (rig.value ("clipOversampling") == 3.0f);

    // A setting changed while it is on is kept for later and adds no delay now.
    setBand (*rig.proc, 1, "lookahead", 5.0f);
    CHECK (rig.proc->getLatencySamples() == 0);

    setPlain (*rig.proc, "lowLatency", 0.0f);
    CHECK (rig.proc->getLatencySamples() == full);                  // constant latency: 10 ms whatever the lookahead
}

TEST_CASE ("Low Latency passes a neutral band through with no delay at all", "[lowlatency]")
{
    Rig rig;
    rig.everythingDelayed();
    setPlain (*rig.proc, "lowLatency", 1.0f);
    rig.restart();
    const auto in = makeNoise (1, 9600, 5u, 0.5f);
    test::resetAllocationCount();
    const auto out = rig.renderer.render (in, nullptr, { 64, 300, 1, 512 });
    CHECK (test::allocationCount() == 0);
    CHECK (bitIdentical (out, in));
}

TEST_CASE ("Low Latency switched during playback: no step, no allocation, exact before and after", "[lowlatency][transition]")
{
    const int n = 48000, block = 128, onBlock = 100, offBlock = 220;
    const auto in = sineBuffer (220.0, n, 0.5);
    const float sineStep = 0.5f * (float) (2.0 * dsp::kPi * 220.0 / kFs);

    Rig rig;
    setBand (*rig.proc, 1, "thresh", 12.0f);
    setBand (*rig.proc, 2, "thresh", 12.0f);
    setBand (*rig.proc, 1, "lookahead", 5.0f);
    rig.restart();
    const int delay = rig.proc->getLatencySamples();
    REQUIRE (delay == 240);

    test::resetAllocationCount();
    const auto out = rig.renderer.render (in, nullptr, { block }, false, [&] (int b, PluginProcessor& p)
    {
        if (b == onBlock)  setPlain (p, "lowLatency", 1.0f);
        if (b == offBlock) setPlain (p, "lowLatency", 0.0f);
    });
    CHECK (test::allocationCount() == 0);
    CHECK (rig.proc->getLatencySamples() == delay);

    const int fade = (int) std::lround (dsp::kLookaheadFadeSeconds * kFs) + 2 * block;
    for (int i = delay; i < onBlock * block; ++i)                               // before: the 5 ms delay
        REQUIRE (out.getSample (0, i) == in.getSample (0, i - delay));
    for (int i = onBlock * block + delay + fade; i < offBlock * block; ++i)     // on: no delay
        REQUIRE (out.getSample (0, i) == in.getSample (0, i));
    for (int i = offBlock * block + delay + fade; i < n; ++i)                   // off again: the delay is back
        REQUIRE (out.getSample (0, i) == in.getSample (0, i - delay));

    float largestStep = 0.0f;
    for (int i = delay + 1; i < n; ++i)
        largestStep = std::max (largestStep, std::abs (out.getSample (0, i) - out.getSample (0, i - 1)));
    CHECK (largestStep < 1.5f * sineStep);                                      // a crossfade, not a click
}

TEST_CASE ("An old state without Low Latency loads with it off and the delay it had", "[lowlatency][state]")
{
    Rig saved;
    saved.everythingDelayed();
    juce::MemoryBlock block;
    saved.proc->getStateInformation (block);
    const int delay = saved.proc->getLatencySamples();

    // The same state as written before the parameter existed.
    auto xml = juce::AudioProcessor::getXmlFromBinary (block.getData(), (int) block.getSize());
    REQUIRE (xml != nullptr);
    for (auto* e = xml->getFirstChildElement(); e != nullptr;)
    {
        auto* next = e->getNextElement();
        if (e->getStringAttribute ("id") == "lowLatency") xml->removeChildElement (e, true);
        e = next;
    }
    REQUIRE (xml->toString().contains ("lowLatency") == false);
    juce::MemoryBlock old;
    juce::AudioProcessor::copyXmlToBinary (*xml, old);

    Rig loaded (0);
    setPlain (*loaded.proc, "lowLatency", 1.0f);                     // must go back to its default: missing in the file
    loaded.proc->setStateInformation (old.getData(), (int) old.getSize());
    CHECK (loaded.value ("lowLatency") == 0.0f);
    CHECK (loaded.proc->getLatencySamples() == delay);
}

TEST_CASE ("Low Latency: the clipper's ceiling still holds on every sample without oversampling", "[lowlatency][clip]")
{
    const float ceiling = dsp::dbToLinear (-6.0f);
    for (const float mode : { 1.0f, 2.0f })
    {
        INFO ((mode == 2.0f ? "hard" : "soft"));
        Rig rig (1);
        setPlain (*rig.proc, "clipMode", mode);
        setPlain (*rig.proc, "clipCeiling", -6.0f);
        setPlain (*rig.proc, "clipOversampling", 3.0f);
        setPlain (*rig.proc, "lowLatency", 1.0f);
        rig.restart();
        REQUIRE (rig.proc->getLatencySamples() == 0);
        const auto out = rig.renderer.render (sineBuffer (7100.0, 24000, 1.5), nullptr, { 64, 300, 1 });
        float peak = 0.0f;
        for (int i = 0; i < out.getNumSamples(); ++i) peak = std::max (peak, std::abs (out.getSample (0, i)));
        CHECK (peak <= ceiling);          // sample peaks; inter-sample peaks can go over at 1x
        CHECK (peak > 0.99f * ceiling);   // and it is clipping
    }
}
