// The second experimental band engine (A-prime) and switching between engines.
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <complex>
#include <memory>

#include "dsp/A3Model.h"
#include "dsp/APrimeRegionEngine.h"
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

struct EngineRun
{
    std::unique_ptr<dsp::IBandEngine> engine;
    dsp::EngineSettings settings;

    explicit EngineRun (Kind kind)
    {
        if (kind == Kind::APrime) engine = std::make_unique<dsp::APrimeRegionEngine>();
        else                      engine = std::make_unique<dsp::A3RegionEngine>();
        engine->prepare (kFs, 512);
    }

    void use (int slot, double lo, double hi)
    {
        settings.usedMask |= 1u << (slot - 1);
        settings.bands[(size_t) slot - 1].fLoHz = (float) lo;
        settings.bands[(size_t) slot - 1].fHiHz = (float) hi;
        settings.bands[(size_t) slot - 1].thresholdDb = 12.0f;
    }

    void run (std::vector<std::vector<float>>& channels, int block = 128)
    {
        const int n = (int) channels[0].size();
        for (int pos = 0; pos < n; pos += block)
        {
            const int len = std::min (block, n - pos);
            float* ptr[2] {};
            for (size_t c = 0; c < channels.size(); ++c) ptr[c] = channels[c].data() + pos;
            engine->setSettings (settings);
            engine->process (ptr, (int) channels.size(), len);
        }
    }
};

double responseDb (Kind kind, double lo, double hi, double gainDb, double f)
{
    const auto h = engineImpulseResponse (kind, kFs, { { lo, hi, gainDb, 1 } }, 1 << 15, 2 * 48000);
    return magDb (MeasuredResponse (h, kFs).exact (f));
}
}

TEST_CASE ("A-prime: the settled response is the symmetric peaking form", "[dsp][aprime]")
{
    struct Case { double lo, hi, gainDb; };
    for (const Case& c : { Case { 500.0, 2000.0, -12.0 }, Case { 2000.0, 8000.0, -24.0 }, Case { 100.0, 200.0, 6.0 }, Case { 300.0, 3000.0, -3.0 } })
    {
        INFO ("band " << c.lo << " - " << c.hi << " Hz at " << c.gainDb << " dB");
        const auto h = engineImpulseResponse (Kind::APrime, kFs, { { c.lo, c.hi, c.gainDb, 1 } }, 1 << 15, 2 * 48000);
        const MeasuredResponse measured (h, kFs);
        const double centre = digitalCentre (c.lo, c.hi, kFs);

        for (double f : { 0.25 * centre, 0.5 * centre, c.lo, centre, c.hi, 2.0 * centre, 4.0 * centre })
        {
            const double expected = magDb (dsp::aPrimeRegionResponse (f, kFs, c.lo, c.hi, dbToGain (c.gainDb)));
            INFO ("f " << f);
            CHECK_THAT (magDb (measured.exact (f)), WithinAbs (expected, 0.05));
        }

        // Full depth at the centre, exactly half the command (in dB) at both nominal edges.
        CHECK_THAT (magDb (measured.exact (centre)), WithinAbs (c.gainDb, 0.05));
        CHECK_THAT (magDb (measured.exact (c.lo)), WithinAbs (0.5 * c.gainDb, 0.05));
        CHECK_THAT (magDb (measured.exact (c.hi)), WithinAbs (0.5 * c.gainDb, 0.05));
    }

    // Cut and boost are mirror images.
    for (double f : { 400.0, 700.0, 1000.0, 1600.0, 3000.0 })
        CHECK_THAT (responseDb (Kind::APrime, 500.0, 2000.0, 12.0, f) + responseDb (Kind::APrime, 500.0, 2000.0, -12.0, f), WithinAbs (0.0, 0.05));
}

TEST_CASE ("A-prime keeps its width with depth where A3 narrows", "[dsp][aprime]")
{
    // One-octave band, 24 dB cut: how deep is it at the nominal edges?
    const double lo = 1000.0, hi = 2000.0;
    const double a3Edge = responseDb (Kind::A3, lo, hi, -24.0, lo);
    const double aPrimeEdge = responseDb (Kind::APrime, lo, hi, -24.0, lo);
    INFO ("depth at the lower edge: A3 " << a3Edge << " dB, A-prime " << aPrimeEdge << " dB");
    CHECK_THAT (aPrimeEdge, WithinAbs (-12.0, 0.05));
    CHECK (a3Edge > -3.5);                                               // A3 never gets past about -3 dB there
    CHECK_THAT (responseDb (Kind::A3, lo, hi, -24.0, digitalCentre (lo, hi, kFs)), WithinAbs (-24.0, 0.05));   // same centre depth

    // Two adjacent 2-octave bands at -6 dB: the junction ripple is much smaller.
    const auto ripple = [] (Kind kind)
    {
        const auto h = engineImpulseResponse (kind, kFs, { { 250.0, 1000.0, -6.0, 1 }, { 1000.0, 4000.0, -6.0, 2 } }, 1 << 15, 2 * 48000);
        const MeasuredResponse m (h, kFs);
        double deepest = 0.0, shallowest = -100.0;
        for (double f = 500.0; f <= 2000.0; f *= 1.02)
        {
            deepest = std::min (deepest, magDb (m.exact (f)));
            shallowest = std::max (shallowest, magDb (m.exact (f)));
        }
        return shallowest - deepest;
    };
    CHECK (ripple (Kind::A3) > 2.0);
    CHECK (ripple (Kind::APrime) < 1.2);
}

TEST_CASE ("A-prime: exact null, bounded when the gain moves, same dynamics as A3", "[dsp][aprime]")
{
    const int n = 24000;

    SECTION ("neutral bands leave the signal bit-identical, also in mid/side")
    {
        const auto noise = makeNoise (2, n, 5u, 0.7f);
        for (auto mode : { dsp::StereoMode::Stereo, dsp::StereoMode::MidSide })
        {
            EngineRun run (Kind::APrime);
            for (int s = 1; s <= 8; ++s)
            {
                run.use (s, 60.0 * std::pow (2.0, s), 120.0 * std::pow (2.0, s));
                run.settings.bands[(size_t) s - 1].stereoMode = mode;
            }
            std::vector<std::vector<float>> x (2, std::vector<float> ((size_t) n));
            for (int c = 0; c < 2; ++c) std::copy (noise.getReadPointer (c), noise.getReadPointer (c) + n, x[(size_t) c].begin());
            run.run (x);
            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < n; ++i)
                    REQUIRE (x[(size_t) c][(size_t) i] == noise.getSample (c, i));
        }
    }

    SECTION ("fast gain steps do not overshoot the static bound (the study's worst cases)")
    {
        // Worst cases of the numerical study: band at 1 kHz (1 and 1/3 octave), a tone three octaves
        // BELOW it (the out-of-band filter state is what depends on the damping) or at its centre,
        // the gain switching at 10 / 37 Hz with a 0.01 ms attack and a 1 ms release.
        //   plain gain-dependent filter: +8.5 dB (24 dB steps), +23 dB (48 dB steps)
        //   without the width lag: +1.6 dB / +4.1 dB;   this variant: at most +0.26 dB.
        // Static bound: the largest |1 + (G - 1) B_R'(f)| for any gain and any width gain in the range.
        const auto staticBound = [] (double f, double lo, double hi, double lowDb, double highDb)
        {
            const double wl = std::tan (dsp::kPi * lo / kFs), wh = std::tan (dsp::kPi * hi / kFs);
            const double g = std::sqrt (wl * wh), R = (wh - wl) / (2.0 * g);
            const std::complex<double> sp (0.0, std::tan (dsp::kPi * f / kFs) / g);
            double bound = 1.0;
            for (int a = 0; a <= 24; ++a)
                for (int b = 0; b <= 24; ++b)
                {
                    const double G = dbToGain (lowDb + (highDb - lowDb) * a / 24.0), Gw = dbToGain (lowDb + (highDb - lowDb) * b / 24.0);
                    const double r2 = 2.0 * R / std::sqrt (Gw);
                    bound = std::max (bound, std::abs (1.0 + (G - 1.0) * r2 * sp / (sp * sp + r2 * sp + 1.0)));
                }
            return bound;
        };

        struct Range { double highDb, lowDb; };
        double worst = -100.0;
        for (const Range& range : { Range { 0.0, -24.0 }, Range { 0.0, -48.0 }, Range { 12.0, -12.0 } })
            for (double widthOct : { 1.0, 1.0 / 3.0 })
                for (double fm : { 10.0, 37.0 })
                    for (double tone : { 125.5, 1000.0 })
                    {
                        const double lo = 1000.0 / std::pow (2.0, widthOct / 2.0), hi = 1000.0 * std::pow (2.0, widthOct / 2.0);
                        const int length = 48000;
                        EngineRun run (Kind::APrime);
                        run.use (1, lo, hi);

                        std::vector<float> gain ((size_t) length, 1.0f);
                        double gDb = range.highDb;
                        const double attack = std::exp (-1.0 / (0.00001 * kFs)), release = std::exp (-1.0 / (0.001 * kFs));
                        const int halfPeriod = (int) (kFs / fm / 2.0);
                        for (int i = 0; i < length; ++i)
                        {
                            const double target = (i / halfPeriod) % 2 == 1 ? range.lowDb : range.highDb;
                            gDb = target + (target < gDb ? attack : release) * (gDb - target);
                            gain[(size_t) i] = (float) dbToGain (gDb);
                        }
                        dsp::ExternalGains ext;
                        ext.data[0] = gain.data();
                        ext.length = length;
                        run.engine->setExternalGains (&ext);

                        std::vector<std::vector<float>> x (1, sine (tone, kFs, length, 0.25));
                        run.run (x, 100);
                        float peak = 0.0f;
                        for (float v : x[0]) peak = std::max (peak, std::abs (v));
                        const double over = 20.0 * std::log10 ((double) peak / (0.25 * staticBound (tone, lo, hi, range.lowDb, range.highDb)));
                        worst = std::max (worst, over);
                        INFO ("gain " << range.highDb << " <-> " << range.lowDb << " dB, " << widthOct << " oct, fm " << fm
                              << " Hz, tone " << tone << " Hz: " << over << " dB over the static bound");
                        CHECK (over < 0.4);
                        CHECK (run.engine->getStateResetCount() == 0u);
                    }
        INFO ("worst overshoot " << worst << " dB");
        CHECK (worst < 0.4);
    }

    SECTION ("the gains are the shared dynamics core's: identical traces for both engines")
    {
        const auto noise = makeNoise (1, n, 9u, 0.6f);
        std::vector<float> traces[2];
        for (int k = 0; k < 2; ++k)
        {
            EngineRun run ((Kind) k);
            run.use (1, 300.0, 3000.0);
            run.settings.bands[0].thresholdDb = -40.0f;
            run.settings.bands[0].ratio = 6.0f;
            traces[k].assign ((size_t) n, 1.0f);
            dsp::GainTrace trace;
            trace.data[0][0] = traces[k].data();
            trace.capacity = n;
            run.engine->setGainTrace (&trace);
            std::vector<std::vector<float>> x (1, std::vector<float> (noise.getReadPointer (0), noise.getReadPointer (0) + n));
            run.run (x);
        }
        CHECK (traces[0] == traces[1]);
        CHECK (traces[0][(size_t) n / 2] < 0.9f);
    }

    SECTION ("random extreme gains stay finite")
    {
        EngineRun run (Kind::APrime);
        for (int s = 1; s <= 8; ++s) run.use (s, 40.0 * std::pow (2.0, s), 90.0 * std::pow (2.0, s));
        std::vector<std::vector<float>> gains (8, std::vector<float> ((size_t) n));
        juce::Random rng (1234);
        dsp::ExternalGains ext;
        for (int b = 0; b < 8; ++b)
        {
            for (auto& v : gains[(size_t) b]) v = (float) dbToGain (-60.0 + 84.0 * rng.nextDouble());
            ext.data[b] = gains[(size_t) b].data();
        }
        ext.length = n;
        run.engine->setExternalGains (&ext);
        const auto noise = makeNoise (1, n, 3u, 0.5f);
        std::vector<std::vector<float>> x (1, std::vector<float> (noise.getReadPointer (0), noise.getReadPointer (0) + n));
        run.run (x);
        for (float v : x[0]) REQUIRE (std::isfinite (v));
    }
}

TEST_CASE ("Engine switch (internal): crossfaded, saved, and every load selects A-prime", "[dsp][aprime][engine]")
{
    PluginProcessor p;
    OfflineRenderer renderer (p);
    CHECK (p.getEngineKind() == Kind::APrime);                            // the default (owner's pick, 2026-11-24)

    p.getBandSlots().createBand();
    setBand (p, 1, "fLo", 500.0f); setBand (p, 1, "fHi", 2000.0f);
    setBand (p, 1, "thresh", 12.0f); setBand (p, 1, "makeup", -12.0f);
    REQUIRE (renderer.prepare (kFs, 512, { 1, 0 }));

    // A new project saves its engine. One saved before A-prime became the default has no engine
    // entry; like one saved with A3 selected, it now opens on A-prime (the only engine offered).
    const auto savedEngineEntry = [] (const juce::MemoryBlock& b)
    {
        const auto xml = juce::AudioProcessor::getXmlFromBinary (b.getData(), (int) b.getSize());
        REQUIRE (xml != nullptr);
        const auto* np = xml->getChildByName ("NonParam");
        return np != nullptr && np->hasAttribute ("engine") ? np->getIntAttribute ("engine") : -1;
    };
    juce::MemoryBlock defaultState;
    p.getStateInformation (defaultState);
    CHECK (savedEngineEntry (defaultState) == (int) Kind::APrime);
    juce::MemoryBlock oldState;
    {
        PluginProcessor old;
        old.getState().state.getChildWithName ("NonParam").removeProperty ("engine", nullptr);
        old.getStateInformation (oldState);
        CHECK (savedEngineEntry (oldState) == -1);
    }
    p.setEngineKind (Kind::A3);
    REQUIRE (renderer.prepare (kFs, 512, { 1, 0 }));

    // The two engines differ at the band edge (A3 about -2.7 dB, A-prime -6 dB for a -12 dB band).
    const int n = 48000;
    juce::AudioBuffer<float> edgeTone (1, n);
    const auto s = sine (500.0, kFs, n, 0.5);
    edgeTone.copyFrom (0, 0, s.data(), n);
    const auto level = [&] (const juce::AudioBuffer<float>& b)
    {
        double acc = 0.0;
        for (int i = n / 2; i < n; ++i) acc += (double) b.getSample (0, i) * b.getSample (0, i);
        return 10.0 * std::log10 (acc / (n / 2)) - 10.0 * std::log10 (0.125);
    };
    CHECK_THAT (level (renderer.render (edgeTone, nullptr, { 256 })), WithinAbs (-2.7, 0.2));

    p.setEngineKind (Kind::APrime);
    CHECK (p.getEngineKind() == Kind::APrime);
    REQUIRE (renderer.prepare (kFs, 512, { 1, 0 }));
    CHECK_THAT (level (renderer.render (edgeTone, nullptr, { 256 })), WithinAbs (-6.0, 0.1));

    // Switching while audio runs: a crossfade, no step, and nothing is allocated.
    p.setEngineKind (Kind::A3);
    REQUIRE (renderer.prepare (kFs, 512, { 1, 0 }));
    test::resetAllocationCount();
    const auto out = renderer.render (edgeTone, nullptr, { 128 }, false, [] (int block, PluginProcessor& proc)
    {
        if (block == 100) proc.setEngineKind (Kind::APrime);
        if (block == 200) proc.setEngineKind (Kind::A3);
    });
    CHECK (test::allocationCount() == 0);
    float largestStep = 0.0f;
    for (int i = 1; i < n; ++i)
        largestStep = std::max (largestStep, std::abs (out.getSample (0, i) - out.getSample (0, i - 1)));
    CHECK (largestStep < 1.3f * 0.5f * (float) (2.0 * dsp::kPi * 500.0 / kFs));
    double acc = 0.0;
    for (int i = 150 * 128; i < 190 * 128; ++i) acc += (double) out.getSample (0, i) * out.getSample (0, i);
    CHECK_THAT (10.0 * std::log10 (acc / (40 * 128)) - 10.0 * std::log10 (0.125), WithinAbs (-6.0, 0.15));   // A-prime in between

    // Saved with the project ...
    p.setEngineKind (Kind::APrime);
    juce::MemoryBlock saved;
    p.getStateInformation (saved);
    PluginProcessor reopened;
    reopened.setStateInformation (saved.getData(), (int) saved.getSize());
    CHECK (reopened.getEngineKind() == Kind::APrime);
    reopened.setStateInformation (oldState.getData(), (int) oldState.getSize());
    CHECK (reopened.getEngineKind() == Kind::APrime);                     // an old project opens on A-prime
    reopened.setStateInformation (defaultState.getData(), (int) defaultState.getSize());
    CHECK (reopened.getEngineKind() == Kind::APrime);
    p.setEngineKind (Kind::A3);                                           // saved as A3 explicitly
    juce::MemoryBlock savedA3;
    p.getStateInformation (savedA3);
    CHECK (savedEngineEntry (savedA3) == (int) Kind::A3);
    reopened.setStateInformation (savedA3.getData(), (int) savedA3.getSize());
    CHECK (reopened.getEngineKind() == Kind::APrime);                     // A3 is no longer offered
    p.setEngineKind (Kind::APrime);

    // ... but a preset, "new" or an A/B switch does not change it.
    REQUIRE (p.getPresets().loadByName ("Starter - De-Ess").wasOk());
    CHECK (p.getEngineKind() == Kind::APrime);
    p.getPresets().loadEmpty();
    CHECK (p.getEngineKind() == Kind::APrime);
    p.getPresets().switchTo (1);
    p.setEngineKind (Kind::A3);
    p.getPresets().switchTo (0);
    CHECK (p.getEngineKind() == Kind::A3);

    // Audition and metering follow the selected engine.
    p.setEngineKind (Kind::APrime);
    p.getBandSlots().createBand();
    p.setAudition (dsp::AuditionMode::Solo, 1);
    CHECK (p.getEngine().getAuditionMode() == dsp::AuditionMode::Solo);
    p.setEngineKind (Kind::A3);
    CHECK (p.getEngine().getAuditionMode() == dsp::AuditionMode::Solo);
}
