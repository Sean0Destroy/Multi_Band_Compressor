// Detector routing and stereo modes through the plugin processor: external sidechain,
// follow-band and custom detector ranges, detector channel modes, L/R and mid/side processing.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kFs = 48000.0;

struct Rig
{
    std::unique_ptr<PluginProcessor> proc = std::make_unique<PluginProcessor>();
    OfflineRenderer renderer { *proc };
    Layout layout;

    Rig (int mainChannels, int sidechainChannels, int bands = 1) : layout { mainChannels, sidechainChannels }
    {
        for (int i = 0; i < bands; ++i) proc->getBandSlots().createBand();
        REQUIRE (renderer.prepare (kFs, 512, layout));
    }

    void band (int slot, const char* suffix, float v) { setBand (*proc, slot, suffix, v); }
    void restart() { REQUIRE (renderer.prepare (kFs, 512, layout)); }

    juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& in, const juce::AudioBuffer<float>* sc = nullptr,
                                     std::vector<int> blocks = { 128 }, const OfflineRenderer::BlockHook& hook = {})
    {
        return renderer.render (in, sc, blocks, false, hook);
    }

    float gr (int slot, int lane = 0) const { return proc->getGainReductionMeter().getGainReductionDb (slot, lane); }
};

juce::AudioBuffer<float> tones (int channels, int n, std::initializer_list<std::pair<double, double>> freqAmp)
{
    juce::AudioBuffer<float> b (channels, n);
    b.clear();
    for (auto [f, a] : freqAmp)
    {
        const auto s = sine (f, kFs, n, a);
        for (int c = 0; c < channels; ++c) b.addFrom (c, 0, s.data(), n);
    }
    return b;
}

/** Amplitude of the component at f (single-bin DTFT over [start, end)). */
double toneAmplitude (const juce::AudioBuffer<float>& b, int ch, double f, int start, int end)
{
    double re = 0.0, im = 0.0;
    for (int i = start; i < end; ++i)
    {
        const double ph = 2.0 * dsp::kPi * f * (double) i / kFs;
        re += (double) b.getSample (ch, i) * std::cos (ph);
        im += (double) b.getSample (ch, i) * std::sin (ph);
    }
    return 2.0 * std::sqrt (re * re + im * im) / (double) (end - start);
}

bool allFinite (const juce::AudioBuffer<float>& b)
{
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! std::isfinite (b.getSample (c, i))) return false;
    return true;
}

bool allZero (const juce::AudioBuffer<float>& b)
{
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = 0; i < b.getNumSamples(); ++i)
            if (! (b.getSample (c, i) == 0.0f)) return false;
    return true;
}

double maxAbsDiff (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
{
    double m = 0.0;
    for (int c = 0; c < a.getNumChannels(); ++c)
        for (int i = 0; i < a.getNumSamples(); ++i)
            m = std::max (m, std::abs ((double) a.getSample (c, i) - (double) b.getSample (c, i)));
    return m;
}

// scSource / scMode / scChan / stereoMode choice indices (schema order).
constexpr float kMain = 0.0f, kExternal = 1.0f;
constexpr float kOwn = 0.0f, kBand = 1.0f, kCustom = 2.0f;
constexpr float kPerChannel = 0.0f, kMid = 1.0f, kSide = 2.0f, kMax = 3.0f, kSum = 4.0f;
constexpr float kStereo = 0.0f, kLeftRight = 1.0f, kMidOnly = 2.0f, kSideOnly = 3.0f, kMidSide = 4.0f;
constexpr int kN = 48000;
}

TEST_CASE ("External sidechain drives the gain reduction and never reaches the output", "[dsp][routing]")
{
    const int scChannels = GENERATE (1, 2);
    INFO ("sidechain channels " << scChannels);

    Rig rig (2, scChannels);
    rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
    rig.band (1, "thresh", -30.0f); rig.band (1, "ratio", 4.0f);
    const double fc = digitalCentre (500.0, 2000.0, kFs);

    const auto quiet = tones (2, kN, { { fc, 0.003 } });            // about -50 dBFS: below the threshold
    const auto loudSc = tones (scChannels, kN, { { fc, 0.5 } });

    // Main source: the quiet programme does not trigger, whatever is on the sidechain.
    rig.band (1, "scSource", kMain);
    rig.restart();
    CHECK (bitIdentical (rig.render (quiet, &loudSc), quiet));
    CHECK (rig.gr (1) == 0.0f);

    // External source: the loud sidechain compresses the quiet programme.
    rig.band (1, "scSource", kExternal);
    rig.restart();
    const auto ducked = rig.render (quiet, &loudSc);
    CHECK (rig.gr (1) < -10.0f);
    CHECK (rig.gr (1, 0) == rig.gr (1, 1));                         // mono sidechain is duplicated; stereo is identical here
    CHECK (toneAmplitude (ducked, 0, fc, kN / 2, kN) < 0.5 * 0.003);

    // Leakage: with a silent main input the output is silent, even while the band works hard.
    juce::AudioBuffer<float> silence (2, kN);
    silence.clear();
    rig.restart();
    CHECK (allZero (rig.render (silence, &loudSc)));
    CHECK (rig.gr (1) < -10.0f);
}

TEST_CASE ("External source without a sidechain bus gives 0 dB reduction (no fallback to the main input)", "[dsp][routing]")
{
    Rig rig (2, 0);
    rig.band (1, "thresh", -50.0f); rig.band (1, "ratio", 10.0f);
    rig.band (1, "fLo", 100.0f); rig.band (1, "fHi", 8000.0f);
    const auto loud = makeNoise (2, kN, 4u, 0.7f);

    rig.band (1, "scSource", kMain);
    rig.restart();
    CHECK_FALSE (bitIdentical (rig.render (loud), loud));
    CHECK (rig.gr (1) < -6.0f);

    rig.band (1, "scSource", kExternal);
    rig.restart();
    CHECK (bitIdentical (rig.render (loud), loud));
    CHECK (rig.gr (1) == 0.0f);
}

TEST_CASE ("Non-finite sidechain samples are removed before the detector", "[dsp][routing]")
{
    Rig rig (2, 2);
    rig.band (1, "scSource", kExternal);
    rig.band (1, "thresh", -40.0f);
    rig.restart();

    const auto in = makeNoise (2, 9600, 6u, 0.5f);
    auto sc = makeNoise (2, 9600, 7u, 0.5f);
    sc.setSample (0, 100, std::numeric_limits<float>::quiet_NaN());
    sc.setSample (1, 2000, std::numeric_limits<float>::infinity());
    sc.setSample (0, 5000, -std::numeric_limits<float>::infinity());

    const auto out = rig.render (in, &sc);
    CHECK (allFinite (out));
    CHECK (rig.proc->getEngine().getStateResetCount() == 0u);
    CHECK (rig.gr (1) < -1.0f);
}

TEST_CASE ("Detector range: follow another band, custom range, and deleting the followed band", "[dsp][routing]")
{
    // Band 1 (100 - 400 Hz) never compresses; band 2 (2 - 8 kHz) is the one under test.
    // Programme: loud 200 Hz, quiet 4 kHz.
    const auto configure = [] (Rig& r)
    {
        r.band (1, "fLo", 100.0f);  r.band (1, "fHi", 400.0f);  r.band (1, "thresh", 12.0f);
        r.band (2, "fLo", 2000.0f); r.band (2, "fHi", 8000.0f);
        r.band (2, "thresh", -30.0f); r.band (2, "ratio", 4.0f);
    };
    const auto in = tones (1, kN, { { 200.0, 0.5 }, { 4000.0, 0.003 } });

    Rig own (1, 0, 2);
    configure (own);
    own.restart();
    const auto outOwn = own.render (in);
    CHECK (own.gr (2) == 0.0f);                                   // its own range only holds the quiet 4 kHz

    Rig follow (1, 0, 2);
    configure (follow);
    follow.band (2, "scMode", kBand);
    follow.band (2, "scRef", 1.0f);
    follow.restart();
    const auto outFollow = follow.render (in);
    CHECK (follow.gr (2) < -10.0f);                               // band 1's range holds the loud 200 Hz
    CHECK (toneAmplitude (outFollow, 0, 4000.0, kN / 2, kN) < 0.5 * toneAmplitude (outOwn, 0, 4000.0, kN / 2, kN));
    CHECK_THAT (toneAmplitude (outFollow, 0, 200.0, kN / 2, kN), WithinAbs (0.5, 0.01));   // 200 Hz itself is not in band 2

    // The same range typed in as a custom range is the same detector.
    Rig custom (1, 0, 2);
    configure (custom);
    custom.band (2, "scMode", kCustom);
    custom.band (2, "scLo", 100.0f);
    custom.band (2, "scHi", 400.0f);
    custom.restart();
    CHECK (bitIdentical (custom.render (in), outFollow));

    // A custom range equal to the band's own range is the same as "Own".
    Rig customOwn (1, 0, 2);
    configure (customOwn);
    customOwn.band (2, "scMode", kCustom);
    customOwn.band (2, "scLo", 2000.0f);
    customOwn.band (2, "scHi", 8000.0f);
    customOwn.restart();
    CHECK (bitIdentical (customOwn.render (in), outOwn));

    // Deleting the followed band converts the follower to that band's range: same sound.
    REQUIRE (follow.proc->getBandSlots().deleteBand (1));
    CHECK (follow.proc->getState().getRawParameterValue ("b2_scMode")->load() == kCustom);
    CHECK_THAT ((double) follow.proc->getState().getRawParameterValue ("b2_scLo")->load(), WithinAbs (100.0, 1e-3));
    CHECK_THAT ((double) follow.proc->getState().getRawParameterValue ("b2_scHi")->load(), WithinAbs (400.0, 1e-3));
    follow.restart();
    CHECK (bitIdentical (follow.render (in), outFollow));

    // A reference to a slot that does not exist (9..16) falls back to the band's own range.
    Rig dangling (1, 0, 2);
    configure (dangling);
    dangling.band (2, "scMode", kBand);
    dangling.band (2, "scRef", 12.0f);
    dangling.restart();
    CHECK (bitIdentical (dangling.render (in), outOwn));
}

TEST_CASE ("Detector channel modes", "[dsp][routing]")
{
    const double fc = digitalCentre (500.0, 2000.0, kFs);
    const auto configure = [] (Rig& r, float chan, float thresh = -30.0f)
    {
        r.band (1, "fLo", 500.0f); r.band (1, "fHi", 2000.0f);
        r.band (1, "thresh", thresh); r.band (1, "ratio", 4.0f); r.band (1, "knee", 0.0f);
        r.band (1, "link", 0.0f);
        r.band (1, "scChan", chan);
        r.restart();
    };

    const auto s = sine (fc, kFs, kN, 0.4);
    juce::AudioBuffer<float> leftOnly (2, kN), inPhase (2, kN), outOfPhase (2, kN);
    leftOnly.clear();
    leftOnly.copyFrom (0, 0, s.data(), kN);
    inPhase.copyFrom (0, 0, s.data(), kN);    inPhase.copyFrom (1, 0, s.data(), kN);
    outOfPhase.copyFrom (0, 0, s.data(), kN); outOfPhase.copyFrom (1, 0, s.data(), kN);
    outOfPhase.applyGain (1, 0, kN, -1.0f);

    SECTION ("per channel (unlinked): only the loud channel is reduced; Max reduces both alike")
    {
        Rig per (2, 0); configure (per, kPerChannel);
        per.render (leftOnly);
        CHECK (per.gr (1, 0) < -6.0f);
        CHECK (per.gr (1, 1) == 0.0f);

        Rig max (2, 0); configure (max, kMax);
        max.render (leftOnly);
        CHECK (max.gr (1, 0) == per.gr (1, 0));
        CHECK (max.gr (1, 1) == max.gr (1, 0));
    }

    SECTION ("Mid ignores an out-of-phase signal, Side ignores an in-phase one")
    {
        Rig mid (2, 0); configure (mid, kMid);
        mid.render (outOfPhase);
        CHECK (mid.gr (1, 0) == 0.0f);
        mid.restart();
        mid.render (inPhase);
        CHECK (mid.gr (1, 0) < -6.0f);
        CHECK (mid.gr (1, 1) == mid.gr (1, 0));

        Rig side (2, 0); configure (side, kSide);
        side.render (inPhase);
        CHECK (side.gr (1, 0) == 0.0f);
        side.restart();
        side.render (outOfPhase);
        CHECK_THAT ((double) side.gr (1, 0), WithinAbs ((double) mid.gr (1, 0), 0.01));
    }

    SECTION ("Sum is 6 dB hotter than Mid for an in-phase signal")
    {
        Rig sum (2, 0); configure (sum, kSum, -30.0f);
        sum.render (inPhase);
        Rig mid (2, 0); configure (mid, kMid, -30.0f - 6.0206f);
        mid.render (inPhase);
        CHECK_THAT ((double) sum.gr (1, 0), WithinAbs ((double) mid.gr (1, 0), 0.02));
        CHECK (sum.gr (1, 0) < -6.0f);
    }

    SECTION ("mono input: every mode detects the single channel")
    {
        const auto mono = tones (1, kN, { { fc, 0.4 } });
        Rig reference (1, 0); configure (reference, kPerChannel);
        const auto expected = reference.render (mono);
        for (float chan : { kMid, kMax })
        {
            Rig r (1, 0); configure (r, chan);
            CHECK (bitIdentical (r.render (mono), expected));
        }
    }
}

TEST_CASE ("Stereo modes: left/right, mid only, side only, mid/side", "[dsp][routing]")
{
    const auto configure = [] (Rig& r, float mode, float link = 100.0f)
    {
        r.band (1, "fLo", 300.0f); r.band (1, "fHi", 3000.0f);
        r.band (1, "thresh", -40.0f); r.band (1, "ratio", 8.0f);
        r.band (1, "link", link);
        r.band (1, "stereoMode", mode);
        r.restart();
    };

    // Independent mid and side content.
    const auto mid = makeNoise (1, kN, 31u, 0.4f);
    const auto side = makeNoise (1, kN, 32u, 0.2f);
    juce::AudioBuffer<float> in (2, kN);
    for (int i = 0; i < kN; ++i)
    {
        in.setSample (0, i, mid.getSample (0, i) + side.getSample (0, i));
        in.setSample (1, i, mid.getSample (0, i) - side.getSample (0, i));
    }
    const auto component = [] (const juce::AudioBuffer<float>& b, bool wantSide)
    {
        juce::AudioBuffer<float> out (1, b.getNumSamples());
        for (int i = 0; i < b.getNumSamples(); ++i)
            out.setSample (0, i, 0.5f * (b.getSample (0, i) + (wantSide ? -1.0f : 1.0f) * b.getSample (1, i)));
        return out;
    };
    const auto inMid = component (in, false), inSide = component (in, true);

    SECTION ("Left/Right equals Stereo with the link at 0 %")
    {
        Rig a (2, 0); configure (a, kLeftRight, 100.0f);          // link is ignored
        Rig b (2, 0); configure (b, kStereo, 0.0f);
        CHECK (bitIdentical (a.render (in), b.render (in)));
    }

    SECTION ("Mid only leaves the side component alone; Side only leaves the mid alone")
    {
        Rig m (2, 0); configure (m, kMidOnly);
        const auto outM = m.render (in);
        CHECK (maxAbsDiff (component (outM, true), inSide) < 2.0e-6);
        CHECK (maxAbsDiff (component (outM, false), inMid) > 0.05);
        CHECK (m.gr (1, 0) < -6.0f);
        CHECK (m.gr (1, 1) == 0.0f);                              // lane 1 (side) is idle

        Rig s (2, 0); configure (s, kSideOnly);
        const auto outS = s.render (in);
        CHECK (maxAbsDiff (component (outS, false), inMid) < 2.0e-6);
        CHECK (maxAbsDiff (component (outS, true), inSide) > 0.02);
        CHECK (s.gr (1, 0) == 0.0f);
        CHECK (s.gr (1, 1) < -3.0f);
    }

    SECTION ("Mid/Side compresses both components independently")
    {
        // A3: the two lanes are fully independent. (A-prime's two lanes share one filter width,
        // so there each lane's shape also follows the other's gain.)
        Rig ms (2, 0); ms.proc->setEngineKind (dsp::EngineKind::A3); configure (ms, kMidSide);
        const auto out = ms.render (in);
        CHECK (maxAbsDiff (component (out, false), inMid) > 0.05);
        CHECK (maxAbsDiff (component (out, true), inSide) > 0.02);
        CHECK (ms.gr (1, 0) < ms.gr (1, 1));                      // the louder mid is reduced more

        // Each component is treated like a mono signal on its own.
        Rig monoMid (1, 0); monoMid.proc->setEngineKind (dsp::EngineKind::A3); configure (monoMid, kStereo);
        CHECK (maxAbsDiff (component (out, false), monoMid.render (inMid)) < 5.0e-6);
        Rig monoSide (1, 0); monoSide.proc->setEngineKind (dsp::EngineKind::A3); configure (monoSide, kStereo);
        CHECK (maxAbsDiff (component (out, true), monoSide.render (inSide)) < 5.0e-6);
    }

    SECTION ("mono input ignores the stereo mode")
    {
        Rig reference (1, 0); configure (reference, kStereo);
        const auto expected = reference.render (inMid);
        for (float mode : { kLeftRight, kMidOnly, kSideOnly, kMidSide })
        {
            Rig r (1, 0); configure (r, mode);
            CHECK (bitIdentical (r.render (inMid), expected));
        }
    }

    SECTION ("a neutral mid/side band is bit-transparent")
    {
        Rig r (2, 0);
        r.band (1, "thresh", 12.0f);
        r.band (1, "stereoMode", kMidSide);
        r.restart();
        CHECK (bitIdentical (r.render (in), in));
    }
}

TEST_CASE ("Changing the stereo mode while compressing fades the band out and in again", "[dsp][routing]")
{
    Rig rig (2, 0);
    rig.band (1, "fLo", 300.0f); rig.band (1, "fHi", 3000.0f);
    rig.band (1, "thresh", -40.0f); rig.band (1, "ratio", 8.0f); rig.band (1, "link", 0.0f);
    rig.restart();

    auto in = tones (2, kN, { { 900.0, 0.4 } });
    in.applyGain (1, 0, kN, 0.3f);                                // different levels left and right

    const int changeBlock = 150, block = 128, changeAt = changeBlock * block;
    const auto out = rig.render (in, nullptr, { block }, [] (int b, PluginProcessor& p)
    {
        if (b == changeBlock) setBand (p, 1, "stereoMode", kMidSide);
    });
    REQUIRE (allFinite (out));

    // The change the band makes (output - input) has no step: its largest sample-to-sample move
    // during the transition stays close to what the steady states show.
    const auto largestMove = [&] (int start, int end)
    {
        double m = 0.0;
        for (int i = start + 1; i < end; ++i)
            for (int c = 0; c < 2; ++c)
            {
                const double d1 = (double) out.getSample (c, i) - in.getSample (c, i);
                const double d0 = (double) out.getSample (c, i - 1) - in.getSample (c, i - 1);
                m = std::max (m, std::abs (d1 - d0));
            }
        return m;
    };
    const double steady = std::max (largestMove (changeAt - 4800, changeAt), largestMove (kN - 4800, kN));
    CHECK (largestMove (changeAt, changeAt + 4800) < 1.5 * steady + 1.0e-4);

    // In between, the band is completely out for a moment (the lanes are swapped while silent).
    bool touchedNeutral = false;
    for (int i = changeAt; i < changeAt + 2400 && ! touchedNeutral; ++i)
        touchedNeutral = out.getSample (0, i) == in.getSample (0, i) && out.getSample (1, i) == in.getSample (1, i);
    CHECK (touchedNeutral);

    // Afterwards it behaves like a band that was in mid/side mode from the start.
    Rig fresh (2, 0);
    fresh.band (1, "fLo", 300.0f); fresh.band (1, "fHi", 3000.0f);
    fresh.band (1, "thresh", -40.0f); fresh.band (1, "ratio", 8.0f); fresh.band (1, "link", 0.0f);
    fresh.band (1, "stereoMode", kMidSide);
    fresh.restart();
    fresh.render (in);
    CHECK_THAT ((double) rig.gr (1, 0), WithinAbs ((double) fresh.gr (1, 0), 0.05));
    CHECK_THAT ((double) rig.gr (1, 1), WithinAbs ((double) fresh.gr (1, 1), 0.05));
}

TEST_CASE ("Switching the detector source crossfades over 5 ms", "[dsp][routing]")
{
    Rig rig (1, 1);
    rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
    rig.band (1, "thresh", -40.0f); rig.band (1, "ratio", 100.0f); rig.band (1, "knee", 0.0f);
    rig.band (1, "attack", 0.01f); rig.band (1, "range", 48.0f);
    rig.restart();

    const double fc = digitalCentre (500.0, 2000.0, kFs);
    juce::AudioBuffer<float> silentMain (1, kN);
    silentMain.clear();
    const auto loudSc = tones (1, kN, { { fc, 0.5 } });

    std::vector<float> trace ((size_t) kN, 1.0f);
    dsp::GainTrace tr;
    tr.data[0][0] = trace.data();
    tr.capacity = kN;
    rig.proc->getEngine().setGainTrace (&tr);

    const int changeBlock = 100, block = 96, changeAt = changeBlock * block;
    rig.render (silentMain, &loudSc, { block }, [] (int b, PluginProcessor& p)
    {
        if (b == changeBlock) setBand (p, 1, "scSource", kExternal);
    });
    rig.proc->getEngine().setGainTrace (nullptr);

    const auto grDb = [&trace] (int i) { return 20.0 * std::log10 ((double) trace[(size_t) i]); };
    CHECK (grDb (changeAt - 1) == 0.0);
    const double finalGr = grDb (kN - 1);
    CHECK (finalGr < -25.0);

    // With an instant switch and a 0.01 ms attack the reduction would be near its final value
    // within a millisecond. With the 5 ms crossfade the detector sees 20 % of the level after
    // 1 ms (14 dB lower), so the reduction is still far from final; after 6 ms it has arrived.
    CHECK (grDb (changeAt + 48) > finalGr + 8.0);
    CHECK (grDb (changeAt + 96) > grDb (changeAt + 192));         // still growing
    CHECK_THAT (grDb (changeAt + 6 * 48 + 96), WithinAbs (finalGr, 1.0));
}
