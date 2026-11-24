// Audition: band solo (the band's processed component) and detector listen.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <memory>

#include "dsp/A3Model.h"
#include "support/AllocationCounter.h"
#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;
using Catch::Matchers::WithinAbs;
using Mode = dsp::AuditionMode;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kFade = 480 + 256;        // 10 ms crossfade plus a block of slack

struct Rig
{
    std::unique_ptr<PluginProcessor> proc = std::make_unique<PluginProcessor>();
    OfflineRenderer renderer { *proc };
    Layout layout;

    explicit Rig (int channels = 1, int bands = 1, int sidechain = 0) : layout { channels, sidechain }
    {
        for (int i = 0; i < bands; ++i) proc->getBandSlots().createBand();
        REQUIRE (renderer.prepare (kFs, 512, layout));
    }

    void band (int slot, const char* suffix, float v) { setBand (*proc, slot, suffix, v); }
    void restart() { REQUIRE (renderer.prepare (kFs, 512, layout)); }
    juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& in, const juce::AudioBuffer<float>* sc = nullptr, bool bypassed = false)
    {
        return renderer.render (in, sc, { 128 }, bypassed);
    }
};

juce::AudioBuffer<float> sineBuffer (double f, int n, double amplitude, int channels = 1)
{
    juce::AudioBuffer<float> b (channels, n);
    const auto s = sine (f, kFs, n, amplitude);
    for (int c = 0; c < channels; ++c) b.copyFrom (c, 0, s.data(), n);
    return b;
}

double rmsDb (const juce::AudioBuffer<float>& b, int ch, int start, int end)
{
    double acc = 0.0;
    for (int i = start; i < end; ++i) acc += (double) b.getSample (ch, i) * b.getSample (ch, i);
    return 10.0 * std::log10 (acc / (double) (end - start) + 1.0e-30);
}
}

TEST_CASE ("Listen: the output is the band's detector signal, 6 dB down", "[dsp][audition]")
{
    const int n = 24000;
    Rig rig;
    rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
    rig.band (1, "thresh", -60.0f); rig.band (1, "ratio", 20.0f);          // heavy compression must not matter
    rig.restart();
    CHECK (rig.proc->getAuditionMode() == Mode::None);

    rig.proc->setAudition (Mode::Listen, 1);
    CHECK (rig.proc->getAuditionMode() == Mode::Listen);
    CHECK (rig.proc->getAuditionSlot() == 1);

    // In band: the detector filter's gain at that frequency minus 6 dB. Far below: strongly attenuated.
    for (double f : { 1000.0, 700.0, 100.0, 6000.0 })
    {
        INFO ("f " << f);
        rig.restart();
        const auto in = sineBuffer (f, n, 0.5);
        test::resetAllocationCount();
        const auto out = rig.render (in);
        CHECK (test::allocationCount() == 0);
        const double expected = dbOf (detectorGain (f, 500.0, 2000.0, kFs)) - 6.0206;
        CHECK_THAT (rmsDb (out, 0, n / 2, n) - rmsDb (in, 0, n / 2, n), WithinAbs (expected, 0.05));
    }

    // Listening to a band that does not exist gives silence, not the programme.
    rig.proc->setAudition (Mode::Listen, 5);
    rig.restart();
    const auto silent = rig.render (sineBuffer (1000.0, n, 0.5));
    CHECK (rmsDb (silent, 0, n / 2, n) < -120.0);
}

TEST_CASE ("Listen follows the detector routing: external source, custom range, mid/side", "[dsp][audition]")
{
    const int n = 24000;

    SECTION ("external sidechain is what is heard (and only then does it reach the output)")
    {
        Rig rig (2, 1, 2);
        rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
        rig.band (1, "scSource", 1.0f);
        rig.restart();
        juce::AudioBuffer<float> silence (2, n);
        silence.clear();
        const auto key = sineBuffer (1000.0, n, 0.5, 2);

        const auto normal = rig.render (silence, &key);
        CHECK (rmsDb (normal, 0, n / 2, n) < -120.0);                      // not listening: no sidechain in the output

        rig.proc->setAudition (Mode::Listen, 1);
        rig.restart();
        const auto heard = rig.render (silence, &key);
        const double expected = dbOf (detectorGain (1000.0, 500.0, 2000.0, kFs)) - 6.0206 + 10.0 * std::log10 (0.125);
        CHECK_THAT (rmsDb (heard, 0, n / 2, n), WithinAbs (expected, 0.05));
        CHECK_THAT (rmsDb (heard, 1, n / 2, n), WithinAbs (expected, 0.05));
    }

    SECTION ("custom detector range")
    {
        Rig rig;
        rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
        rig.band (1, "scMode", 2.0f); rig.band (1, "scLo", 60.0f); rig.band (1, "scHi", 200.0f);
        rig.proc->setAudition (Mode::Listen, 1);
        rig.restart();
        const auto in = sineBuffer (100.0, n, 0.5);
        const auto out = rig.render (in);
        CHECK_THAT (rmsDb (out, 0, n / 2, n) - rmsDb (in, 0, n / 2, n), WithinAbs (dbOf (detectorGain (100.0, 60.0, 200.0, kFs)) - 6.0206, 0.05));
    }

    SECTION ("a mid/side band's detector lanes are decoded back to left / right")
    {
        Rig rig (2);
        rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
        rig.band (1, "stereoMode", 4.0f);                                  // Mid/Side, detector per lane = mid and side
        rig.proc->setAudition (Mode::Listen, 1);
        rig.restart();

        // Left-only input: mid = side = L/2, so the decoded detector signal is left only.
        auto in = sineBuffer (1000.0, n, 0.5, 2);
        in.clear (1, 0, n);
        const auto out = rig.render (in);
        const double expected = dbOf (detectorGain (1000.0, 500.0, 2000.0, kFs)) - 6.0206;
        CHECK_THAT (rmsDb (out, 0, n / 2, n) - rmsDb (in, 0, n / 2, n), WithinAbs (expected, 0.05));
        CHECK (rmsDb (out, 1, n / 2, n) < -100.0);
    }
}

TEST_CASE ("Solo: the output is the band's own component after its gain", "[dsp][audition]")
{
    const int n = 24000;
    const auto in = makeNoise (2, n, 11u, 0.4f);

    // Force the band gain to exactly 0.5 with the measurement hook: then
    //   normal output  y = x + (0.5 - 1) c      (c = the band's component at unity gain)
    //   solo output    s = 0.5 c
    // so s == -(y - x).
    float half = 0.5f;
    const auto run = [&] (Mode mode)
    {
        Rig rig (2);
        rig.band (1, "fLo", 300.0f); rig.band (1, "fHi", 3000.0f);
        rig.restart();
        dsp::ExternalGains ext;
        ext.data[0] = &half;
        rig.proc->getEngine().setExternalGains (&ext);
        rig.proc->setAudition (mode, 1);
        auto out = rig.render (in);
        rig.proc->getEngine().setExternalGains (nullptr);
        return out;
    };

    const auto normal = run (Mode::None), solo = run (Mode::Solo);
    double worst = 0.0;
    for (int c = 0; c < 2; ++c)
        for (int i = kFade; i < n; ++i)
            worst = std::max (worst, std::abs ((double) solo.getSample (c, i) + ((double) normal.getSample (c, i) - in.getSample (c, i))));
    CHECK (worst < 2.0e-6);
    CHECK (rmsDb (solo, 0, n / 2, n) > -40.0);

    // A sine at the band centre comes out at the band gain; far outside the band it is rejected.
    Rig rig;
    rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
    rig.band (1, "thresh", 12.0f); rig.band (1, "makeup", -6.0f);
    rig.proc->setAudition (Mode::Solo, 1);
    rig.restart();
    const double fc = digitalCentre (500.0, 2000.0, kFs);
    const auto centre = sineBuffer (fc, n, 0.5);
    CHECK_THAT (rmsDb (rig.render (centre), 0, n / 2, n) - rmsDb (centre, 0, n / 2, n), WithinAbs (-6.0, 0.05));
    rig.restart();
    const auto low = sineBuffer (40.0, n, 0.5);
    CHECK (rmsDb (rig.render (low), 0, n / 2, n) - rmsDb (low, 0, n / 2, n) < -15.0);

    // A mid/side band soloed: with a left-only input both lanes carry it, output on the left only.
    Rig ms (2);
    ms.band (1, "fLo", 500.0f); ms.band (1, "fHi", 2000.0f);
    ms.band (1, "thresh", 12.0f); ms.band (1, "stereoMode", 4.0f);
    ms.proc->setAudition (Mode::Solo, 1);
    ms.restart();
    auto leftOnly = sineBuffer (fc, n, 0.5, 2);
    leftOnly.clear (1, 0, n);
    const auto msOut = ms.render (leftOnly);
    CHECK_THAT (rmsDb (msOut, 0, n / 2, n) - rmsDb (leftOnly, 0, n / 2, n), WithinAbs (0.0, 0.05));
    CHECK (rmsDb (msOut, 1, n / 2, n) < -100.0);
}

TEST_CASE ("Audition: crossfaded switching, off while bypassed, cleared by state changes", "[dsp][audition]")
{
    const int n = 9600;
    Rig rig;
    rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f); rig.band (1, "thresh", 12.0f);
    rig.restart();
    const auto in = sineBuffer (300.0, n, 0.5);

    SECTION ("switching on and off is a 10 ms crossfade")
    {
        const float largestInputStep = 0.5f * (float) (2.0 * dsp::kPi * 300.0 / kFs);
        const auto out = rig.renderer.render (in, nullptr, { 128 }, false, [] (int block, PluginProcessor& p)
        {
            if (block == 20) p.setAudition (Mode::Solo, 1);
            if (block == 50) p.setAudition (Mode::None, 0);
        });

        for (int i = 0; i < 20 * 128; ++i) REQUIRE (out.getSample (0, i) == in.getSample (0, i));           // before
        for (int i = 50 * 128 + kFade; i < n; ++i) REQUIRE (out.getSample (0, i) == in.getSample (0, i));   // after: exactly back
        CHECK (std::abs (out.getSample (0, 40 * 128)) < 0.5f);                                               // during: the band component

        float largestStep = 0.0f;
        for (int i = 1; i < n; ++i)
            largestStep = std::max (largestStep, std::abs (out.getSample (0, i) - out.getSample (0, i - 1)));
        CHECK (largestStep < 1.6f * largestInputStep);
    }

    SECTION ("changing the auditioned band or the mode fades out, switches, fades in")
    {
        rig.proc->getBandSlots().createBand();
        rig.band (2, "fLo", 100.0f); rig.band (2, "fHi", 400.0f); rig.band (2, "thresh", 12.0f);
        rig.proc->setAudition (Mode::Solo, 1);
        rig.restart();
        const auto out = rig.renderer.render (in, nullptr, { 128 }, false, [] (int block, PluginProcessor& p)
        {
            if (block == 30) p.setAudition (Mode::Listen, 2);
        });
        float largestStep = 0.0f;
        for (int i = 1; i < n; ++i)
            largestStep = std::max (largestStep, std::abs (out.getSample (0, i) - out.getSample (0, i - 1)));
        CHECK (largestStep < 0.06f);
        CHECK (rig.proc->getAuditionMode() == Mode::Listen);
        CHECK (rig.proc->getAuditionSlot() == 2);
    }

    SECTION ("bypass plays the input, not the audition")
    {
        rig.proc->setAudition (Mode::Solo, 1);
        rig.restart();
        CHECK_FALSE (bitIdentical (rig.render (in), in));
        const auto bypassed = rig.render (in, nullptr, true);
        for (int i = kFade; i < n; ++i) REQUIRE (bypassed.getSample (0, i) == in.getSample (0, i));
        setPlain (*rig.proc, "bypass", 1.0f);
        const auto pluginBypassed = rig.render (in);
        for (int i = kFade; i < n; ++i) REQUIRE (pluginBypassed.getSample (0, i) == in.getSample (0, i));
    }

    SECTION ("never left on: state load, preset load, A/B and removing the band clear it")
    {
        juce::MemoryBlock state;
        rig.proc->getStateInformation (state);

        rig.proc->setAudition (Mode::Solo, 1);
        rig.proc->setStateInformation (state.getData(), (int) state.getSize());
        CHECK (rig.proc->getAuditionMode() == Mode::None);

        rig.proc->setAudition (Mode::Listen, 1);
        REQUIRE (rig.proc->getPresets().loadByName ("Starter - De-Ess").wasOk());
        CHECK (rig.proc->getAuditionMode() == Mode::None);

        rig.proc->setAudition (Mode::Solo, 1);
        rig.proc->getPresets().switchTo (1);
        rig.proc->getPresets().switchTo (0);
        CHECK (rig.proc->getAuditionMode() == Mode::None);

        rig.proc->getBandSlots().createBand();
        rig.proc->setAudition (Mode::Solo, 2);
        REQUIRE (rig.proc->getBandSlots().deleteBand (1));                  // another band: stays
        CHECK (rig.proc->getAuditionMode() == Mode::Solo);
        REQUIRE (rig.proc->getBandSlots().deleteBand (2));                  // the soloed band: cleared
        CHECK (rig.proc->getAuditionMode() == Mode::None);
        CHECK (rig.proc->getAuditionSlot() == 0);
    }
}
