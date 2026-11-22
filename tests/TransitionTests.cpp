// Changes made while audio runs, in the configurations where something is delayed (lookahead,
// "Fixed Latency", oversampling): adding and removing bands, stereo-mode changes, the engine
// switch, bypass with the clipper, audition. Each case here was a click or a dip before it was
// fixed (second independent review, M1-M3 and m1-m6).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <functional>
#include <memory>
#include <vector>

#include "dsp/A3RegionEngine.h"
#include "dsp/APrimeRegionEngine.h"
#include "dsp/Lookahead.h"
#include "dsp/Oversampler.h"
#include "dsp/Ramp.h"
#include "support/AllocationCounter.h"
#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;
using Catch::Matchers::WithinAbs;
using Kind = dsp::EngineKind;

namespace
{
constexpr double kFs = 48000.0;
constexpr int kLook = 480;              // 10 ms at 48 kHz

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
    void global (const char* id, float v)             { setPlain (*proc, id, v); }
    void restart() { REQUIRE (renderer.prepare (kFs, 512, layout)); }

    juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& in, std::vector<int> blocks = { 128 },
                                     bool bypassed = false, const OfflineRenderer::BlockHook& hook = {},
                                     const juce::AudioBuffer<float>* sidechain = nullptr)
    {
        return renderer.render (in, sidechain, blocks, bypassed, hook);
    }
};

juce::AudioBuffer<float> sineBuffer (double f, int n, double amplitude, int channels = 1)
{
    juce::AudioBuffer<float> b (channels, n);
    const auto s = sine (f, kFs, n, amplitude);
    for (int c = 0; c < channels; ++c) b.copyFrom (c, 0, s.data(), n);
    return b;
}

/** Largest sample-to-sample step a sine of this frequency and amplitude can make. */
float sineStep (double f, double amplitude) { return (float) (amplitude * 2.0 * dsp::kPi * f / kFs); }

float largestStep (const juce::AudioBuffer<float>& b, int ch, int start, int end)
{
    float m = 0.0f;
    for (int i = std::max (1, start); i < end; ++i)
        m = std::max (m, std::abs (b.getSample (ch, i) - b.getSample (ch, i - 1)));
    return m;
}

float peakIn (const juce::AudioBuffer<float>& b, int ch, int start, int end)
{
    float m = 0.0f;
    for (int i = start; i < end; ++i) m = std::max (m, std::abs (b.getSample (ch, i)));
    return m;
}

double rmsDb (const juce::AudioBuffer<float>& b, int ch, int start, int end)
{
    double acc = 0.0;
    for (int i = start; i < end; ++i) acc += (double) b.getSample (ch, i) * b.getSample (ch, i);
    return 10.0 * std::log10 (acc / (double) (end - start) + 1.0e-30);
}

/** First index >= start where out[i] != in[i - delay] (silence before the delay), or -1. */
int firstMismatch (const juce::AudioBuffer<float>& out, const juce::AudioBuffer<float>& in, int delay, int start = 0)
{
    for (int i = start; i < out.getNumSamples(); ++i)
        for (int c = 0; c < out.getNumChannels(); ++c)
            if (out.getSample (c, i) != (i >= delay ? in.getSample (c, i - delay) : 0.0f))
                return i;
    return -1;
}

/** Drives an engine directly (mono), with the settings chosen per block. */
template <typename Engine>
std::vector<float> runEngine (Engine& engine, const std::vector<float>& in, int block,
                              const std::function<dsp::EngineSettings (int)>& settingsFor)
{
    auto out = in;
    engine.setSettings (settingsFor (0));
    engine.prepare (kFs, 512);
    const int n = (int) in.size();
    for (int pos = 0, b = 0; pos < n; pos += block, ++b)
    {
        engine.setSettings (settingsFor (b));
        float* channels[1] { out.data() + pos };
        engine.process (channels, 1, std::min (block, n - pos));
    }
    return out;
}
}

// ---- M1: bands that start or stop while their gain is delayed --------------------------------------

TEST_CASE ("Adding a band while gains are delayed: nothing is replayed, no step", "[dsp][lookahead][transition]")
{
    const int n = 24000, block = 128, addBlock = 60, addAt = addBlock * block;
    const auto in = sineBuffer (219.0, n, 0.5);      // inside the default range of the second slot

    for (const Kind kind : { Kind::A3, Kind::APrime })
    {
        INFO (dsp::engineName (kind));

        SECTION (std::string ("a neutral band comes back by undo: the output stays the delayed input - ") + dsp::engineName (kind))
        {
            Rig rig (1, 2);
            for (int slot = 1; slot <= 2; ++slot) { rig.band (slot, "thresh", 12.0f); rig.band (slot, "ratio", 1.0f); }
            rig.global ("constantLatency", 1.0f);
            REQUIRE (rig.proc->getBandSlots().deleteBand (2));
            rig.proc->setEngineKind (kind);
            rig.restart();
            REQUIRE (rig.proc->getLatencySamples() == kLook);

            const auto out = rig.render (in, { block }, false, [] (int b, PluginProcessor& p)
            {
                if (b == addBlock) REQUIRE (p.getBandSlots().undoLast());
            });
            REQUIRE (rig.proc->getBandSlots().isUsed (2));
            CHECK (firstMismatch (out, in, kLook) == -1);
        }

        SECTION (std::string ("a new compressing band: unheard for the gain delay, then it fades in - ") + dsp::engineName (kind))
        {
            Rig rig (1, 1);
            rig.band (1, "thresh", 12.0f);
            rig.global ("constantLatency", 1.0f);
            rig.proc->setEngineKind (kind);
            rig.restart();

            const auto out = rig.render (in, { block }, false, [] (int b, PluginProcessor& p)
            {
                if (b == addBlock)
                {
                    const int slot = p.getBandSlots().createBand();
                    setBand (p, slot, "thresh", -40.0f);
                    setBand (p, slot, "ratio", 20.0f);
                }
            });

            // The band's gain is delayed by 10 ms like the audio: until then the output is the
            // delayed input exactly; afterwards it compresses, without a step.
            CHECK (firstMismatch (out, in, kLook) >= addAt + kLook);
            CHECK (largestStep (out, 0, addAt - block, n) < 1.2f * sineStep (219.0, 0.5));
            CHECK (rmsDb (out, 0, n - 4800, n) < rmsDb (in, 0, n - 4800, n) - 6.0);
        }
    }
}

TEST_CASE ("Removing and re-adding a band while gains are delayed", "[dsp][lookahead][transition]")
{
    // Engine level: band 2 compresses hard, is removed, and later comes back as a neutral band.
    const int n = 4 * 48000, block = 64;
    const int removeBlock = 24000 / block, addBlock = 96000 / block;
    const auto in = sine (1000.0, kFs, n, 0.5);

    const auto settingsFor = [&] (int b)
    {
        dsp::EngineSettings s;
        s.constantLatency = true;
        s.usedMask = b >= removeBlock && b < addBlock ? 1u : 3u;
        s.bands[0].fLoHz = 60.0f;  s.bands[0].fHiHz = 120.0f; s.bands[0].thresholdDb = 12.0f; s.bands[0].ratio = 1.0f;
        s.bands[1].fLoHz = 500.0f; s.bands[1].fHiHz = 2000.0f;
        s.bands[1].thresholdDb = b < addBlock ? -40.0f : 12.0f;
        s.bands[1].ratio = b < addBlock ? 20.0f : 1.0f;
        return s;
    };

    const auto check = [&] (const std::vector<float>& out)
    {
        // Compressing at first.
        double acc = 0.0;
        for (int i = 12000; i < 24000; ++i) acc += (double) out[(size_t) i] * out[(size_t) i];
        CHECK (10.0 * std::log10 (acc / 12000.0) < 10.0 * std::log10 (0.125) - 12.0);

        // The removal fades out over 10 ms, 10 ms late (the gain is delayed): no step.
        float step = 0.0f;
        for (int i = 1; i < n; ++i) step = std::max (step, std::abs (out[(size_t) i] - out[(size_t) i - 1]));
        CHECK (step < 1.2f * sineStep (1000.0, 0.5));

        // Once it is gone, and when it comes back neutral, the output is the delayed input
        // exactly: nothing of the old gain reduction is replayed.
        int mismatch = -1;
        for (int i = 24000 + 3 * kLook; i < n && mismatch < 0; ++i)
            if (out[(size_t) i] != in[(size_t) (i - kLook)]) mismatch = i;
        CHECK (mismatch == -1);
    };

    dsp::A3RegionEngine a3;
    check (runEngine (a3, in, block, settingsFor));
    dsp::APrimeRegionEngine aPrime;
    check (runEngine (aPrime, in, block, settingsFor));
}

// ---- m2 / M10: the lookahead itself changing ---------------------------------------------------------

TEST_CASE ("Lookahead raised twice in quick succession reads no history it does not have", "[dsp][lookahead][transition]")
{
    // 0 -> 0.5 ms -> 10 ms on consecutive host blocks. A constant input must stay that constant
    // (a tap that reads back further than what was collected would return silence).
    for (const int block : { 16, 32, 64, 256, 512 })
    {
        INFO ("host block " << block);
        Rig rig;
        rig.band (1, "thresh", 12.0f);
        rig.restart();

        juce::AudioBuffer<float> dc (1, 24000);
        for (int i = 0; i < 24000; ++i) dc.setSample (0, i, 0.5f);

        const auto out = rig.render (dc, { block }, false, [] (int b, PluginProcessor& p)
        {
            if (b == 20) setBand (p, 1, "lookahead", 0.5f);
            if (b == 21) setBand (p, 1, "lookahead", 10.0f);
        });

        float lowest = 1.0f, highest = 0.0f;
        for (int i = 0; i < 24000; ++i)
        {
            lowest = std::min (lowest, out.getSample (0, i));
            highest = std::max (highest, out.getSample (0, i));
        }
        CHECK (lowest == 0.5f);
        CHECK (highest == 0.5f);
        CHECK (rig.proc->getLatencySamples() == kLook);
    }
}

TEST_CASE ("A lookahead change is a 10 ms crossfade", "[dsp][lookahead][transition]")
{
    // A rising ramp: delayed by d samples it is lower by d * slope, so the difference to the
    // input shows the crossfade position directly.
    const int n = 48000, block = 128;
    const float slope = 1.0e-5f;
    juce::AudioBuffer<float> in (1, n);
    for (int i = 0; i < n; ++i) in.setSample (0, i, slope * (float) i);

    Rig rig;
    rig.band (1, "thresh", 12.0f);
    rig.restart();
    const auto out = rig.render (in, { block }, false, [] (int b, PluginProcessor& p)
    {
        if (b == 100) setBand (p, 1, "lookahead", 10.0f);
    });

    const double full = (double) slope * kLook;
    int start = -1;
    for (int i = 0; i < n && start < 0; ++i)
        if ((double) in.getSample (0, i) - out.getSample (0, i) > 0.002 * full) start = i;
    REQUIRE (start >= 100 * block);
    REQUIRE (start < 100 * block + kLook + 2 * block);       // after the history was collected

    const auto at = [&] (int offset) { return ((double) in.getSample (0, start + offset) - out.getSample (0, start + offset)) / full; };
    CHECK_THAT (at (48), WithinAbs (0.1, 0.02));             // 1 ms in: a tenth of the way
    CHECK_THAT (at (240), WithinAbs (0.5, 0.02));
    CHECK_THAT (at (432), WithinAbs (0.9, 0.02));
    CHECK_THAT (at (600), WithinAbs (1.0, 0.002));
}

// ---- M2: stereo mode ---------------------------------------------------------------------------------

TEST_CASE ("Stereo mode changed while gains are delayed: no step", "[dsp][lookahead][stereo][transition]")
{
    const int n = 96000, block = 128, changeBlock = 200, changeAt = changeBlock * block;
    juce::AudioBuffer<float> in (2, n);
    const auto loud = sine (997.0, kFs, n, 0.5), quiet = sine (997.0, kFs, n, 0.005);
    in.copyFrom (0, 0, loud.data(), n);
    in.copyFrom (1, 0, quiet.data(), n);

    for (const Kind kind : { Kind::A3, Kind::APrime })
        for (const bool fixedLatency : { false, true })
        {
            INFO (dsp::engineName (kind) << (fixedLatency ? ", fixed latency" : ", no latency"));
            const auto configure = [&] (Rig& rig, float mode)
            {
                rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
                rig.band (1, "thresh", -30.0f); rig.band (1, "ratio", 20.0f);
                rig.band (1, "stereoMode", mode);
                rig.global ("constantLatency", fixedLatency ? 1.0f : 0.0f);
                rig.proc->setEngineKind (kind);
                rig.restart();
            };

            Rig rig (2);
            configure (rig, 1.0f);                                        // Left/Right
            const auto out = rig.render (in, { block }, false, [] (int b, PluginProcessor& p)
            {
                if (b == changeBlock) setBand (p, 1, "stereoMode", 4.0f);  // Mid/Side
            });

            for (int c = 0; c < 2; ++c)
                CHECK (largestStep (out, c, changeAt - block, changeAt + 9600) < 1.2f * sineStep (997.0, 0.5));

            // The change did happen: it ends up where a run that started in Mid/Side is.
            Rig reference (2);
            configure (reference, 4.0f);
            const auto expected = reference.render (in, { block });
            double worst = 0.0;
            for (int c = 0; c < 2; ++c)
                for (int i = n - 4800; i < n; ++i)
                    worst = std::max (worst, std::abs ((double) out.getSample (c, i) - expected.getSample (c, i)));
            CHECK (worst < 1.0e-4);
            CHECK (std::abs (rmsDb (out, 1, changeAt - 4800, changeAt) - rmsDb (out, 1, n - 4800, n)) > 3.0);   // and it is audible
        }
}

// ---- M3: engine switch -------------------------------------------------------------------------------

TEST_CASE ("Engine switch with delays in use: no dip, no step", "[dsp][engine][transition]")
{
    const int n = 96000, block = 128;
    const auto switching = [] (int b, PluginProcessor& p)
    {
        if (b == 150) p.setEngineKind (Kind::APrime);
        if (b == 450) p.setEngineKind (Kind::A3);
    };

    SECTION ("neutral band with 10 ms lookahead: the output stays the delayed input")
    {
        const auto in = sineBuffer (1000.0, n, 0.5);
        Rig rig;
        rig.band (1, "thresh", 12.0f);
        rig.band (1, "lookahead", 10.0f);
        rig.restart();
        test::resetAllocationCount();
        const auto out = rig.render (in, { block }, false, switching);
        CHECK (test::allocationCount() == 0);
        CHECK (firstMismatch (out, in, kLook) == -1);
        CHECK (rig.proc->getEngineKind() == Kind::A3);
    }

    SECTION ("lookahead, 8x oversampling and coloration at 0 %: still the delayed input")
    {
        const auto in = makeNoise (2, n, 5u, 0.3f);
        Rig rig (2);
        rig.band (1, "thresh", 12.0f);
        rig.band (1, "lookahead", 3.0f);
        rig.global ("oversampling", 3.0f);
        rig.restart();
        const int latency = rig.proc->getLatencySamples();
        REQUIRE (latency == dsp::lookaheadSamples (3.0, kFs) + dsp::oversamplingLatency (3));
        CHECK (firstMismatch (rig.render (in, { block }, false, switching), in, latency) == -1);
    }

    SECTION ("host bypass returns the delayed input throughout, whatever the settings")
    {
        // Input gain +12 dB, a band that would compress hard, fixed latency: under host bypass
        // none of it may be heard while the engines change over.
        const auto in = sineBuffer (1000.0, n, 0.25, 2);
        Rig rig (2);
        rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
        rig.band (1, "thresh", -40.0f); rig.band (1, "ratio", 20.0f);
        rig.global ("inGain", 12.0f);
        rig.global ("constantLatency", 1.0f);
        rig.restart();
        const auto out = rig.render (in, { block }, true, switching);
        // (Playback started un-bypassed, so the first 20 ms ramp to neutral.)
        CHECK (firstMismatch (out, in, kLook, 4800) == -1);
    }

    SECTION ("a compressing band: the level never dips below both engines, no step")
    {
        const auto in = sineBuffer (1000.0, n, 0.5);
        Rig rig;
        rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
        rig.band (1, "thresh", -30.0f); rig.band (1, "ratio", 4.0f);
        rig.band (1, "lookahead", 10.0f);
        rig.restart();
        const auto out = rig.render (in, { block }, false, switching);
        CHECK (largestStep (out, 0, 4800, n) < 1.2f * sineStep (1000.0, 0.5));

        // A tone at the band centre is treated the same by both engines: steady level throughout.
        const double steady = rmsDb (out, 0, 9600, 14400);
        for (int start = 150 * block; start + 480 <= 150 * block + 9600; start += 240)
            CHECK_THAT (rmsDb (out, 0, start, start + 480), WithinAbs (steady, 0.3));
    }

    SECTION ("a soloed band stays soloed")
    {
        const auto in = sineBuffer (200.0, n, 0.5);
        Rig rig;
        rig.band (1, "fLo", 2000.0f); rig.band (1, "fHi", 4000.0f);
        rig.band (1, "thresh", 12.0f);
        rig.proc->setAudition (dsp::AuditionMode::Solo, 1);
        rig.restart();
        const auto out = rig.render (in, { block }, false, switching);
        const float steady = peakIn (out, 0, 9600, 150 * block);
        REQUIRE (steady < 0.1f);                                          // 200 Hz is far below the band
        CHECK (peakIn (out, 0, 150 * block, n) < 1.1f * steady);
    }
}

TEST_CASE ("Engine switch: the engine that comes back starts from a clean state", "[dsp][engine][transition]")
{
    // A3 is left in deep gain reduction with a long release, then A-prime runs for ten seconds.
    // Back on A3 nothing of that old gain reduction may be left.
    const int block = 128, n = 12 * 48000;
    juce::AudioBuffer<float> in (1, n);
    const auto loud = sine (1000.0, kFs, n, 0.5), quiet = sine (1000.0, kFs, n, 0.002);
    for (int i = 0; i < n; ++i) in.setSample (0, i, i < 72000 ? loud[(size_t) i] : quiet[(size_t) i]);

    Rig rig;
    rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
    rig.band (1, "thresh", -40.0f); rig.band (1, "ratio", 20.0f);
    rig.band (1, "attack", 1.0f); rig.band (1, "release", 1000.0f);
    rig.restart();
    const auto out = rig.render (in, { block }, false, [] (int b, PluginProcessor& p)
    {
        if (b == 48000 / 128) p.setEngineKind (Kind::APrime);
        if (b == 480000 / 128) p.setEngineKind (Kind::A3);
    });

    CHECK (rmsDb (out, 0, 24000, 48000) < rmsDb (in, 0, 24000, 48000) - 20.0);       // deep reduction at first
    CHECK_THAT (rmsDb (out, 0, 504000, 528000) - rmsDb (in, 0, 504000, 528000), WithinAbs (0.0, 0.2));
}

// ---- m1: clipper and bypass ----------------------------------------------------------------------------

namespace
{
/** A host block index >= `from` at whose first sample |x| lies in [lo, hi]: a change made there
    is made while the clipper is doing something (at a zero crossing nothing could be heard). */
int blockWhere (const juce::AudioBuffer<float>& x, int block, int from, float lo, float hi)
{
    for (int b = from; (b + 1) * block < x.getNumSamples(); ++b)
    {
        const float a = std::abs (x.getSample (0, b * block));
        if (a >= lo && a <= hi) return b;
    }
    return -1;
}

/** Renders through the processor directly, switching to the host's bypass call at `bypassFrom`. */
juce::AudioBuffer<float> renderWithHostBypassFrom (PluginProcessor& p, const juce::AudioBuffer<float>& in, int block, int bypassFrom)
{
    juce::AudioBuffer<float> out (in.getNumChannels(), in.getNumSamples());
    juce::MidiBuffer midi;
    for (int pos = 0, b = 0; pos < in.getNumSamples(); pos += block, ++b)
    {
        const int n = std::min (block, in.getNumSamples() - pos);
        juce::AudioBuffer<float> chunk (in.getNumChannels(), n);
        for (int c = 0; c < in.getNumChannels(); ++c) chunk.copyFrom (c, 0, in, c, pos, n);
        if (b >= bypassFrom) p.processBlockBypassed (chunk, midi);
        else                 p.processBlock (chunk, midi);
        for (int c = 0; c < in.getNumChannels(); ++c) out.copyFrom (c, pos, chunk, c, 0, n);
    }
    return out;
}
}

TEST_CASE ("Clipper: bypass, switching it off and changing the curve are fades", "[dsp][clip][transition]")
{
    const int n = 48000, block = 128;
    const auto loud = sineBuffer (220.0, n, 1.0);
    const float ceiling = dsp::dbToLinear (-6.0f);
    const float own = sineStep (220.0, 1.0);                               // 0.029 per sample

    // The change is made while the signal is far above the ceiling (a cut would jump by ~0.4).
    const int changeBlock = blockWhere (loud, block, 60, 0.9f, 1.0f), changeAt = changeBlock * block;
    REQUIRE (changeBlock > 0);

    for (const float mode : { 1.0f, 2.0f })
    {
        INFO ("clip mode " << mode);

        struct Change { const char* id; float value; };
        for (const Change change : { Change { "bypass", 1.0f }, Change { "clipMode", 0.0f } })
        {
            INFO (change.id);
            Rig rig (1, 0);   // no bands: only the clipper acts
            rig.global ("clipMode", mode);
            rig.global ("clipCeiling", -6.0f);
            rig.restart();
            const auto out = rig.render (loud, { block }, false, [&change, changeBlock] (int b, PluginProcessor& p)
            {
                if (b == changeBlock) setPlain (p, change.id, change.value);
            });

            CHECK (peakIn (out, 0, 0, changeAt) <= ceiling);
            CHECK (largestStep (out, 0, 0, n) < 1.05f * own);
            CHECK (std::abs (out.getSample (0, changeAt + 96)) < 0.6f);       // 2 ms later it is still mostly clipped
            for (int i = changeAt + 960 + block; i < n; ++i)               // 20 ms later: the input, exactly
            {
                INFO ("sample " << i << ", change at " << changeAt);
                REQUIRE (out.getSample (0, i) == loud.getSample (0, i));
            }
        }

        // The host's bypass, engaged at the same moment.
        Rig rig (1, 0);   // no bands: only the clipper acts
        rig.global ("clipMode", mode);
        rig.global ("clipCeiling", -6.0f);
        rig.restart();
        const auto bypassed = renderWithHostBypassFrom (*rig.proc, loud, block, changeBlock);
        CHECK (peakIn (bypassed, 0, 0, changeAt) <= ceiling);
        CHECK (largestStep (bypassed, 0, 0, n) < 1.05f * own);
        for (int i = changeAt + 960 + block; i < n; ++i)
            REQUIRE (bypassed.getSample (0, i) == loud.getSample (0, i));
    }

    // Soft <-> hard: the ceiling holds during the change, and there is no step. The change is
    // made where the two curves differ most (|x| about the ceiling: soft 0.44, hard 0.5).
    const int curveBlock = blockWhere (loud, block, 60, 0.48f, 0.56f), curveAt = curveBlock * block;
    REQUIRE (curveBlock > 0);
    REQUIRE (std::abs (dsp::hardClip (loud.getSample (0, curveAt), ceiling) - dsp::softClip (loud.getSample (0, curveAt), ceiling)) > 1.5f * own);

    for (const float from : { 1.0f, 2.0f })
    {
        Rig rig (1, 0);   // no bands: only the clipper acts
        rig.global ("clipMode", from);
        rig.global ("clipCeiling", -6.0f);
        rig.restart();
        const float to = from == 1.0f ? 2.0f : 1.0f;
        const auto out = rig.render (loud, { block }, false, [to, curveBlock] (int b, PluginProcessor& p)
        {
            if (b == curveBlock) setPlain (p, "clipMode", to);
        });
        CHECK (peakIn (out, 0, 0, n) <= ceiling);
        CHECK (largestStep (out, 0, 0, n) < 1.05f * own);

        // Afterwards it is the other curve exactly.
        Rig reference (1, 0);
        reference.global ("clipMode", to);
        reference.global ("clipCeiling", -6.0f);
        reference.restart();
        const auto expected = reference.render (loud, { block });
        for (int i = curveAt + 960 + block; i < n; ++i)
            REQUIRE (out.getSample (0, i) == expected.getSample (0, i));
    }
}

// ---- m3: A-prime with delayed gains ----------------------------------------------------------------------

TEST_CASE ("A-prime with Fixed Latency is A-prime without it, 10 ms later, from the first sample", "[dsp][aprime][lookahead][transition]")
{
    const int n = 48000;
    const auto in = makeNoise (2, n, 23u, 0.3f);
    const auto run = [&] (bool fixedLatency)
    {
        Rig rig (2);
        rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
        rig.band (1, "thresh", -45.0f); rig.band (1, "ratio", 20.0f);
        rig.global ("constantLatency", fixedLatency ? 1.0f : 0.0f);
        rig.proc->setEngineKind (Kind::APrime);
        rig.restart();
        return rig.render (in, { 128 });
    };

    const auto direct = run (false), delayed = run (true);
    CHECK (firstMismatch (delayed, direct, kLook) == -1);
    CHECK (rmsDb (direct, 0, n / 2, n) < rmsDb (in, 0, n / 2, n) - 1.0);      // it does compress
}

// ---- m4: audition ---------------------------------------------------------------------------------------

TEST_CASE ("Audition goes through the clipper and stays aligned with the output", "[dsp][audition][transition]")
{
    const int n = 24000;
    const float ceiling = dsp::dbToLinear (-6.0f);

    SECTION ("solo of a band with a lot of makeup cannot exceed the ceiling")
    {
        Rig rig;
        rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
        rig.band (1, "thresh", 12.0f); rig.band (1, "makeup", 24.0f);
        rig.global ("clipMode", 2.0f);
        rig.global ("clipCeiling", -6.0f);
        rig.proc->setAudition (dsp::AuditionMode::Solo, 1);
        rig.restart();
        const auto out = rig.render (sineBuffer (1000.0, n, 0.9));
        CHECK (peakIn (out, 0, 0, n) == ceiling);
    }

    SECTION ("listening to a hot external sidechain cannot exceed the ceiling")
    {
        Rig rig (1, 1, 1);
        rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
        rig.band (1, "scSource", 1.0f);
        rig.global ("clipMode", 2.0f);
        rig.global ("clipCeiling", -6.0f);
        rig.proc->setAudition (dsp::AuditionMode::Listen, 1);
        rig.restart();
        const auto sidechain = sineBuffer (1000.0, n, 3.0);
        const auto out = rig.render (sineBuffer (1000.0, n, 0.1), { 128 }, false, {}, &sidechain);
        CHECK (peakIn (out, 0, 0, n) == ceiling);
    }

    struct Case { dsp::AuditionMode mode; float lookaheadMs; float oversampling; };
    for (const Case c : { Case { dsp::AuditionMode::Solo, 0.0f, 3.0f },
                          Case { dsp::AuditionMode::Solo, 5.0f, 2.0f },
                          Case { dsp::AuditionMode::Listen, 5.0f, 0.0f },
                          Case { dsp::AuditionMode::Listen, 10.0f, 3.0f } })
    {
        INFO ((c.mode == dsp::AuditionMode::Solo ? "solo" : "listen") << ", lookahead " << c.lookaheadMs << " ms, oversampling " << c.oversampling);
        const auto in = makeNoise (2, n, 31u, 0.3f);
        const auto run = [&] (bool delayed)
        {
            Rig rig (2);
            rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
            rig.band (1, "thresh", -30.0f); rig.band (1, "ratio", 4.0f);
            if (delayed)
            {
                rig.band (1, "lookahead", c.lookaheadMs);
                rig.global ("oversampling", c.oversampling);
            }
            rig.proc->setAudition (c.mode, 1);
            rig.restart();
            auto out = rig.render (in, { 128 });
            return std::pair { out, rig.proc->getLatencySamples() };
        };

        const auto [plain, zero] = run (false);
        const auto [late, latency] = run (true);
        REQUIRE (zero == 0);
        REQUIRE (latency == dsp::lookaheadSamples ((double) c.lookaheadMs, kFs) + dsp::oversamplingLatency ((int) c.oversampling));

        if (c.mode == dsp::AuditionMode::Listen)
        {
            // The detector signal does not depend on the lookahead: the same signal, later.
            CHECK (firstMismatch (late, plain, latency, 4800) == -1);
        }
        else
        {
            // Solo with lookahead is a different signal (the gain leads); what must hold is that
            // it lines up with the normal output: solo + (everything else) = output, i.e. the
            // band's component is where the plugin's latency says it is.
            Rig rig (2);
            rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
            rig.band (1, "thresh", 12.0f); rig.band (1, "makeup", -6.0f);
            rig.band (1, "lookahead", c.lookaheadMs);
            rig.global ("oversampling", c.oversampling);
            rig.restart();
            const auto normal = rig.render (in, { 128 });
            rig.proc->setAudition (dsp::AuditionMode::Solo, 1);
            rig.restart();
            const auto solo = rig.render (in, { 128 });

            // normal = x_delayed + (g - 1) c, solo = g c  =>  normal - x_delayed = solo (g - 1) / g, g = 0.5.
            const double g = (double) dsp::dbToLinear (-6.0f);
            double worst = 0.0;
            for (int ch = 0; ch < 2; ++ch)
                for (int i = 4800; i < n; ++i)
                {
                    const double change = (double) normal.getSample (ch, i) - in.getSample (ch, i - latency);
                    worst = std::max (worst, std::abs (change - (double) solo.getSample (ch, i) * (g - 1.0) / g));
                }
            CHECK (worst < 1.0e-5);
            (void) late; (void) plain;
        }
    }
}

TEST_CASE ("Audition switched on while its signal is delayed: it fades in, it does not start abruptly", "[dsp][audition][transition]")
{
    // 10 ms lookahead plus 8x oversampling: the audition signal arrives 535 samples late, which
    // is longer than the 10 ms fade. Without waiting for it the fade would end in silence and
    // the signal would then start at full level in the middle of a cycle.
    const int n = 24000, block = 128, onBlock = 60;
    const auto in = sineBuffer (1030.0, n, 0.5);                 // not at a zero crossing when it would start
    REQUIRE (std::abs (in.getSample (0, onBlock * block)) > 0.4f);

    for (const auto mode : { dsp::AuditionMode::Listen, dsp::AuditionMode::Solo })
    {
        INFO ((mode == dsp::AuditionMode::Solo ? "solo" : "listen"));
        Rig rig;
        rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
        rig.band (1, "thresh", 12.0f);
        rig.band (1, "lookahead", 10.0f);
        rig.global ("oversampling", 3.0f);
        rig.restart();
        const auto out = rig.render (in, { block }, false, [mode] (int b, PluginProcessor& p)
        {
            if (b == onBlock) p.setAudition (mode, 1);
        });

        CHECK (largestStep (out, 0, 4800, n) < 1.2f * sineStep (1030.0, 0.5));
        CHECK (peakIn (out, 0, n - 4800, n) > 0.15f);            // and the audition did come on
    }
}

// ---- m5: detector channels on a mono track ------------------------------------------------------------------

TEST_CASE ("Mono: the detector channel choice never switches the compression off", "[dsp][routing][transition]")
{
    const int n = 24000;
    const auto in = sineBuffer (1000.0, n, 0.5);
    const auto run = [&] (float channelMode)
    {
        Rig rig;
        rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
        rig.band (1, "thresh", -30.0f); rig.band (1, "ratio", 4.0f);
        rig.band (1, "scChan", channelMode);
        rig.restart();
        return rig.render (in);
    };

    const auto perChannel = run (0.0f);
    CHECK (rmsDb (perChannel, 0, n / 2, n) < rmsDb (in, 0, n / 2, n) - 6.0);
    CHECK (bitIdentical (run (1.0f), perChannel));     // Mid
    CHECK (bitIdentical (run (2.0f), perChannel));     // Side: a mono signal has none, so it means the same
    CHECK (bitIdentical (run (3.0f), perChannel));     // Max
    CHECK (rmsDb (run (4.0f), 0, n / 2, n) < rmsDb (perChannel, 0, n / 2, n) - 1.0);   // Sum: 6 dB hotter detector
}

// ---- m6: meters after the editor was closed --------------------------------------------------------------------

TEST_CASE ("Meters start afresh when an editor opens again", "[meter][transition]")
{
    Rig rig (2, 0);
    auto& p = *rig.proc;
    const auto tone = sineBuffer (997.0, 5 * 48000, 0.5, 2);
    juce::AudioBuffer<float> silence (2, 10 * 48000), shortSilence (2, 12000);
    silence.clear();
    shortSilence.clear();

    p.addAnalyzerClient();
    rig.render (tone, { 512 });
    CHECK (p.getLoudnessMeter().getMomentaryLufs() > -12.0f);
    const float integrated = p.getLoudnessMeter().getIntegratedLufs();
    const float truePeak = p.getLoudnessMeter().getTruePeakDb();
    p.removeAnalyzerClient();

    rig.render (silence, { 512 });                     // nobody is measuring

    p.addAnalyzerClient();
    rig.render (shortSilence, { 512 });                // 250 ms of silence with the editor open again
    CHECK (p.getLoudnessMeter().getMomentaryLufs() == rt::LoudnessMeter::silenceLufs);
    CHECK (p.getLoudnessMeter().getShortTermLufs() == rt::LoudnessMeter::silenceLufs);
    CHECK (p.getOutputMeter().getRmsDb (0) == rt::LevelMeter::silenceDb);
    CHECK (p.getInputMeter().getRmsDb (1) == rt::LevelMeter::silenceDb);
    // What was measured while an editor was open is kept.
    CHECK (p.getLoudnessMeter().getIntegratedLufs() == integrated);
    CHECK (p.getLoudnessMeter().getTruePeakDb() == truePeak);
    p.removeAnalyzerClient();
}
