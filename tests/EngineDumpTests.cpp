#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <fstream>
#include <limits>

#include "PluginProcessor.h"
#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"

// Hidden refactoring aid (not run by ctest): renders fixed scenarios and writes the raw float32
// output to TL_DUMP_DIR. Run it before and after an engine refactor WITH THE SAME COMPILER AND
// FLAGS and compare the files byte for byte:
//   TL_DUMP_DIR=/tmp/before tl_tests "[.dump]"     (old code)
//   TL_DUMP_DIR=/tmp/after  tl_tests "[.dump]"     (new code)
//   cmp -s /tmp/before/x.f32 /tmp/after/x.f32
// The files are not portable across compilers or CPUs (FMA contraction, libm), which is why this
// is a tool and not a golden-file test.
using namespace tl;

namespace
{
void dump (const std::string& dir, const std::string& name, const juce::AudioBuffer<float>& b)
{
    std::ofstream out (dir + "/" + name + ".f32", std::ios::binary | std::ios::trunc);
    REQUIRE (out.good());
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
        out.write (reinterpret_cast<const char*> (b.getReadPointer (ch)), (std::streamsize) (sizeof (float) * (size_t) b.getNumSamples()));
    REQUIRE (out.good());
}

void dump (const std::string& dir, const std::string& name, const std::vector<std::vector<float>>& chans)
{
    std::ofstream out (dir + "/" + name + ".f32", std::ios::binary | std::ios::trunc);
    REQUIRE (out.good());
    for (auto& c : chans)
        out.write (reinterpret_cast<const char*> (c.data()), (std::streamsize) (sizeof (float) * c.size()));
    REQUIRE (out.good());
}

juce::AudioBuffer<float> programme (int channels, int n, double fs, std::uint32_t seed)
{
    // Noise with level steps plus low and high tones: exercises attack, release and every band.
    auto b = test::makeNoise (channels, n, seed, 0.5f);
    for (int ch = 0; ch < channels; ++ch)
    {
        auto* x = b.getWritePointer (ch);
        for (int i = 0; i < n; ++i)
        {
            const double t = (double) i / fs;
            const double env = 0.15 + 0.85 * (std::fmod (t, 0.5) < 0.2 ? 1.0 : 0.25);
            const double tones = 0.4 * std::sin (2.0 * dsp::kPi * 70.0 * t + ch) + 0.2 * std::sin (2.0 * dsp::kPi * 6300.0 * t);
            x[i] = (float) (env * ((double) x[i] + tones)) * (ch == 1 ? 0.6f : 1.0f);
        }
    }
    return b;
}
}

TEST_CASE ("Engine output dump for refactor verification", "[.dump]")
{
    const auto dirJ = juce::SystemStats::getEnvironmentVariable ("TL_DUMP_DIR", {});
    if (dirJ.isEmpty()) SKIP ("TL_DUMP_DIR is not set");
    const std::string dir = dirJ.toStdString();
    juce::File (dirJ).createDirectory();

    // 1. Four bands, stereo, 48 kHz, irregular blocks.
    {
        PluginProcessor p;
        for (int i = 0; i < 4; ++i) p.getBandSlots().createBand();
        test::setBand (p, 1, "thresh", -36.0f); test::setBand (p, 1, "ratio", 4.0f);
        test::setBand (p, 2, "thresh", -30.0f); test::setBand (p, 2, "attack", 0.5f); test::setBand (p, 2, "release", 40.0f);
        test::setBand (p, 3, "thresh", -42.0f); test::setBand (p, 3, "ratio", 100.0f); test::setBand (p, 3, "knee", 0.0f);
        test::setBand (p, 4, "thresh", -28.0f); test::setBand (p, 4, "range", 6.0f);
        test::OfflineRenderer r (p);
        REQUIRE (r.prepare (48000.0, 512, {}));
        dump (dir, "s1_four_bands", r.render (programme (2, 96000, 48000.0, 11u), nullptr, { 64, 480, 1, 512, 33 }));
    }

    // 2. Eight bands, unlinked, mixed detectors, 44.1 kHz.
    {
        PluginProcessor p;
        for (int i = 0; i < 8; ++i) p.getBandSlots().createBand();
        for (int s = 1; s <= 8; ++s)
        {
            test::setBand (p, s, "thresh", -44.0f + 2.0f * (float) s);
            test::setBand (p, s, "ratio", 1.5f + (float) s);
            test::setBand (p, s, "link", (float) (s % 3) * 50.0f);
            test::setBand (p, s, "detMode", (float) (s % 3));
            test::setBand (p, s, "detBlend", 0.1f * (float) s);
            test::setBand (p, s, "rmsTime", 3.0f * (float) s);
            test::setBand (p, s, "knee", 3.0f * (float) (s % 5));
        }
        test::OfflineRenderer r (p);
        REQUIRE (r.prepare (44100.0, 256, {}));
        dump (dir, "s2_eight_bands", r.render (programme (2, 88200, 44100.0, 12u), nullptr, { 256 }));
    }

    // 3. Mono, 96 kHz: edge automation, overlapping/reversed edges, bypasses, create/delete, host bypass.
    {
        PluginProcessor p;
        for (int i = 0; i < 3; ++i) p.getBandSlots().createBand();
        for (int s = 1; s <= 3; ++s) { test::setBand (p, s, "thresh", -40.0f); test::setBand (p, s, "ratio", 6.0f); }
        test::OfflineRenderer r (p);
        test::Layout mono; mono.mainChannels = 1;
        REQUIRE (r.prepare (96000.0, 1024, mono));
        const auto in = programme (1, 192000, 96000.0, 13u);

        const auto hook = [] (int block, PluginProcessor& proc)
        {
            test::setBand (proc, 1, "fLo", 40.0f + 15.0f * (float) (block % 40));
            test::setBand (proc, 1, "fHi", 3000.0f - 60.0f * (float) (block % 40));     // crosses fLo
            test::setBand (proc, 2, "fHi", 400.0f * (1.0f + 0.05f * (float) (block % 25)));
            test::setBand (proc, 2, "bypass", (block / 20) % 2 == 1 ? 1.0f : 0.0f);
            test::setPlain (proc, "bypass", (block / 55) % 3 == 2 ? 1.0f : 0.0f);
            if (block == 60) proc.getBandSlots().deleteBand (3);
            if (block == 61) proc.getBandSlots().createBand();
            if (block == 90) proc.getBandSlots().deleteBand (1);
            if (block == 140) proc.getBandSlots().createBand();
            test::setBand (proc, 3, "attack", 0.01f + (float) (block % 7));
            test::setBand (proc, 3, "release", 5.0f + 30.0f * (float) (block % 11));
        };
        auto out = r.render (in, nullptr, { 1024, 7, 300 }, false, hook);
        dump (dir, "s3_automation", out);
        dump (dir, "s3_host_bypass", r.render (in, nullptr, { 128 }, true));
        dump (dir, "s3_after_bypass", r.render (in, nullptr, { 128 }, false));
    }

    // 4. Non-finite input and extreme levels through the processor guards.
    {
        PluginProcessor p;
        for (int i = 0; i < 2; ++i) p.getBandSlots().createBand();
        test::setBand (p, 1, "thresh", -60.0f);
        test::OfflineRenderer r (p);
        REQUIRE (r.prepare (48000.0, 256, {}));
        auto in = programme (2, 24000, 48000.0, 14u);
        in.setSample (0, 5000, std::numeric_limits<float>::quiet_NaN());
        in.setSample (1, 9000, std::numeric_limits<float>::infinity());
        for (int i = 12000; i < 12100; ++i) in.setSample (0, i, 1.0e6f);
        dump (dir, "s4_nonfinite", r.render (in, nullptr, { 256, 31 }));
    }

    // 5. Engine direct: external gain trajectories and the gain trace.
    {
        test::EngineRig rig (48000.0);
        rig.use (1, 100.0, 800.0);
        rig.use (2, 800.0, 6000.0);
        rig.use (5, 300.0, 3000.0);
        rig.band (5).thresholdDb = -35.0f;
        rig.band (5).ratio = 5.0f;

        const int n = 48000;
        std::vector<float> g1 ((size_t) n), g2 ((size_t) n);
        for (int i = 0; i < n; ++i)
        {
            g1[(size_t) i] = 0.25f + 0.75f * (float) (0.5 + 0.5 * std::sin (2.0 * dsp::kPi * 3.0 * i / 48000.0));
            g2[(size_t) i] = (i / 4000) % 2 == 0 ? 1.0f : 0.1f;
        }
        dsp::ExternalGains ext;
        ext.data[0] = g1.data();
        ext.data[1] = g2.data();
        ext.length = n;
        rig.engine.setExternalGains (&ext);

        std::vector<float> trace ((size_t) n, -1.0f), trace2 ((size_t) n, -1.0f);
        dsp::GainTrace tr;
        tr.data[4][0] = trace.data();
        tr.data[4][1] = trace2.data();
        tr.capacity = n;
        rig.engine.setGainTrace (&tr);

        const auto src = programme (2, n, 48000.0, 15u);
        std::vector<std::vector<float>> chans (2, std::vector<float> ((size_t) n));
        for (int ch = 0; ch < 2; ++ch)
            std::copy (src.getReadPointer (ch), src.getReadPointer (ch) + n, chans[(size_t) ch].begin());
        rig.run (chans, 100);
        rig.engine.setExternalGains (nullptr);
        rig.engine.setGainTrace (nullptr);

        dump (dir, "s5_external_gains", chans);
        dump (dir, "s5_gain_trace", std::vector<std::vector<float>> { trace, trace2 });
    }
}
