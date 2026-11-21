// Compression behaviour through the plugin processor (parameters -> engine -> audio):
// static curves, time constants, detector modes, range, automation and stereo link.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <random>

#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;
using Catch::Matchers::WithinAbs;

namespace
{
struct Rig
{
    std::unique_ptr<PluginProcessor> proc = std::make_unique<PluginProcessor>();
    OfflineRenderer renderer { *proc };
    double fs;

    Rig (double sampleRate, int channels = 1, int bands = 1) : fs (sampleRate)
    {
        for (int i = 0; i < bands; ++i) proc->getBandSlots().createBand();
        renderer.prepare (fs, 512, { channels, 0 });
    }

    void band (int slot, const char* suffix, float v) { setBand (*proc, slot, suffix, v); }

    juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& in, std::vector<int> blocks = { 128 })
    {
        return renderer.render (in, nullptr, blocks);
    }

    /** Re-prepares (resets the engine) so that each measurement starts from the same state. */
    void restart() { renderer.prepare (fs, 512, { proc->getMainBusNumInputChannels(), 0 }); }
};

juce::AudioBuffer<float> toBuffer (const std::vector<std::vector<float>>& ch)
{
    juce::AudioBuffer<float> b ((int) ch.size(), (int) ch[0].size());
    for (int c = 0; c < b.getNumChannels(); ++c)
        b.copyFrom (c, 0, ch[(size_t) c].data(), b.getNumSamples());
    return b;
}

double rmsOf (const juce::AudioBuffer<float>& b, int ch, int start, int end)
{
    double acc = 0.0;
    for (int i = start; i < end; ++i) acc += (double) b.getSample (ch, i) * b.getSample (ch, i);
    return std::sqrt (acc / (end - start));
}

/** Measured steady-state GR at the band's digital centre: output/input RMS over the last 0.2 s. */
double steadyGrDb (Rig& rig, double fc, double amplitude, double seconds)
{
    const int n = (int) (seconds * rig.fs);
    const auto in = toBuffer ({ sine (fc, rig.fs, n, amplitude) });
    rig.restart();
    const auto out = rig.render (in);
    const int start = n - (int) (0.2 * rig.fs);
    return dbOf (rmsOf (out, 0, start, n) / rmsOf (in, 0, start, n));
}

double referenceGr (double level, double T, double R, double W, double range)
{
    const double invR = R >= 100.0 ? 0.0 : 1.0 / R;
    double y;
    if (2.0 * (level - T) < -W)                 y = level;
    else if (std::abs (2.0 * (level - T)) <= W) y = level + (invR - 1.0) * std::pow (level - T + W / 2.0, 2.0) / (2.0 * W);
    else                                        y = T + (level - T) * invR;
    return std::max (y - level, -range);
}

/** Records gEff of (slot, channel) for the processor's next renders. */
struct Trace
{
    std::vector<float> g[dsp::kMaxBands][dsp::kMaxChannels];
    dsp::GainTrace t;

    Trace (PluginProcessor& p, int n)
    {
        t.capacity = n;
        for (int b = 0; b < dsp::kMaxBands; ++b)
            for (int c = 0; c < dsp::kMaxChannels; ++c)
            {
                g[b][c].assign ((size_t) n, 1.0f);
                t.data[b][c] = g[b][c].data();
            }
        p.getEngine().setGainTrace (&t);
    }

    double db (int slot, int ch, int i) const { return dbOf (g[slot - 1][ch][(size_t) i]); }
};
} // namespace

TEST_CASE ("Static curve accuracy: peak and RMS detectors", "[dsp][compressor]")
{
    const double fs = 48000.0;
    const double fLo = 500.0, fHi = 2000.0;
    const double fc = digitalCentre (fLo, fHi, fs);
    const double detDb = dbOf (detectorGain (fc, fLo, fHi, fs));   // detector filter loss at fc (-0.53 dB)
    std::vector<std::vector<double>> rows;

    for (int mode : { 0, 1 })
        for (double knee : { 0.0, 12.0 })
        {
            Rig rig (fs);
            rig.band (1, "fLo", (float) fLo);   rig.band (1, "fHi", (float) fHi);
            rig.band (1, "thresh", -30.0f);     rig.band (1, "ratio", 4.0f);
            rig.band (1, "knee", (float) knee); rig.band (1, "range", 48.0f);
            rig.band (1, "detMode", (float) mode);
            rig.band (1, "attack", mode == 0 ? 1.0f : 10.0f);
            rig.band (1, "release", mode == 0 ? 5000.0f : 100.0f);
            rig.band (1, "rmsTime", 30.0f);

            double worst = 0.0;
            for (double inDb = -50.0; inDb <= 0.01; inDb += 2.5)
            {
                const double level = inDb + detDb + (mode == 1 ? -3.0103 : 0.0);   // sine peak / RMS
                const double expected = referenceGr (level, -30.0, 4.0, knee, 48.0);
                const double measured = steadyGrDb (rig, fc, dbToGain (inDb), 0.8);
                INFO ("mode " << mode << " knee " << knee << " in " << inDb);
                CHECK_THAT (measured, WithinAbs (expected, 0.25));
                worst = std::max (worst, std::abs (measured - expected));
                rows.push_back ({ (double) mode, knee, inDb, level, expected, measured });
            }
            INFO ("worst static error " << worst);
            CHECK (worst < 0.25);
        }

    measure::writeCsv ("static_curves.csv", { "det_mode_0peak_1rms", "knee_db", "input_peak_dbfs", "detector_level_db", "expected_gr_db", "measured_gr_db" }, rows);
}

/** Raised-cosine fade-in: a hard tone onset makes the detector HP ring (a transient at fLo far
    above the steady out-of-band level), which a long release would then hold for seconds. */
std::vector<float> fadeIn (std::vector<float> x, int len)
{
    for (int i = 0; i < len && i < (int) x.size(); ++i)
        x[(size_t) i] *= (float) (0.5 - 0.5 * std::cos (3.14159265358979323846 * i / len));
    return x;
}

TEST_CASE ("Detector selectivity: out-of-band tones are attenuated by the detector filter", "[dsp][compressor]")
{
    // The detector must see only its own band: Butterworth HP at fLo then LP at fHi. The steady GR
    // for a tone outside the band must equal the static GR at the input level plus
    // 20 log10 |H_det(f)| (float64 bilinear model of the specified filters).
    const double fs = 48000.0, fLo = 500.0, fHi = 2000.0, amp = 0.5;
    const double T = -60.0, R = 4.0;

    struct Case { double f; int mode; };   // mode 0 peak (below-band tones: sample peaks are exact), 1 RMS
    std::vector<std::vector<double>> rows;
    for (auto cs : { Case { 250.0, 0 }, Case { 125.0, 0 }, Case { 250.0, 1 }, Case { 125.0, 1 }, Case { 4000.0, 1 }, Case { 8000.0, 1 } })
    {
        Rig rig (fs);
        rig.band (1, "fLo", (float) fLo); rig.band (1, "fHi", (float) fHi);
        rig.band (1, "thresh", (float) T); rig.band (1, "ratio", (float) R); rig.band (1, "knee", 0.0f);
        rig.band (1, "range", 48.0f);     rig.band (1, "detMode", (float) cs.mode);
        rig.band (1, "rmsTime", 300.0f);
        rig.band (1, "attack", cs.mode == 0 ? 0.1f : 1.0f);
        rig.band (1, "release", cs.mode == 0 ? 5000.0f : 1000.0f);

        const int n = (int) (2.5 * rig.fs);
        Trace tr (*rig.proc, n);
        rig.render (toBuffer ({ fadeIn (sine (cs.f, fs, n, amp), (int) (0.2 * fs)) }));

        const double hDet = dbOf (detectorGain (cs.f, fLo, fHi, fs));
        const double level = dbOf (amp) + hDet + (cs.mode == 1 ? -3.0103 : 0.0);
        const double expected = referenceGr (level, T, R, 0.0, 48.0);
        double measured = 0.0;
        const int start = n - (int) (0.3 * fs);
        if (cs.mode == 0)
            for (int i = start; i < n; ++i) measured = std::min (measured, tr.db (1, 0, i));   // deepest = peak GR
        else
        {
            for (int i = start; i < n; ++i) measured += tr.db (1, 0, i);
            measured /= (n - start);
        }
        INFO ("f " << cs.f << " mode " << cs.mode << " H_det " << hDet << " dB, expected " << expected << " measured " << measured);
        CHECK (hDet < -11.0);                                // genuinely out of band (>= 1 octave)
        CHECK_THAT (measured, WithinAbs (expected, 0.05));
        rows.push_back ({ cs.f, (double) cs.mode, hDet, expected, measured });
    }
    measure::writeCsv ("detector_selectivity.csv", { "freq_hz", "det_mode_0peak_1rms", "h_det_db", "expected_gr_db", "measured_gr_db" }, rows);

    // An out-of-band tone that the detector filter keeps below the knee leaves the band exactly
    // neutral: gEff == 1 for every sample. (In band, the same tone would be 14 dB over threshold.)
    Rig rig (fs);
    rig.band (1, "fLo", (float) fLo); rig.band (1, "fHi", (float) fHi);
    rig.band (1, "thresh", -20.0f);   rig.band (1, "knee", 6.0f); rig.band (1, "ratio", 4.0f);
    const int n = (int) fs;
    Trace tr (*rig.proc, n);
    const auto in = toBuffer ({ fadeIn (sine (125.0, fs, n, amp), (int) (0.2 * fs)) });
    const auto out = rig.render (in);
    CHECK (std::all_of (tr.g[0][0].begin(), tr.g[0][0].end(), [] (float g) { return g == 1.0f; }));
    CHECK (bitIdentical (in, out));
}

TEST_CASE ("Attack and release time constants through the processor", "[dsp][compressor]")
{
    // RMS detector with 1 ms averaging and a tone at the centre of a 4-16 kHz band: the GR target
    // is a clean step, so the smoother's time constants are directly visible in gEff.
    std::vector<std::vector<double>> traceRows;
    for (double fs : { 44100.0, 48000.0, 96000.0, 192000.0 })
    {
        const double attackMs = 30.0, releaseMs = 300.0;
        Rig rig (fs);
        rig.band (1, "fLo", 4000.0f);  rig.band (1, "fHi", 16000.0f);
        rig.band (1, "thresh", -40.0f); rig.band (1, "ratio", 100.0f); rig.band (1, "knee", 0.0f);
        rig.band (1, "range", 48.0f);  rig.band (1, "detMode", 1.0f); rig.band (1, "rmsTime", 1.0f);
        rig.band (1, "attack", (float) attackMs); rig.band (1, "release", (float) releaseMs);

        const int onset = (int) (0.05 * fs), stop = (int) (0.65 * fs), n = (int) (2.0 * fs);
        auto x = sine (digitalCentre (4000.0, 16000.0, fs), fs, n, 0.25);
        for (int i = 0; i < n; ++i)
            if (i < onset || i >= stop) x[(size_t) i] = 0.0f;

        Trace tr (*rig.proc, n);
        rig.render (toBuffer ({ x }));

        const double finalGr = tr.db (1, 0, stop - 1);
        int tA = -1, tR = -1;
        for (int i = onset; i < stop && tA < 0; ++i)
            if (tr.db (1, 0, i) <= (1.0 - std::exp (-1.0)) * finalGr) tA = i - onset;
        for (int i = stop; i < n && tR < 0; ++i)
            if (tr.db (1, 0, i) >= std::exp (-1.0) * finalGr) tR = i - stop;

        const double attackMeas = 1e3 * tA / fs, releaseMeas = 1e3 * tR / fs;
        INFO ("fs " << fs << " final GR " << finalGr << " attack " << attackMeas << " ms, release " << releaseMeas << " ms");
        CHECK (finalGr < -20.0);
        // The 1 ms RMS averager adds a small, explained lag on top of the smoother: after the stop
        // the RMS level falls by 4.343 dB per tau_rms, so the release target reaches 0 after
        // t0 = |GR| tau_rms / 4.343 and on average lags by t0 / 2 (2.85 ms for 24.8 dB).
        const double lagMs = std::abs (finalGr) * 1.0 / 4.343 / 2.0;
        CHECK (attackMeas >= attackMs);
        CHECK (attackMeas <= attackMs + 1.5);
        CHECK_THAT (releaseMeas, WithinAbs (releaseMs + lagMs, 0.3));

        if (fs == 48000.0)
            for (int i = 0; i < n; i += 24)
                traceRows.push_back ({ i / fs, (double) x[(size_t) i], tr.db (1, 0, i) });
    }
    measure::writeCsv ("attack_release_trace_rms.csv", { "time_s", "input", "gain_db" }, traceRows);
}

TEST_CASE ("Attack/release traces of the peak detector (export)", "[dsp][compressor]")
{
    const double fs = 48000.0;
    std::vector<std::vector<double>> rows;
    const int n = (int) (1.2 * fs), onset = (int) (0.1 * fs), stop = (int) (0.5 * fs);
    struct AR { float attack, release; };
    const std::vector<AR> cases { { 0.1f, 50.0f }, { 1.0f, 150.0f }, { 10.0f, 150.0f }, { 30.0f, 500.0f } };
    std::vector<std::vector<float>> gains;

    for (auto ar : cases)
    {
        Rig rig (fs);
        rig.band (1, "fLo", 500.0f);    rig.band (1, "fHi", 2000.0f);
        rig.band (1, "thresh", -30.0f); rig.band (1, "ratio", 4.0f); rig.band (1, "knee", 6.0f);
        rig.band (1, "attack", ar.attack); rig.band (1, "release", ar.release);
        auto x = sine (1000.0, fs, n, 0.5);
        for (int i = 0; i < n; ++i) if (i < onset || i >= stop) x[(size_t) i] *= 0.01f;   // -40 dB / -6 dB / -40 dB steps
        Trace tr (*rig.proc, n);
        rig.render (toBuffer ({ x }));
        gains.push_back (tr.g[0][0]);

        // The GR never exceeds the static GR of the loud part (beyond detector-filter onset ringing).
        // The peak detector has no hold: on a sustained tone the smoother settles between the
        // per-sample peaks, up to ~1 dB short of the static value for slow attacks (10-30 ms).
        const double staticGr = referenceGr (dbOf (0.5 * detectorGain (1000.0, 500, 2000, fs)), -30, 4, 6, 24);
        double deepest = 0.0;
        for (int i = 0; i < n; ++i) deepest = std::min (deepest, tr.db (1, 0, i));
        INFO ("attack " << ar.attack << " release " << ar.release);
        CHECK (deepest >= staticGr - 0.3);    // detector-filter onset ringing may exceed the steady peak slightly
        CHECK (deepest <= staticGr + 1.2);
    }

    for (int i = 0; i < n; i += 12)
    {
        std::vector<double> r { i / (double) fs };
        for (auto& g : gains) r.push_back (dbOf (g[(size_t) i]));
        rows.push_back (r);
    }
    measure::writeCsv ("attack_release_trace_peak.csv", { "time_s", "gain_db_a0.1_r50", "gain_db_a1_r150", "gain_db_a10_r150", "gain_db_a30_r500" }, rows);
}

TEST_CASE ("Detector modes: blend endpoints are bit-identical to peak / RMS", "[dsp][compressor]")
{
    const double fs = 48000.0;
    const auto in = makeNoise (2, 20000, 77u, 0.7f);

    auto renderWith = [&] (float mode, float blend)
    {
        Rig rig (fs, 2);
        rig.band (1, "thresh", -30.0f); rig.band (1, "ratio", 6.0f);
        rig.band (1, "detMode", mode);  rig.band (1, "detBlend", blend);
        rig.band (1, "fLo", 300.0f);    rig.band (1, "fHi", 3000.0f);
        return rig.render (in);
    };

    const auto peak = renderWith (0.0f, 0.37f), rms = renderWith (1.0f, 0.37f);
    CHECK (bitIdentical (peak, renderWith (2.0f, 0.0f)));
    CHECK (bitIdentical (rms, renderWith (2.0f, 1.0f)));
    CHECK_FALSE (bitIdentical (peak, rms));
}

TEST_CASE ("Detector modes: peak reacts to crest factor, RMS to energy, blend in between", "[dsp][compressor]")
{
    const double fs = 48000.0;
    const int n = (int) fs;
    // Click train (high crest factor) in a broad band.
    std::vector<float> clicks ((size_t) n, 0.0f);
    for (int i = 0; i < n; i += 480) { clicks[(size_t) i] = 0.9f; clicks[(size_t) i + 1] = -0.9f; }

    auto deepestGr = [&] (float mode, float blend)
    {
        Rig rig (fs);
        rig.band (1, "fLo", 1000.0f); rig.band (1, "fHi", 8000.0f);
        rig.band (1, "thresh", -40.0f); rig.band (1, "ratio", 4.0f); rig.band (1, "knee", 0.0f);
        rig.band (1, "attack", 0.1f);  rig.band (1, "release", 50.0f);
        rig.band (1, "detMode", mode); rig.band (1, "detBlend", blend); rig.band (1, "rmsTime", 30.0f);
        Trace tr (*rig.proc, n);
        rig.render (toBuffer ({ clicks }));
        double d = 0.0;
        for (int i = n / 2; i < n; ++i) d = std::min (d, tr.db (1, 0, i));
        return d;
    };

    const double p = deepestGr (0.0f, 0.0f), r = deepestGr (1.0f, 0.0f), b = deepestGr (2.0f, 0.5f);
    INFO ("peak " << p << " rms " << r << " blend " << b);
    CHECK (p < r - 6.0);   // peak detector sees the clicks' crest factor
    CHECK (b < r);
    CHECK (b > p);
}

TEST_CASE ("Range limits the gain reduction", "[dsp][compressor]")
{
    const double fs = 48000.0;
    const double fc = digitalCentre (500.0, 2000.0, fs);
    std::vector<std::vector<double>> rows;

    for (float range : { 3.0f, 6.0f, 12.0f, 24.0f, 36.0f, 48.0f })
    {
        Rig rig (fs);
        rig.band (1, "fLo", 500.0f);    rig.band (1, "fHi", 2000.0f);
        rig.band (1, "thresh", -80.0f); rig.band (1, "ratio", 100.0f); rig.band (1, "knee", 0.0f);
        rig.band (1, "range", range);   rig.band (1, "attack", 1.0f);  rig.band (1, "release", 100.0f);
        const double measured = steadyGrDb (rig, fc, 0.5, 0.6);
        INFO ("range " << range);
        CHECK_THAT (measured, WithinAbs (-range, 0.05));
        rows.push_back ({ range, measured });
    }
    measure::writeCsv ("range_limit.csv", { "range_db", "measured_gr_db" }, rows);

    // Range 0 disables the band's compression exactly.
    Rig rig (fs, 2);
    rig.band (1, "thresh", -80.0f); rig.band (1, "ratio", 100.0f); rig.band (1, "range", 0.0f);
    const auto in = makeNoise (2, 10000, 4u, 0.9f);
    CHECK (bitIdentical (in, rig.render (in)));
}

TEST_CASE ("Automation: parameter steps are smoothed", "[dsp][compressor]")
{
    const double fs = 48000.0;
    const int n = (int) (0.8 * fs);
    const auto x = sine (1000.0, fs, n, 0.5);
    double maxStepIn = 0.0;
    for (int i = 1; i < n; ++i) maxStepIn = std::max (maxStepIn, (double) std::abs (x[(size_t) i] - x[(size_t) i - 1]));

    SECTION ("threshold step: GR follows the attack smoother")
    {
        Rig rig (fs);
        rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
        rig.band (1, "thresh", 12.0f); rig.band (1, "ratio", 100.0f); rig.band (1, "attack", 5.0f);
        // The peak level is only ~10 dB above the point where the 24 dB range clamps; for ~22 % of
        // each cycle (near zero crossings) the target is shallower. A long release keeps that ripple
        // below 0.05 dB (with 150 ms the mean settles 0.08 dB short of the range).
        rig.band (1, "release", 1000.0f);
        Trace tr (*rig.proc, n);
        rig.renderer.render (toBuffer ({ x }), nullptr, { 128 }, false, [&] (int block, PluginProcessor& p)
        {
            if (block == 100) setBand (p, 1, "thresh", -40.0f);
        });
        const int stepAt = 100 * 128;
        CHECK (tr.db (1, 0, stepAt - 1) == 0.0);
        const double finalGr = tr.db (1, 0, n - 1);
        const double b = 1.0 - std::exp (-1.0 / (0.005 * fs));
        double maxDelta = 0.0;
        for (int i = stepAt; i < n; ++i) maxDelta = std::max (maxDelta, std::abs (tr.db (1, 0, i) - tr.db (1, 0, i - 1)));
        INFO ("final " << finalGr << " max per-sample GR change " << maxDelta);
        CHECK_THAT (finalGr, WithinAbs (-24.0, 0.05));   // ratio inf, limited by the default 24 dB range
        CHECK (maxDelta <= b * 60.0 + 1e-3);   // bounded by (1 - aA) * max target
    }

    SECTION ("edge jump: band glides (20 ms log2 smoothing), no click")
    {
        Rig rig (fs);
        rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
        rig.band (1, "thresh", -40.0f); rig.band (1, "ratio", 100.0f); rig.band (1, "range", 12.0f);
        rig.band (1, "attack", 1.0f); rig.band (1, "release", 50.0f);
        const auto out = rig.renderer.render (toBuffer ({ x }), nullptr, { 128 }, false, [&] (int block, PluginProcessor& p)
        {
            if (block == 150) { setBand (p, 1, "fLo", 4000.0f); setBand (p, 1, "fHi", 16000.0f); }
        });
        double maxStepOut = 0.0, peakOut = 0.0;
        for (int i = 140 * 128; i < n; ++i)
        {
            maxStepOut = std::max (maxStepOut, (double) std::abs (out.getSample (0, i) - out.getSample (0, i - 1)));
            peakOut = std::max (peakOut, (double) std::abs (out.getSample (0, i)));
        }
        INFO ("max |dy| " << maxStepOut << " vs input " << maxStepIn << ", peak " << peakOut);
        CHECK (maxStepOut <= maxStepIn * 1.01);
        CHECK (peakOut <= 0.5 * 1.0012);   // within +0.01 dB of the input peak
        // Band moved two octaves above the tone: only the bell skirt remains. The detector (HP at
        // 4 kHz, -24 dB at 1 kHz) still sees the tone at about -30 dB, so the band keeps ~9-10 dB GR.
        const double grNow = rig.proc->getGainReductionMeter().getGainReductionDb (1, 0);
        const double expected = magDb (a3Model (1000.0, fs, { { 4000.0, 16000.0, grNow } }));
        CHECK (grNow < -6.0);
        CHECK_THAT (dbOf (rmsOf (out, 0, n - 4800, n) / (0.5 / std::sqrt (2.0))), WithinAbs (expected, 0.02));
    }

    SECTION ("random per-block automation of every DSP parameter stays finite and bounded")
    {
        Rig rig (fs, 2, 8);
        const auto in = makeNoise (2, n, 9u, 0.8f);
        std::mt19937 rng (5);
        std::uniform_real_distribution<float> u (0.0f, 1.0f);
        const char* ids[] { "fLo", "fHi", "thresh", "ratio", "knee", "attack", "release", "range", "detMode", "detBlend", "rmsTime", "link", "bypass" };
        const auto out = rig.renderer.render (in, nullptr, { 1, 64, 300, 17 }, false, [&] (int, PluginProcessor& p)
        {
            for (int s = 1; s <= 8; ++s)
                for (auto* id : ids)
                    if (u (rng) < 0.3f) p.getState().getParameter (schema::bandParamId (s, id))->setValueNotifyingHost (u (rng));
            if (u (rng) < 0.05f) p.getState().getParameter ("bypass")->setValueNotifyingHost (u (rng) < 0.5f ? 0.0f : 1.0f);
        });
        float peak = 0.0f;
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
            {
                REQUIRE (std::isfinite (out.getSample (c, i)));
                peak = std::max (peak, std::abs (out.getSample (c, i)));
            }
        CHECK (peak < 2.0f);
        CHECK (rig.proc->getEngine().getStateResetCount() == 0u);
    }
}

TEST_CASE ("Stereo: alignment and link", "[dsp][compressor]")
{
    const double fs = 48000.0;
    const int n = 30000;

    SECTION ("identical channels give bit-identical outputs, linked or not; mono matches")
    {
        const auto mono = makeNoise (1, n, 21u, 0.8f);
        juce::AudioBuffer<float> st (2, n);
        st.copyFrom (0, 0, mono, 0, 0, n);
        st.copyFrom (1, 0, mono, 0, 0, n);

        for (float link : { 0.0f, 50.0f, 100.0f })
        {
            Rig rig (fs, 2, 8);
            for (int s = 1; s <= 8; ++s) { rig.band (s, "thresh", -30.0f); rig.band (s, "link", link); }
            const auto out = rig.render (st);
            CHECK (std::memcmp (out.getReadPointer (0), out.getReadPointer (1), sizeof (float) * (size_t) n) == 0);

            Rig rm (fs, 1, 8);
            for (int s = 1; s <= 8; ++s) { rm.band (s, "thresh", -30.0f); rm.band (s, "link", link); }
            const auto outMono = rm.render (mono);
            CHECK (std::memcmp (out.getReadPointer (0), outMono.getReadPointer (0), sizeof (float) * (size_t) n) == 0);
        }
    }

    SECTION ("link 100: identical gains; link 0: quiet channel untouched; link 50: halfway")
    {
        juce::AudioBuffer<float> st (2, n);
        const auto loud = sine (1000.0, fs, n, 0.5), quiet = sine (1000.0, fs, n, 0.001);
        st.copyFrom (0, 0, loud.data(), n);
        st.copyFrom (1, 0, quiet.data(), n);

        for (float link : { 0.0f, 50.0f, 100.0f })
        {
            Rig rig (fs, 2);
            rig.band (1, "fLo", 500.0f); rig.band (1, "fHi", 2000.0f);
            rig.band (1, "thresh", -30.0f); rig.band (1, "ratio", 4.0f); rig.band (1, "link", link);
            Trace tr (*rig.proc, n);
            const auto out = rig.render (st);
            INFO ("link " << link);
            if (link == 100.0f)
                CHECK (std::memcmp (tr.g[0][0].data(), tr.g[0][1].data(), sizeof (float) * (size_t) n) == 0);
            if (link == 0.0f)
                CHECK (std::memcmp (out.getReadPointer (1), st.getReadPointer (1), sizeof (float) * (size_t) n) == 0);
            if (link == 50.0f)
                CHECK_THAT (tr.db (1, 1, n - 1), WithinAbs (0.5 * tr.db (1, 0, n - 1), 0.05));
        }
    }

    SECTION ("unlinked gain differences rotate the interchannel phase (A3 limitation, research: ~37 deg)")
    {
        const MeasuredResponse l (engineImpulseResponse (fs, { { 707.1, 1414.2, -12.0 } }, 16384), fs);
        double maxPhase = 0.0;
        for (double f : logGrid (20.0, 20000.0, 2000)) maxPhase = std::max (maxPhase, std::abs (phaseDeg (l.at (f))));
        CHECK_THAT (maxPhase, WithinAbs (36.76, 0.05));   // the other channel at 0 dB GR has 0 deg
    }
}
