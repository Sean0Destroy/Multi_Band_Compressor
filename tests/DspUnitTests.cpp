#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <random>

#include "dsp/BandDetector.h"
#include "dsp/EdgeSmoother.h"
#include "dsp/GainComputer.h"
#include "dsp/GrSmoother.h"
#include "dsp/TptSvf.h"

using namespace tl::dsp;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace
{
// Independent float64 statement of the Giannoulis/Massberg/Reiss soft knee.
double referenceGr (double x, double T, double R, double W, double range)
{
    const double invR = R >= 100.0 ? 0.0 : 1.0 / R;
    double y;
    if (2.0 * (x - T) < -W)                   y = x;
    else if (std::abs (2.0 * (x - T)) <= W)   y = x + (invR - 1.0) * std::pow (x - T + W / 2.0, 2.0) / (2.0 * W);
    else                                      y = T + (x - T) * invR;
    return std::max (y - x, -range);
}
}

TEST_CASE ("Gain computer matches the soft-knee definition", "[dsp][unit]")
{
    std::mt19937 rng (3);
    std::uniform_real_distribution<float> xs (-120.0f, 24.0f);

    for (float T : { -80.0f, -40.0f, -18.0f, 0.0f, 12.0f })
        for (float R : { 1.0f, 1.5f, 2.0f, 4.0f, 10.0f, 99.0f, 100.0f })
            for (float W : { 0.0f, 0.5f, 6.0f, 24.0f })
                for (float range : { 0.0f, 6.0f, 24.0f, 48.0f })
                {
                    GainComputer gc;
                    gc.set (T, R, W, range);
                    for (int i = 0; i < 200; ++i)
                    {
                        const float x = xs (rng);
                        const double ref = referenceGr (x, T, R, W, range);
                        INFO ("T " << T << " R " << R << " W " << W << " range " << range << " x " << x);
                        REQUIRE_THAT ((double) gc.gainReductionDb (x), WithinAbs (ref, 2e-4 + 1e-6 * std::abs (ref)));
                        REQUIRE (gc.gainReductionDb (x) <= 0.0f);
                        if (2.0f * (x - T) < -W)
                            REQUIRE (gc.gainReductionDb (x) == 0.0f);   // exactly zero below the knee
                    }
                }
}

TEST_CASE ("Gain computer: continuity, infinite ratio and range", "[dsp][unit]")
{
    GainComputer gc;
    gc.set (-20.0f, 4.0f, 6.0f, 48.0f);
    // Continuous at both knee boundaries.
    CHECK_THAT (gc.gainReductionDb (-23.0f), WithinAbs (0.0, 1e-6));
    CHECK_THAT (gc.gainReductionDb (-17.0f), WithinAbs (-3.0 * 0.75, 1e-5));
    CHECK_THAT (gc.gainReductionDb (-16.999f), WithinAbs (-3.001 * 0.75, 1e-4));
    // Slope above the knee is 1/R.
    CHECK_THAT (gc.gainReductionDb (0.0f) - gc.gainReductionDb (-10.0f), WithinAbs (-7.5, 1e-4));

    gc.set (-20.0f, 100.0f, 0.0f, 48.0f);   // "inf": output pinned to T
    CHECK_THAT (gc.gainReductionDb (0.0f), WithinAbs (-20.0, 1e-5));
    CHECK (gc.gainReductionDb (-20.0f) == 0.0f);

    gc.set (-40.0f, 100.0f, 0.0f, 12.0f);   // range limits the reduction
    CHECK_THAT (gc.gainReductionDb (0.0f), WithinAbs (-12.0, 1e-6));
    gc.set (-40.0f, 100.0f, 0.0f, 0.0f);
    CHECK (gc.gainReductionDb (0.0f) == 0.0f);
}

TEST_CASE ("GR smoother attack and release time constants", "[dsp][unit]")
{
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
        for (double attackMs : { 0.1, 1.0, 10.0, 100.0 })
        {
            const double releaseMs = attackMs * 15.0;
            GrSmoother sm;
            sm.setTimes (attackMs, releaseMs, fs);

            const int nA = (int) std::lround (attackMs * 1e-3 * fs);
            for (int i = 0; i < nA; ++i) sm.tick (-12.0f);
            // After exactly tau (rounded to whole samples) 1 - e^-1 of the step is reached.
            const double expectA = 12.0 * (1.0 - std::exp (-(double) nA / (attackMs * 1e-3 * fs)));
            INFO ("fs " << fs << " attack " << attackMs);
            CHECK_THAT ((double) sm.amountDb(), WithinRel (expectA, 1e-4));

            for (int i = 0; i < 20 * nA; ++i) sm.tick (-12.0f);
            const double start = sm.amountDb();
            const int nR = (int) std::lround (releaseMs * 1e-3 * fs);
            for (int i = 0; i < nR; ++i) sm.tick (0.0f);
            CHECK_THAT ((double) sm.amountDb(), WithinRel (start * std::exp (-(double) nR / (releaseMs * 1e-3 * fs)), 1e-3));
        }
}

TEST_CASE ("Smoothers converge to non-zero targets (no float dead band)", "[dsp][unit]")
{
    // A float one-pole stalls up to 0.5/(1-a) ulps short of its target; the states are double.
    for (double fs : { 48000.0, 192000.0 })
    {
        GrSmoother sm;
        sm.setTimes (300.0, 5000.0, fs);
        for (int i = 0; i < (int) (3.0 * fs); ++i) sm.tick (-48.0f);   // 10 attack time constants
        CHECK_THAT ((double) sm.amountDb(), WithinAbs (48.0 * (1.0 - std::exp (-10.0)), 2e-3));
        for (int i = 0; i < (int) (50.0 * fs); ++i) sm.tick (-6.0f);   // 10 release time constants
        CHECK_THAT ((double) sm.amountDb(), WithinAbs (6.0 + 42.0 * std::exp (-10.0), 1e-3));

        EdgeSmoother e;
        e.prepare (fs);
        e.setTargetsHz (20.0f, 120.0f);
        e.snapToTargets();
        e.setTargetsHz (500.0f, 2000.0f);
        for (int i = 0; i < (int) fs; ++i) e.tick();                    // 50 time constants
        CHECK (e.smoothedLoLog2() == std::log2 (500.0f));               // exactly on target
        CHECK (e.smoothedHiLog2() == std::log2 (2000.0f));
    }
}

TEST_CASE ("GR smoother stays exactly at unity without reduction", "[dsp][unit]")
{
    GrSmoother sm;
    sm.setTimes (0.01, 5.0, 48000.0);
    for (int i = 0; i < 1000; ++i)
        REQUIRE (GrSmoother::gainFromAmount (sm.tick (0.0f)) == 1.0f);
    CHECK_THAT ((double) GrSmoother::gainFromAmount (6.0f), WithinRel (std::pow (10.0, -6.0 / 20.0), 1e-6));
}

TEST_CASE ("Activity ramp is linear over 10 ms", "[dsp][unit]")
{
    for (double fs : { 44100.0, 48000.0, 192000.0 })
    {
        ActivityRamp r;
        r.prepare (fs);
        r.setTarget (true);
        const int n = (int) std::lround (0.010 * fs);
        float prev = 0.0f;
        int reached = -1;
        for (int i = 1; i <= n + 2; ++i)
        {
            const float a = r.tick();
            REQUIRE (a >= prev);
            if (i < n - 1) REQUIRE_THAT ((double) a, WithinAbs ((double) i / n, 1e-4));
            if (reached < 0 && a == 1.0f) reached = i;
            prev = a;
        }
        CHECK (std::abs (reached - n) <= 1);
        r.setTarget (false);
        for (int i = 0; i < n + 2; ++i) r.tick();
        CHECK (r.value() == 0.0f);
        CHECK (r.isSettledOff());
    }
}

TEST_CASE ("Edge smoother: log2 one-pole, ordering and runtime clamp", "[dsp][unit]")
{
    const double fs = 48000.0;
    EdgeSmoother e;
    e.prepare (fs);

    e.setTargetsHz (2000.0f, 500.0f);   // reversed: resolved in DSP
    e.snapToTargets();
    float lo = 0, hi = 0;
    e.clampedLog2 (lo, hi);
    CHECK_THAT ((double) std::exp2 (lo), WithinRel (500.0, 1e-5));
    CHECK_THAT ((double) std::exp2 (hi), WithinRel (2000.0, 1e-5));

    e.setTargetsHz (1000.0f, 1000.0f);   // identical edges -> 1/24 octave apart
    e.snapToTargets();
    e.clampedLog2 (lo, hi);
    CHECK_THAT ((double) (hi - lo), WithinAbs (1.0 / 24.0, 1e-5));

    e.setTargetsHz (1.0f, 30000.0f);     // outside the runtime range
    e.snapToTargets();
    e.clampedLog2 (lo, hi);
    CHECK_THAT ((double) std::exp2 (lo), WithinRel (10.0, 1e-5));
    CHECK_THAT ((double) std::exp2 (hi), WithinRel (0.45 * fs, 1e-5));

    // Smoothing: 2 octave step, after 20 ms 63.2 % of the step in octaves.
    e.setTargetsHz (250.0f, 1000.0f);
    e.snapToTargets();
    e.setTargetsHz (1000.0f, 4000.0f);
    const int n = (int) std::lround (0.020 * fs);
    for (int i = 0; i < n; ++i) e.tick();
    CHECK_THAT ((double) (e.smoothedLoLog2() - std::log2 (250.0f)), WithinAbs (2.0 * (1.0 - std::exp (-1.0)), 1e-4));
    CHECK_THAT ((double) (e.smoothedHiLog2() - std::log2 (1000.0f)), WithinAbs (2.0 * (1.0 - std::exp (-1.0)), 1e-4));
}

TEST_CASE ("TPT SVF matches its bilinear transfer function", "[dsp][unit]")
{
    // Bandpass of the A3 section at the digital centre has gain 1/(2R) exactly (2R bp == x).
    const double fs = 48000.0, pi = 3.14159265358979323846;
    const double wl = std::tan (pi * 500.0 / fs), wh = std::tan (pi * 2000.0 / fs);
    const double g = std::sqrt (wl * wh), R = (wh - wl) / (2.0 * g);
    const auto c = SvfCoeffs::make (g, R);
    const double fc = fs / pi * std::atan (g);

    SvfState st;
    double peak = 0.0;
    for (int i = 0; i < 48000; ++i)
    {
        const float x = (float) std::sin (2.0 * pi * fc * i / fs);
        const auto o = svfTick (c, st, x);
        if (i > 24000) peak = std::max (peak, std::abs ((double) c.twoR * o.bp));
    }
    CHECK_THAT (peak, WithinAbs (1.0, 2e-4));
}

TEST_CASE ("Detector levels: peak, RMS and blend on a sine", "[dsp][unit]")
{
    // Hard knee, ratio inf, threshold T: gr = T - level above T, so the level is T - gr.
    const double fs = 48000.0, pi = 3.14159265358979323846;
    const float T = -30.0f;
    GainComputer gc;
    gc.set (T, 100.0f, 0.0f, 48.0f);

    const double peakDb = 20.0 * std::log10 (0.5), rmsDb = peakDb - 3.0103;
    struct Case { DetectorMode mode; float blend; double expectMax; };
    for (auto cs : { Case { DetectorMode::Peak, 0.0f, peakDb }, Case { DetectorMode::Rms, 0.0f, rmsDb },
                     Case { DetectorMode::Blend, 0.5f, 0.5 * (peakDb + rmsDb) }, Case { DetectorMode::Blend, 0.0f, peakDb },
                     Case { DetectorMode::Blend, 1.0f, rmsDb } })
    {
        BandDetectorConfig cfg;
        cfg.mode = cs.mode;
        cfg.blend = cs.blend;
        cfg.setRmsTime (50.0, fs);
        cfg.setKneeStart (gc.kneeStartDb());
        BandDetectorState st;
        double maxLevel = -1000.0;
        for (int i = 0; i < 96000; ++i)
        {
            const float d = (float) (0.5 * std::sin (2.0 * pi * 1000.0 * i / fs));
            const float gr = detectorGainReduction (st, cfg, gc, d);
            if (i > 48000) maxLevel = std::max (maxLevel, (double) (T - gr));
        }
        INFO ("mode " << (int) cs.mode << " blend " << cs.blend);
        CHECK_THAT (maxLevel, WithinAbs (cs.expectMax, 0.03));   // RMS ripple at 2 kHz with 50 ms is < 0.03 dB
    }
}
