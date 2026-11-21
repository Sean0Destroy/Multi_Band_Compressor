// Frequency responses of the A3 region engine at fixed gain reduction, measured from impulse
// responses of the production float32 engine (ExternalGains hook), compared with the float64
// model and with the P0-R / P0-R2 research reference values.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <algorithm>
#include <cmath>
#include <random>

#include "dsp/GrSmoother.h"
#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kModelTolDb = 0.005;   // measured float32 engine vs float64 model, any frequency
constexpr double kRefTolDb = 0.011;     // research values are quoted to 2 decimals

int impulseLength (double fs) { return nextPow2 ((int) (0.5 * fs)); }

struct BandMetrics
{
    double centreDb, halfDepthOct, edgeLoDb, edgeHiDb, outLo1OctDb, outHi1OctDb, coreDepthSpanDb, maxPhaseDeg, maxModelDevDb;
};

BandMetrics measureBand (double fs, double fLo, double fHi, double gainDb, std::vector<std::vector<double>>* curve = nullptr)
{
    const std::vector<RegionSpec> spec { { fLo, fHi, gainDb } };
    const MeasuredResponse r (engineImpulseResponse (fs, spec, impulseLength (fs)), fs);

    BandMetrics m {};
    const double fc = digitalCentre (fLo, fHi, fs);
    m.centreDb = magDb (r.exact (fc));
    m.edgeLoDb = magDb (r.exact (fLo));
    m.edgeHiDb = magDb (r.exact (fHi));
    m.outLo1OctDb = magDb (r.exact (fLo / 2.0));
    m.outHi1OctDb = magDb (r.exact (std::min (fHi * 2.0, 0.49 * fs)));

    const double fA = r.crossing (5.0, fc, gainDb / 2.0);
    const double fB = r.crossing (fc, 0.499 * fs, gainDb / 2.0);
    m.halfDepthOct = std::log2 (fB / fA);

    // Span (max - min) of the magnitude over the central half of the band (in octaves). For the A3
    // bell this is the bell's own depth variation, not a ripple; junction ripple is measured by the
    // adjacent-band test.
    const double w = std::log2 (fHi / fLo);
    double coreMin = 1e9, coreMax = -1e9;
    for (double f : logGrid (fLo * std::pow (2.0, w / 4.0), fHi * std::pow (2.0, -w / 4.0), 200))
    {
        const double d = magDb (r.at (f));
        coreMin = std::min (coreMin, d);
        coreMax = std::max (coreMax, d);
    }
    m.coreDepthSpanDb = coreMax - coreMin;

    for (double f : logGrid (20.0, std::min (20000.0, 0.49 * fs), 1500))
    {
        const auto h = r.at (f);
        m.maxPhaseDeg = std::max (m.maxPhaseDeg, std::abs (phaseDeg (h)));
        m.maxModelDevDb = std::max (m.maxModelDevDb, std::abs (magDb (h) - magDb (a3Model (f, fs, spec))));
        if (curve != nullptr)
            curve->push_back ({ f, magDb (h), phaseDeg (h), magDb (a3Model (f, fs, spec)) });
    }
    return m;
}
} // namespace

TEST_CASE ("A3 response: research reference, 2-octave band 500-2000 Hz at 48 kHz", "[dsp][response]")
{
    const double fs = 48000.0;
    for (double gd : { -3.0, -6.0, -12.0, -24.0, -48.0 })
    {
        const auto m = measureBand (fs, 500.0, 2000.0, gd);
        INFO ("GR " << gd);
        CHECK_THAT (m.centreDb, WithinAbs (gd, 1e-3));                // centre exactly g
        CHECK (m.maxModelDevDb < kModelTolDb);
        CHECK (m.edgeLoDb <= 0.0);

        if (gd == -12.0)
        {
            CHECK_THAT (m.edgeLoDb, WithinAbs (-2.74, kRefTolDb));       // research: -2.74 dB at the nominal edge
            CHECK_THAT (m.edgeHiDb, WithinAbs (-2.74, kRefTolDb));
            CHECK_THAT (m.halfDepthOct, WithinAbs (1.06, kRefTolDb));    // research half-depth width
            // One octave outside a 2-octave band (float64 model: -0.602 / -0.581 dB). The requirement's
            // -0.32/-0.31 dB belong to the 1-octave band (next test), see the P0-R E2 table.
            CHECK_THAT (m.outLo1OctDb, WithinAbs (-0.602, 0.006));
            CHECK_THAT (m.outHi1OctDb, WithinAbs (-0.581, 0.006));
        }
        if (gd == -24.0) CHECK_THAT (m.halfDepthOct, WithinAbs (0.54, kRefTolDb));
        if (gd == -48.0) CHECK_THAT (m.halfDepthOct, WithinAbs (0.14, kRefTolDb));
    }
}

TEST_CASE ("A3 response: research reference, 1-octave band at 1 kHz, -12 dB", "[dsp][response]")
{
    const auto m = measureBand (48000.0, 1000.0 / std::sqrt (2.0), 1000.0 * std::sqrt (2.0), -12.0);
    CHECK_THAT (m.centreDb, WithinAbs (-12.0, 1e-3));
    CHECK_THAT (m.edgeLoDb, WithinAbs (-2.74, kRefTolDb));
    CHECK_THAT (m.outLo1OctDb, WithinAbs (-0.32, kRefTolDb));   // P0-R E2: -0.32 / -0.31 dB
    CHECK_THAT (m.outHi1OctDb, WithinAbs (-0.31, kRefTolDb));
}

TEST_CASE ("A3 bandwidth versus gain reduction: narrow, medium, broad", "[dsp][response]")
{
    const double fs = 48000.0;
    struct Width { const char* name; double oct; };
    std::vector<std::vector<double>> table;

    for (auto w : { Width { "narrow", 1.0 / 3.0 }, Width { "medium", 1.0 }, Width { "broad", 3.0 } })
    {
        const double fLo = 1000.0 * std::pow (2.0, -w.oct / 2.0), fHi = 1000.0 * std::pow (2.0, w.oct / 2.0);
        std::vector<std::vector<double>> curves;
        double prevHalf = 1e9;

        for (double gd : { -3.0, -6.0, -12.0, -24.0, -48.0 })
        {
            std::vector<std::vector<double>> curve;
            const auto m = measureBand (fs, fLo, fHi, gd, &curve);
            INFO (w.name << " GR " << gd);

            CHECK_THAT (m.centreDb, WithinAbs (gd, 1e-3));
            CHECK (m.maxModelDevDb < kModelTolDb);
            CHECK (m.halfDepthOct < prevHalf);                 // the bell narrows as GR deepens
            prevHalf = m.halfDepthOct;
            CHECK (m.edgeLoDb > -3.02);                         // nominal edges never reach 1/2 depth beyond -6 dB
            CHECK (m.outLo1OctDb > -0.82);                     // bell skirts: at most ~0.81 dB one octave outside (3-oct, -48 dB)
            CHECK (m.maxPhaseDeg < 90.0);

            table.push_back ({ w.oct, gd, m.centreDb, m.halfDepthOct, m.edgeLoDb, m.edgeHiDb, m.outLo1OctDb,
                               m.outHi1OctDb, m.coreDepthSpanDb, m.maxPhaseDeg, m.maxModelDevDb });

            if (curves.empty()) for (auto& row : curve) curves.push_back ({ row[0] });
            for (size_t i = 0; i < curve.size(); ++i)
            {
                curves[i].push_back (curve[i][1]);
                curves[i].push_back (curve[i][2]);
            }
        }

        measure::writeCsv (std::string ("a3_response_") + w.name + ".csv",
                           { "freq_hz", "mag_db_gr3", "phase_deg_gr3", "mag_db_gr6", "phase_deg_gr6", "mag_db_gr12", "phase_deg_gr12",
                             "mag_db_gr24", "phase_deg_gr24", "mag_db_gr48", "phase_deg_gr48" }, curves);
    }

    measure::writeCsv ("a3_metrics.csv", { "width_oct", "gr_db", "centre_db", "half_depth_width_oct", "edge_lo_db", "edge_hi_db",
                                           "lo_1oct_out_db", "hi_1oct_out_db", "core_depth_span_db", "max_abs_phase_deg", "max_dev_vs_f64_model_db" }, table);
}

TEST_CASE ("Adjacent bands: junction ripple (research reference)", "[dsp][response]")
{
    const double fs = 48000.0;
    const std::vector<RegionSpec> both { { 250.0, 1000.0, -6.0 }, { 1000.0, 4000.0, -6.0 } };
    const MeasuredResponse r (engineImpulseResponse (fs, both, impulseLength (fs)), fs);
    const MeasuredResponse a (engineImpulseResponse (fs, { both[0] }, impulseLength (fs)), fs);
    const MeasuredResponse b (engineImpulseResponse (fs, { both[1] }, impulseLength (fs)), fs);

    const double junction = magDb (r.exact (1000.0));
    double dip = 1e9, maxDev = 0.0, maxDb = -1e9;
    std::vector<std::vector<double>> rows;
    for (double f : logGrid (20.0, 20000.0, 2000))
    {
        const double m = magDb (r.at (f));
        dip = std::min (dip, m);
        maxDb = std::max (maxDb, m);
        maxDev = std::max (maxDev, std::abs (m - magDb (a3Model (f, fs, both))));
        rows.push_back ({ f, m, phaseDeg (r.at (f)), magDb (a.at (f)), magDb (b.at (f)) });
    }

    CHECK_THAT (junction, WithinAbs (-4.07, kRefTolDb));        // research: -4.07 dB at the junction
    CHECK_THAT (dip, WithinAbs (-6.49, kRefTolDb));             // research: -6.49 dB dips
    CHECK_THAT (junction - dip, WithinAbs (2.415, 0.006));      // model 2.415 dB (the 2.42 quoted = rounded -4.07 - -6.49)
    CHECK (maxDev < kModelTolDb);
    CHECK (maxDb <= 1e-6);                                      // cuts never boost

    measure::writeCsv ("adjacent_ripple.csv", { "freq_hz", "combined_db", "combined_phase_deg", "band_250_1000_db", "band_1000_4000_db" }, rows);
}

TEST_CASE ("Overlapping bands: bounds and order independence", "[dsp][response]")
{
    const double fs = 48000.0;
    const int len = impulseLength (fs);
    struct Case { double ga, gb, overlapOct; };

    for (auto c : { Case { -6, -6, 1.0 }, Case { -12, -3, 1.0 }, Case { -12, -12, 2.0 }, Case { -24, -6, 0.5 } })
    {
        const RegionSpec A { 250.0, 1000.0, c.ga, 1 };
        const double sh = std::pow (2.0, 2.0 - c.overlapOct);
        const RegionSpec B { 250.0 * sh, 1000.0 * sh, c.gb, 2 };
        RegionSpec A2 = A, B2 = B;
        A2.slot = 2; B2.slot = 1;   // swapped cascade order

        const auto h1 = engineImpulseResponse (fs, { A, B }, len);
        const auto h2 = engineImpulseResponse (fs, { A2, B2 }, len);

        double maxDiff = 0.0, peak = 0.0;
        for (size_t i = 0; i < h1.size(); ++i)
        {
            maxDiff = std::max (maxDiff, (double) std::abs (h1[i] - h2[i]));
            peak = std::max (peak, (double) std::abs (h1[i]));
        }
        INFO ("gains " << c.ga << "/" << c.gb << " overlap " << c.overlapOct);
        CHECK (dbOf (maxDiff / peak) < -100.0);   // static cascade of LTI sections commutes (float32 floor)

        const MeasuredResponse r (h1, fs);
        double lowest = 1e9, highest = -1e9;
        for (double f : logGrid (20.0, 20000.0, 1500))
        {
            const double m = magDb (r.at (f));
            lowest = std::min (lowest, m);
            highest = std::max (highest, m);
        }
        CHECK (lowest >= c.ga + c.gb - 1e-3);     // S1 bound: never deeper than the sum of the band GRs
        CHECK (highest <= 1e-6);                  // never above unity
    }
}

TEST_CASE ("Sample-rate dependence: bilinear warping vs analog prototype (research reference)", "[dsp][response]")
{
    std::vector<std::vector<double>> rows;
    const std::vector<RegionSpec> spec { { 8000.0, 16000.0, -12.0 } };
    struct Ref { double fs, dev; };
    std::vector<std::vector<double>> curveRows;
    const auto grid = logGrid (1000.0, 20000.0, 800);
    std::vector<std::vector<double>> curves (grid.size());
    for (size_t i = 0; i < grid.size(); ++i) curves[i] = { grid[i], magDb (a3Model (grid[i], 48000.0, spec, true)) };

    for (auto ref : { Ref { 44100.0, 3.12 }, Ref { 48000.0, 2.52 }, Ref { 88200.0, 0.640 }, Ref { 96000.0, 0.54 }, Ref { 192000.0, 0.13 } })
    {
        const MeasuredResponse r (engineImpulseResponse (ref.fs, spec, impulseLength (ref.fs)), ref.fs);
        double maxDev = 0.0, maxModel = 0.0;
        for (size_t i = 0; i < grid.size(); ++i)
        {
            const double f = grid[i];
            const double m = magDb (r.at (f));
            curves[i].push_back (f < 0.49 * ref.fs ? m : std::nan (""));
            if (f >= 0.49 * ref.fs) continue;   // research metric: f in [1, 20] kHz and f < 0.49 fs
            maxDev = std::max (maxDev, std::abs (m - magDb (a3Model (f, ref.fs, spec, true))));
            maxModel = std::max (maxModel, std::abs (m - magDb (a3Model (f, ref.fs, spec))));
        }
        INFO ("fs " << ref.fs);
        CHECK_THAT (maxDev, WithinAbs (ref.dev, ref.fs == 88200.0 ? 0.006 : kRefTolDb));
        CHECK (maxModel < kModelTolDb);
        CHECK_THAT (magDb (r.exact (digitalCentre (8000.0, 16000.0, ref.fs))), WithinAbs (-12.0, 1e-3));
        rows.push_back ({ ref.fs, maxDev, maxModel });
    }

    measure::writeCsv ("samplerate_warping.csv", { "fs_hz", "max_dev_vs_analog_db", "max_dev_vs_f64_bilinear_model_db" }, rows);
    measure::writeCsv ("samplerate_8k_16k_response.csv", { "freq_hz", "analog_db", "fs44100_db", "fs48000_db", "fs88200_db", "fs96000_db", "fs192000_db" }, curves);
}

TEST_CASE ("Sample-rate dependence: low bands stay accurate in float32", "[dsp][response]")
{
    for (double fs : { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 })
        for (auto band : { RegionSpec { 20.0, 40.0, -12.0 }, RegionSpec { 40.0, 80.0, -24.0 } })
        {
            const MeasuredResponse r (engineImpulseResponse (fs, { band }, nextPow2 ((int) (1.0 * fs))), fs);
            double maxDev = 0.0;
            for (double f : logGrid (10.0, 1000.0, 400))
                maxDev = std::max (maxDev, std::abs (magDb (r.at (f)) - magDb (a3Model (f, fs, { band }))));
            INFO ("fs " << fs << " band " << band.fLo);
            CHECK (maxDev < 0.01);
        }
}

TEST_CASE ("Dynamic overshoot: 8 contiguous bands with modulated gains (research reference)", "[dsp][response]")
{
    // P0-R2 review (chk2/chk2b): 8 contiguous bands, independent square GR trajectories to -24 dB
    // (rates/phases from numpy default_rng(7)), tones geomspace(30, 16000, 41), amplitude 0.25,
    // peak over input after a 0.25 s warm-up. Instantaneous gain steps reach about +1.1 dB in the
    // cascade; GR-smoothed gains (attack 0.1 ms, release >= 1 ms) show no overshoot.
    const double fs = 48000.0, pi = 3.14159265358979323846;
    const int n = (int) fs, w0 = (int) fs / 4;
    const double lo[8] { 40, 100, 250, 600, 1500, 3500, 7000, 12000 };
    const double hi[8] { 100, 250, 600, 1500, 3500, 7000, 12000, 18000 };
    const double fm[8] { 11.75133653, 15.56099321, 13.85959966, 6.15290066, 7.20232799, 15.22974824, 3.07371426, 14.49719786 };
    const double ph[8] { 5.00559601, 2.9386315, 1.90304364, 1.74851284, 1.60058101, 2.7950792, 3.16856307, 3.47596337 };

    auto worstPeak = [&] (double attackMs, double releaseMs, bool instant)
    {
        std::vector<std::vector<float>> g (8, std::vector<float> ((size_t) n));
        for (int b = 0; b < 8; ++b)
        {
            dsp::GrSmoother sm;
            sm.setTimes (attackMs, releaseMs, fs);
            for (int i = 0; i < n; ++i)
            {
                const double t = i / fs;
                const float sq = (i < w0 / 2 || std::sin (2 * pi * fm[b] * t + ph[b]) <= 0.0) ? 0.0f : -24.0f;
                g[(size_t) b][(size_t) i] = instant ? (float) dbToGain (sq) : dsp::GrSmoother::gainFromAmount (sm.tick (sq));
            }
        }

        double worst = -1e9;
        for (double f : logGrid (30.0, 16000.0, 41))
        {
            dsp::A3RegionEngine e;
            e.prepare (fs, 512);
            dsp::EngineSettings s;
            dsp::ExternalGains ext;
            ext.length = n;
            for (int b = 0; b < 8; ++b)
            {
                s.usedMask |= 1u << b;
                s.bands[(size_t) b].fLoHz = (float) lo[b];
                s.bands[(size_t) b].fHiHz = (float) hi[b];
                ext.data[b] = g[(size_t) b].data();
            }
            e.setSettings (s);
            e.setExternalGains (&ext);
            auto x = sine (f, fs, n, 0.25, pi / 2.0);   // cosine like the reference
            float* ch[1] { x.data() };
            e.process (ch, 1, n);
            float pk = 0.0f;
            for (int i = w0; i < n; ++i) pk = std::max (pk, std::abs (x[(size_t) i]));
            worst = std::max (worst, dbOf (pk / 0.25));
        }
        return worst;
    };

    const double instant = worstPeak (0.0, 0.0, true);
    CHECK_THAT (instant, WithinAbs (1.113, 0.02));   // research model: +1.113 dB (float64)

    std::vector<std::vector<double>> rows { { 0.0, 0.0, instant } };
    for (double rel : { 1.0, 5.0, 30.0 })
    {
        const double pk = worstPeak (0.1, rel, false);
        INFO ("release " << rel);
        CHECK (pk <= 0.005);   // no overshoot with smoothed gains (research: -0.000 / -0.021 / -1.094 dB)
        rows.push_back ({ 0.1, rel, pk });
    }
    measure::writeCsv ("dynamic_overshoot.csv", { "attack_ms", "release_ms", "worst_peak_over_input_db" }, rows);
}

namespace
{
/** float64 model of one A3 region with moving edges: the engine's edge smoother (double state,
    float log2 targets, 1e-9 octave snap), the runtime clamp, and coefficients recomputed only
    where (n - phase) % grid == 0 (grid 1 = every sample). Targets change at block starts. */
std::vector<double> gridModel (const std::vector<float>& x, double fs, int block, double gainDb, int grid, int phase,
                               const std::vector<std::pair<int, std::pair<float, float>>>& edgeChanges)
{
    const double pi = 3.14159265358979323846;
    const double b = 1.0 - std::exp (-1.0 / (0.020 * fs));
    const float minLog2 = (float) 3.32192809488736234787, maxLog2 = (float) std::log2 (0.45 * fs), minRatio = (float) (1.0 / 24.0);
    const double G = (double) (float) dbToGain (gainDb);

    auto targets = [&] (float lo, float hi, double& tl, double& th)
    {
        const float a = std::log2 (lo), c = std::log2 (hi);
        tl = (double) std::min (a, c);
        th = (double) std::max (a, c);
    };

    double tl = 0, th = 0, sl = 0, sh = 0;
    targets (edgeChanges[0].second.first, edgeChanges[0].second.second, tl, th);
    sl = tl; sh = th;
    double g = 0, R = 0, s1 = 0, s2 = 0;
    auto coeffs = [&]
    {
        const float lo = std::clamp ((float) sl, minLog2, maxLog2 - minRatio);
        const float hi = std::clamp ((float) sh, lo + minRatio, maxLog2);
        const double wl = std::tan (pi * std::exp2 ((double) lo) / fs), wh = std::tan (pi * std::exp2 ((double) hi) / fs);
        g = std::sqrt (wl * wh);
        R = (wh - wl) / (2.0 * g);
    };
    coeffs();

    std::vector<double> y (x.size());
    size_t change = 1;
    for (int n = 0; n < (int) x.size(); ++n)
    {
        if (n % block == 0 && change < edgeChanges.size() && edgeChanges[change].first <= n)
        {
            targets (edgeChanges[change].second.first, edgeChanges[change].second.second, tl, th);
            ++change;
        }

        const double nl = sl + b * (tl - sl); sl = std::abs (tl - nl) < 1e-9 ? tl : nl;
        const double nh = sh + b * (th - sh); sh = std::abs (th - nh) < 1e-9 ? th : nh;
        if (n >= phase && (n - phase) % grid == 0) coeffs();

        const double in = x[(size_t) n];
        const double hp = (in - (2.0 * R + g) * s1 - s2) / (1.0 + 2.0 * R * g + g * g);
        const double bp = g * hp + s1, lp = g * bp + s2;
        s1 = g * hp + bp;
        s2 = g * bp + lp;
        y[(size_t) n] = in + (G - 1.0) * 2.0 * R * bp;
    }
    return y;
}
} // namespace

TEST_CASE ("Coefficients follow moving edges only on the absolute 16-sample grid", "[dsp][response]")
{
    // Odd host blocks (333) so that block starts never align with the grid; edges jump twice
    // (the second jump with reversed edges) while the gain is held at -12 dB.
    const double fs = 48000.0;
    const int n = 48000, block = 333;
    const auto noise = makeNoise (1, n, 14u, 0.5f);
    std::vector<float> x (noise.getReadPointer (0), noise.getReadPointer (0) + n);

    // Edge targets per block start (the block containing the sample index applies them).
    std::vector<std::pair<int, std::pair<float, float>>> changes { { 0, { 300.0f, 1200.0f } } };
    for (int pos = 0; pos < n; pos += block)
    {
        if (pos <= 10007 && 10007 < pos + block) changes.push_back ({ pos, { 2000.0f, 6000.0f } });
        if (pos <= 30001 && 30001 < pos + block) changes.push_back ({ pos, { 5000.0f, 100.0f } });
    }

    dsp::A3RegionEngine e;
    e.prepare (fs, 512);
    dsp::EngineSettings s;
    s.usedMask = 1u;
    s.bands[0].fLoHz = changes[0].second.first;
    s.bands[0].fHiHz = changes[0].second.second;
    const float g = (float) dbToGain (-12.0);
    dsp::ExternalGains ext;
    ext.data[0] = &g;
    e.setSettings (s);
    e.setExternalGains (&ext);

    std::vector<float> y = x;
    size_t next = 1;
    for (int pos = 0; pos < n; pos += block)
    {
        if (next < changes.size() && changes[next].first == pos)
        {
            s.bands[0].fLoHz = changes[next].second.first;
            s.bands[0].fHiHz = changes[next].second.second;
            ++next;
        }
        e.setSettings (s);
        float* ch[1] { y.data() + pos };
        e.process (ch, 1, std::min (block, n - pos));
    }

    auto maxDiffDb = [&] (const std::vector<double>& m)
    {
        double d = 0.0;
        for (int i = 0; i < n; ++i) d = std::max (d, std::abs ((double) y[(size_t) i] - m[(size_t) i]));
        return dbOf (d);
    };

    const double onGrid = maxDiffDb (gridModel (x, fs, block, -12.0, 16, 0, changes));
    const double everySample = maxDiffDb (gridModel (x, fs, block, -12.0, 1, 0, changes));
    const double shifted = maxDiffDb (gridModel (x, fs, block, -12.0, 16, 1, changes));
    const double grid8 = maxDiffDb (gridModel (x, fs, block, -12.0, 8, 0, changes));
    INFO ("max |engine - model| dBFS: grid 16 " << onGrid << ", grid 1 " << everySample << ", grid 16 shifted " << shifted << ", grid 8 " << grid8);
    CHECK (onGrid < -100.0);
    // The model is sensitive enough to tell the grids apart (the test cannot pass vacuously).
    CHECK (everySample > -80.0);
    CHECK (shifted > -80.0);
    CHECK (grid8 > -80.0);
}
