// Regression tests for behaviour that a mutation-testing review found unprotected: each test
// case below fails when the one line it names is changed as described ("mutation") and passes
// on the real code. The numbers in the comments ("real" / "mutated") were measured by applying
// the mutation (48 kHz, Release build), and they are what the tolerances are derived from.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

#include "dsp/OutputStage.h"
#include "rt/LoudnessMeter.h"
#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;
using Catch::Matchers::WithinAbs;
using Kind = dsp::EngineKind;
using Mode = dsp::AuditionMode;

namespace
{
constexpr double kFs = 48000.0;
constexpr float kLeftRight = 1.0f, kMidSide = 4.0f;      // stereoMode choice indices (schema order)
constexpr int kAuditionFade = 480 + 256;                 // 10 ms crossfade plus a block of slack

struct Rig
{
    std::unique_ptr<PluginProcessor> proc = std::make_unique<PluginProcessor>();
    OfflineRenderer renderer { *proc };
    Layout layout;

    explicit Rig (int channels, Kind kind = Kind::A3) : layout { channels, 0 }
    {
        proc->setEngineKind (kind);                      // before prepare: no engine crossfade
        proc->getBandSlots().createBand();
        REQUIRE (renderer.prepare (kFs, 512, layout));
    }

    void band (int slot, const char* suffix, float v) { setBand (*proc, slot, suffix, v); }
    void global (const char* id, float v)             { setPlain (*proc, id, v); }
    void restart() { REQUIRE (renderer.prepare (kFs, 512, layout)); }

    juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& in, const OfflineRenderer::BlockHook& hook = {})
    {
        return renderer.render (in, nullptr, { 128 }, false, hook);
    }

    float autoGainDb() const { return proc->getEngine().getAutoGainDb(); }
};

/** Records the effective gain of band 1 (both lanes) while it is alive. */
struct Trace
{
    std::vector<float> lane[2];
    dsp::GainTrace hook;
    dsp::IBandEngine& engine;

    Trace (Rig& rig, int n) : engine (rig.proc->getEngine())
    {
        for (int c = 0; c < 2; ++c)
        {
            lane[c].assign ((size_t) n, 1.0f);
            hook.data[0][c] = lane[c].data();
        }
        hook.capacity = n;
        engine.setGainTrace (&hook);
    }

    ~Trace() { engine.setGainTrace (nullptr); }

    double db (int laneIndex, int i) const { return 20.0 * std::log10 ((double) lane[laneIndex][(size_t) i]); }

    double meanDb (int laneIndex, int start, int end) const
    {
        double acc = 0.0;
        for (int i = start; i < end; ++i) acc += db (laneIndex, i);
        return acc / (double) (end - start);
    }
};

double rmsDb (const juce::AudioBuffer<float>& b, int ch, int start, int end)
{
    double acc = 0.0;
    for (int i = start; i < end; ++i) acc += (double) b.getSample (ch, i) * b.getSample (ch, i);
    return 10.0 * std::log10 (acc / (double) (end - start) + 1.0e-30);
}

juce::AudioBuffer<float> slice (const juce::AudioBuffer<float>& b, int start, int length)
{
    juce::AudioBuffer<float> out (b.getNumChannels(), length);
    for (int c = 0; c < b.getNumChannels(); ++c) out.copyFrom (c, 0, b, c, start, length);
    return out;
}

/** Feeds a stereo sine (the same in both channels), phase-continuous over calls through `position`. */
void feedSine (rt::LoudnessMeter& m, double frequency, double amplitude, int numSamples, long long& position,
               double phase = 0.0, int fadeInSamples = 0)
{
    std::vector<float> block (480);
    for (int done = 0; done < numSamples; done += 480)
    {
        const int n = std::min (480, numSamples - done);
        for (int i = 0; i < n; ++i)
        {
            const long long t = position + i;
            const double fade = t < fadeInSamples ? 0.5 - 0.5 * std::cos (dsp::kPi * (double) t / (double) fadeInSamples) : 1.0;
            block[(size_t) i] = (float) (fade * amplitude * std::sin (2.0 * dsp::kPi * frequency * (double) t / kFs + phase));
        }
        m.process (block.data(), block.data(), n);
        position += n;
    }
}
}

// ---- 1 ---------------------------------------------------------------------------------------------
// Mutation: BandEngineBase::processChunk without "if (autoActive && ! autoGainRunning) autoGain.reset();".
TEST_CASE ("Automatic gain starts from 0 dB when it is switched on again", "[dsp][autogain][mutationgap]")
{
    Rig rig (2);
    rig.band (1, "fLo", 20.0f); rig.band (1, "fHi", 20000.0f);
    rig.band (1, "thresh", -50.0f); rig.band (1, "ratio", 4.0f);
    rig.global ("autoGain", 1.0f);
    rig.restart();

    // Heavy compression: the correction settles at several dB.
    rig.render (makeNoise (2, 4 * 48000, 51u, 0.3f));
    const float established = rig.autoGainDb();
    INFO ("correction while compressing " << established << " dB");
    REQUIRE (established > 4.0f);

    // Off, and the band made neutral (2 s: 13 release times, the reduction is gone).
    rig.global ("autoGain", 0.0f);
    rig.band (1, "thresh", 12.0f);
    rig.render (makeNoise (2, 2 * 48000, 52u, 0.3f));
    CHECK (rig.autoGainDb() == 0.0f);

    // On again. Nothing needs correcting now, so the correction has to stay at 0 dB from the
    // first block on: the follower starts afresh instead of resuming with the old +6.5 dB.
    //   largest reported correction in the first 200 ms: real 0.00001 dB, mutated +6.47 dB
    //   level of the first 100 ms against the input:      real 0.0000 dB, mutated +6.01 dB
    rig.global ("autoGain", 1.0f);
    const int n = 9600, block = 128;
    const auto in = makeNoise (2, n, 53u, 0.3f);
    juce::AudioBuffer<float> out (2, n);
    float largest = 0.0f;
    for (int pos = 0; pos < n; pos += block)
    {
        const auto piece = rig.render (slice (in, pos, block));
        for (int c = 0; c < 2; ++c) out.copyFrom (c, pos, piece, c, 0, block);
        largest = std::max (largest, std::abs (rig.autoGainDb()));
    }
    INFO ("largest correction in the first 200 ms " << largest << " dB");
    CHECK (largest < 0.5f);

    const double burst = rmsDb (out, 0, 0, 4800) - rmsDb (in, 0, 0, 4800);
    INFO ("level of the first 100 ms against the input " << burst << " dB");
    CHECK_THAT (burst, WithinAbs (0.0, 0.5));
}

// ---- 2 ---------------------------------------------------------------------------------------------
// Mutation: dsp::AutoGain::tick without the gate "refMs > kAutoGainGateMeanSquare". (Without the
// second condition on wetMs as well the follower divides 0 by 0 on its first sample and reads
// -120 dB from then on, which this test and an older one both report.)
TEST_CASE ("Automatic gain follower holds its correction below the gate", "[dsp][autogain][mutationgap]")
{
    dsp::AutoGain follower;
    follower.prepare (kFs);

    long long position = 0;
    const auto feed = [&] (double referenceAmplitude, double processedAmplitude, double seconds)
    {
        const long long end = position + (long long) std::llround (seconds * kFs);
        for (; position < end; ++position)
        {
            const double s = std::sin (2.0 * dsp::kPi * 1000.0 * (double) position / kFs);
            const float reference = (float) (referenceAmplitude * s), processed = (float) (processedAmplitude * s);
            follower.tick (&reference, &processed, 1, position % dsp::kCoeffGrid == 0);
        }
    };

    // Programme: the processed signal is 6 dB below the reference.
    feed (0.5, 0.25, 5.0);
    const double established = follower.gainDb();
    CHECK_THAT (established, WithinAbs (6.02, 0.01));

    // Then a noise floor at -94 dBFS that is the SAME in both (nothing to correct down there).
    // Its mean square (2.3e-10 K-weighted) is far below the gate (1e-7) but above the lower
    // limit for the processed signal (1e-10), so only the gate keeps the follower from adopting
    // the floor's ratio of 0 dB once the averages have decayed (the gate closes 14 s after the
    // programme stops, the floor dominates the averages from about 19 s).
    //   after 30 s: real +5.990 dB (the floor has moved it by 0.03 dB before the gate closed),
    //   mutated +0.0003 dB.
    feed (2.0e-5, 2.0e-5, 30.0);
    INFO ("correction after 30 s below the gate " << follower.gainDb() << " dB, before " << established << " dB");
    CHECK_THAT (follower.gainDb(), WithinAbs (established, 0.25));

    // Digital silence after that changes nothing either.
    feed (0.0, 0.0, 3.0);
    CHECK_THAT (follower.gainDb(), WithinAbs (established, 0.25));
    CHECK (follower.isFinite());

    // Programme followed directly by digital silence: held as well. This part does NOT tell the
    // gate from its absence (real and mutated both read +6.0206 dB after 30 s): both averages
    // decay in the same proportion, so their ratio, and with it the correction, stays where it
    // was. It takes a floor with a different ratio, as above, to see the gate.
    follower.prepare (kFs);
    position = 0;
    feed (0.5, 0.25, 5.0);
    feed (0.0, 0.0, 30.0);
    INFO ("silence: correction after 30 s of digital silence " << follower.gainDb() << " dB");
    CHECK_THAT (follower.gainDb(), WithinAbs (established, 0.25));
}

// ---- 3 ---------------------------------------------------------------------------------------------
// Mutation: rt::LoudnessMeter relative gate 8 LU below the ungated mean instead of 10 LU.
TEST_CASE ("Integrated loudness: the relative gate is 10 LU below the ungated loudness", "[meter][loudness][mutationgap]")
{
    // A stereo 997 Hz sine of peak level P dBFS reads P LUFS. Four seconds at -20 alternate with
    // four seconds at -31.7: the ungated loudness is -22.7 LUFS, so the quiet sections are 9.0 LU
    // below it. They count with the 10 LU gate (at -32.7) and would be dropped by an 8 LU gate
    // (at -30.7), one LU of margin on either side.
    struct Section { double levelDb; int subBlocks; };          // 100 ms sub-blocks
    const double loud = -20.0, quiet = -31.7;
    const std::vector<Section> programme { { loud, 40 }, { quiet, 40 }, { loud, 40 }, { quiet, 40 } };

    // Expected value from the definition: 400 ms blocks every 100 ms, absolute gate -70 LUFS,
    // relative gate `gateLu` below the mean of the blocks that passed the absolute gate.
    std::vector<double> subPower;
    for (const auto& s : programme)
        subPower.insert (subPower.end(), (size_t) s.subBlocks, std::pow (10.0, s.levelDb / 10.0));

    const auto integrated = [&subPower] (double gateLu)
    {
        std::vector<double> blocks;
        for (size_t k = 3; k < subPower.size(); ++k)
        {
            const double p = 0.25 * (subPower[k] + subPower[k - 1] + subPower[k - 2] + subPower[k - 3]);
            if (10.0 * std::log10 (p) >= -70.0) blocks.push_back (p);
        }
        double total = 0.0;
        for (double p : blocks) total += p;
        const double gate = 10.0 * std::log10 (total / (double) blocks.size()) - gateLu;

        double gated = 0.0;
        int count = 0;
        for (double p : blocks)
            if (10.0 * std::log10 (p) >= gate) { gated += p; ++count; }
        return 10.0 * std::log10 (gated / (double) count);
    };

    const double expected = integrated (10.0), withNarrowGate = integrated (8.0);
    INFO ("expected " << expected << " LUFS; with an 8 LU gate it would be " << withNarrowGate << " LUFS");
    CHECK_THAT (expected, WithinAbs (-22.73, 0.05));             // by hand: -20 + 10 log10 ((1 + 10^-1.17) / 2) = -22.73
    REQUIRE (withNarrowGate > expected + 2.0);                   // the programme does tell the two gates apart

    auto m = std::make_unique<rt::LoudnessMeter>();
    m->prepare (kFs);
    long long position = 0;
    for (const auto& s : programme)
        feedSine (*m, 997.0, std::pow (10.0, s.levelDb / 20.0), s.subBlocks * 4800, position);

    // real -22.74 LUFS (0.01 from the expected value: 0.1 LU histogram), mutated -20.23 LUFS.
    INFO ("measured " << m->getIntegratedLufs() << " LUFS");
    CHECK_THAT ((double) m->getIntegratedLufs(), WithinAbs (expected, 0.2));
}

// ---- 4 ---------------------------------------------------------------------------------------------
// Mutation: rt::LoudnessMeter true peak taken from the 2x interpolated signal instead of the 4x one.
TEST_CASE ("True peak: a peak a quarter of a sample away from the samples is found", "[meter][loudness][mutationgap]")
{
    // fs/3 starting at phase 0: the samples sit at 0, 120 and 240 degrees (0.866 of the peak),
    // the 2x interpolated points add 60, 180 and 300 degrees (still 0.866, -1.25 dB), and only
    // the 4x points at 30, 90, 150 ... degrees reach the peak. Faded in over 50 ms so that the
    // maximum is the steady tone, not a switch-on transient of the interpolation filter.
    //   real -6.0203 dBTP (expected -6.0206), mutated -7.27 dBTP (the sample peak is -7.27 dBFS as well).
    const double amplitude = 0.5, expected = 20.0 * std::log10 (amplitude);
    auto m = std::make_unique<rt::LoudnessMeter>();
    m->prepare (kFs);
    long long position = 0;
    feedSine (*m, kFs / 3.0, amplitude, 48000, position, 0.0, 2400);
    INFO ("true peak " << m->getTruePeakDb() << " dBTP, expected " << expected);
    CHECK_THAT ((double) m->getTruePeakDb(), WithinAbs (expected, 0.1));

    // The same an eighth of a cycle further on at fs/4 (samples and 2x points at 22.5 + k 45
    // degrees: 0.924 of the peak, -0.69 dB).   real -6.0205 dBTP, mutated -6.71 dBTP.
    m->prepare (kFs);
    position = 0;
    feedSine (*m, kFs / 4.0, amplitude, 48000, position, dsp::kPi / 8.0, 2400);
    INFO ("true peak at fs/4 " << m->getTruePeakDb() << " dBTP, expected " << expected);
    CHECK_THAT ((double) m->getTruePeakDb(), WithinAbs (expected, 0.1));
}

// ---- 5 ---------------------------------------------------------------------------------------------
// Mutation: APrimeRegionEngine width target "std::abs (da) >= std::abs (db) ? da : db" with "<=".
TEST_CASE ("A-prime: both lanes share the width of the deeper lane", "[dsp][aprime][mutationgap]")
{
    // One-octave band, a tone at its lower edge, Left/Right mode (independent lanes). The edge is
    // where the width shows: constant width means half the centre depth (in dB) there, while
    // the unity-gain width (what the shallower, idle lane asks for) leaves about -3 dB.
    const int n = 2 * 48000, tail = n - 24000;
    const double lo = 1000.0, hi = 2000.0;

    struct Result { juce::AudioBuffer<float> in, out; double gainDb = 0.0; };
    const auto run = [&] (double rightAmplitude)
    {
        Rig rig (2, Kind::APrime);
        rig.band (1, "fLo", (float) lo); rig.band (1, "fHi", (float) hi);
        rig.band (1, "thresh", -40.0f); rig.band (1, "ratio", 20.0f);
        rig.band (1, "attack", 1.0f); rig.band (1, "release", 2000.0f);
        rig.band (1, "stereoMode", kLeftRight);
        rig.restart();

        Result r;
        r.in.setSize (2, n);
        r.in.copyFrom (0, 0, sine (lo, kFs, n, 0.5).data(), n);
        r.in.copyFrom (1, 0, sine (lo, kFs, n, rightAmplitude).data(), n);
        Trace trace (rig, n);
        r.out = rig.render (r.in);
        r.gainDb = trace.meanDb (0, tail, n);
        if (rightAmplitude < 0.01)
            for (int i = 0; i < n; ++i) REQUIRE (trace.lane[1][(size_t) i] == 1.0f);   // the quiet lane does nothing
        return r;
    };

    const auto both = run (0.5);          // both lanes compress alike
    const auto leftOnly = run (0.0005);   // right 60 dB down: below the threshold, gain exactly 1
    INFO ("left lane gain " << leftOnly.gainDb << " dB (both loud: " << both.gainDb << " dB)");
    REQUIRE (leftOnly.gainDb < -20.0);
    CHECK (leftOnly.gainDb == both.gainDb);

    // The loud channel does not care what the other lane does ...
    //   largest difference between the two left outputs: real 0 (bit-identical), mutated 0.24
    double worst = 0.0;
    for (int i = 0; i < n; ++i)
        worst = std::max (worst, std::abs ((double) leftOnly.out.getSample (0, i) - (double) both.out.getSample (0, i)));
    INFO ("largest difference on the left channel " << worst);
    CHECK (worst < 1.0e-5);

    // ... and its depth at the edge is half the band gain:
    //   real -12.00 dB for a gain of -24.00 dB, mutated -2.99 dB.
    const double edgeDepth = rmsDb (leftOnly.out, 0, tail, n) - rmsDb (leftOnly.in, 0, tail, n);
    INFO ("depth at the edge " << edgeDepth << " dB");
    CHECK_THAT (edgeDepth, WithinAbs (0.5 * leftOnly.gainDb, 0.5));

    // The quiet channel is untouched.
    for (int i = 0; i < n; ++i) REQUIRE (leftOnly.out.getSample (1, i) == leftOnly.in.getSample (1, i));
}

// ---- 6 ---------------------------------------------------------------------------------------------
// Mutation: DynamicsCore::computeChunk adopts a new stereo mode without "resetBandState (b);".
TEST_CASE ("A stereo-mode change starts the lanes from zero", "[dsp][routing][mutationgap]")
{
    // Lane 1 is the right channel in Left/Right mode and the side signal in Mid/Side mode. It is
    // driven into the full 24 dB of reduction with a 5 s release, then the mode changes and the
    // programme becomes a quiet signal (below the threshold) that lives in the NEW lane 1 only.
    // That lane must start without the reduction its predecessor had built up.
    const int block = 128, changeBlock = 250, changeAt = changeBlock * block, n = changeAt + 24000;
    const double f = 900.0;

    struct Direction { const char* name; float from, to; };
    for (const Direction& d : { Direction { "Left/Right -> Mid/Side", kLeftRight, kMidSide },
                                Direction { "Mid/Side -> Left/Right", kMidSide, kLeftRight } })
    {
        INFO (d.name);
        Rig rig (2);
        rig.band (1, "fLo", 300.0f); rig.band (1, "fHi", 3000.0f);
        rig.band (1, "thresh", -40.0f); rig.band (1, "ratio", 20.0f);
        rig.band (1, "attack", 1.0f); rig.band (1, "release", 5000.0f);
        rig.band (1, "stereoMode", d.from);
        rig.restart();

        // Lane 1 only, in the mode of the moment: right only for L/R, L = -R (pure side) for M/S.
        const auto loud = sine (f, kFs, n, 0.5), quiet = sine (f, kFs, n, 0.001);
        juce::AudioBuffer<float> in (2, n);
        for (int i = 0; i < n; ++i)
        {
            const bool before = i < changeAt;
            const float v = before ? loud[(size_t) i] : quiet[(size_t) i];
            const bool sideOnly = (before ? d.from : d.to) == kMidSide;
            in.setSample (0, i, sideOnly ? v : 0.0f);
            in.setSample (1, i, sideOnly ? -v : v);
        }

        Trace trace (rig, n);
        const float to = d.to;
        const auto out = rig.render (in, [to] (int b, PluginProcessor& p)
        {
            if (b == changeBlock) setBand (p, 1, "stereoMode", to);
        });

        REQUIRE (trace.db (1, changeAt - 1) < -23.0);            // the old lane 1 was deep in reduction
        REQUIRE (trace.db (0, changeAt - 1) == 0.0);

        // From 50 ms after the change (10 ms fade out, switch, 10 ms fade in) the band is exactly
        // neutral on the real code, and the quiet signal passes unchanged:
        //   gain of lane 1 100 ms after the change: real 0 dB exactly, mutated -23.5 dB
        //   deepest gain from 50 ms after the change:  real 0 dB exactly, mutated -23.8 dB
        //   output against input over the last 400 ms: real 0 dB exactly, mutated -21.5 dB
        // (the same in both directions)
        const int settled = changeAt + 2400;
        INFO ("lane 1 gain 100 ms after the change " << trace.db (1, changeAt + 4800) << " dB");
        double deepest = 0.0;
        for (int i = settled; i < n; ++i)
            deepest = std::min ({ deepest, trace.db (0, i), trace.db (1, i) });
        INFO ("deepest gain after the change " << deepest << " dB");
        CHECK (deepest > -0.5);

        const double change = rmsDb (out, 1, n - 19200, n) - rmsDb (in, 1, n - 19200, n);
        INFO ("right channel against the input after the change " << change << " dB");
        CHECK_THAT (change, WithinAbs (0.0, 0.5));
    }
}

// ---- 7 ---------------------------------------------------------------------------------------------
// Mutation: APrimeRegionEngine solo taken before the band gain ("solo[j] = bp" instead of
// "g[j] * bp"; in the mid/side branch "bm + bs" / "bm - bs" instead of "g0 bm + g1 bs" / "g0 bm - g1 bs").
TEST_CASE ("A-prime solo is the band's component after its gain, in left/right and in mid/side", "[dsp][aprime][audition][mutationgap]")
{
    // A compressing band on independent left / right noise (so mid and side both carry signal).
    // With the band's unity-gain component b per lane and its gain g (recorded by the gain trace):
    //   normal output  y = x + (g - 1) b      ->  b = (y - x) / (g - 1)
    //   solo output    s = g b
    // In mid/side mode the lanes are M and S and the outputs are M +- S of the above.
    const int n = 48000, from = 24000;
    const auto in = makeNoise (2, n, 71u, 0.4f);

    struct Case { const char* name; float stereoMode; bool midSide; };
    for (const Case& c : { Case { "Left/Right", kLeftRight, false }, Case { "Mid/Side", kMidSide, true } })
    {
        INFO (c.name);
        const auto configure = [&c] (Rig& rig)
        {
            rig.band (1, "fLo", 300.0f); rig.band (1, "fHi", 3000.0f);
            rig.band (1, "thresh", -40.0f); rig.band (1, "ratio", 8.0f);
            rig.band (1, "stereoMode", c.stereoMode);
            rig.restart();
        };

        Rig normalRig (2, Kind::APrime);
        configure (normalRig);
        Trace trace (normalRig, n);
        const auto normal = normalRig.render (in);

        Rig soloRig (2, Kind::APrime);
        configure (soloRig);
        soloRig.proc->setAudition (Mode::Solo, 1);
        const auto solo = soloRig.render (in);
        REQUIRE (soloRig.proc->getEngine().getAuditionMode() == Mode::Solo);

        juce::AudioBuffer<float> expected (2, n), ungained (2, n);
        expected.clear();
        ungained.clear();
        for (int i = from; i < n; ++i)
        {
            const double g0 = trace.lane[0][(size_t) i], g1 = trace.lane[1][(size_t) i];
            REQUIRE (g0 < 0.6);                                   // both lanes compress by more than 4 dB throughout
            REQUIRE (g1 < 0.6);
            const double dl = (double) normal.getSample (0, i) - in.getSample (0, i);
            const double dr = (double) normal.getSample (1, i) - in.getSample (1, i);
            // Lane components at unity gain (L and R, or M and S).
            const double b0 = (c.midSide ? 0.5 * (dl + dr) : dl) / (g0 - 1.0);
            const double b1 = (c.midSide ? 0.5 * (dl - dr) : dr) / (g1 - 1.0);
            expected.setSample (0, i, (float) (c.midSide ? g0 * b0 + g1 * b1 : g0 * b0));
            expected.setSample (1, i, (float) (c.midSide ? g0 * b0 - g1 * b1 : g1 * b1));
            ungained.setSample (0, i, (float) (c.midSide ? b0 + b1 : b0));
            ungained.setSample (1, i, (float) (c.midSide ? b0 - b1 : b1));
        }

        // Sample by sample:
        //   largest error: real 3.2e-8 (both modes); mutated 0.33 (L/R), 0.31 (M/S)
        // and as a level, solo against the expected signal:
        //   real 0.0000 dB, mutated +17.8 dB (L/R), +15.3 dB (M/S): the gain reduction that is missing.
        // The tolerance of 2e-5 leaves room for float rounding in how the audition is mixed in
        // (output = x + fade (s - x) with fade == 1, or s itself).
        double worst = 0.0;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = from; i < n; ++i)
                worst = std::max (worst, std::abs ((double) solo.getSample (ch, i) - (double) expected.getSample (ch, i)));
        INFO ("largest error " << worst);
        CHECK (worst < 2.0e-5);

        for (int ch = 0; ch < 2; ++ch)
        {
            const double level = rmsDb (solo, ch, from, n) - rmsDb (expected, ch, from, n);
            const double reduction = rmsDb (expected, ch, from, n) - rmsDb (ungained, ch, from, n);
            INFO ("channel " << ch << ": solo against expected " << level << " dB; the gain takes " << reduction << " dB off the component");
            CHECK (reduction < -6.0);                             // the test would notice a missing gain
            CHECK_THAT (level, WithinAbs (0.0, 0.2));
            CHECK (rmsDb (solo, ch, from, n) > -60.0);            // and it is a signal, not silence
        }
    }
}
