// Linear-phase mode: the FIR stage on its own (layout, exact identity, magnitude against the band
// models, symmetry, mid/side, gain changes without allocation) and the plugin with Quality set to
// Linear Phase (latency, alignment of the dry path, click-free switching).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <complex>
#include <memory>
#include <vector>

#include "dsp/LinearPhase.h"
#include "dsp/RealFft.h"
#include "support/AllocationCounter.h"
#include "support/DspTestSupport.h"
#include "support/OfflineRenderer.h"

using namespace tl;
using namespace tl::test;
using Kind = dsp::EngineKind;

namespace
{
constexpr double kFs = 48000.0;

dsp::LinearPhaseSnapshot::Band band (double fLo, double fHi, float g0, float g1 = -1.0f, bool midSide = false)
{
    dsp::LinearPhaseSnapshot::Band b;
    b.active = true;
    b.midSide = midSide;
    b.lo = (float) std::log2 (fLo);
    b.hi = (float) std::log2 (fHi);
    b.gain[0] = g0;
    b.gain[1] = g1 < 0.0f ? g0 : g1;
    return b;
}

/** Runs the stage over `in` (lanes x n) in blocks of `block`, giving it `snap` for every hop. */
std::vector<std::vector<float>> runStage (dsp::LinearPhaseStage& st, const std::vector<std::vector<float>>& in,
                                          const dsp::LinearPhaseSnapshot& snap, int block,
                                          std::vector<std::vector<float>>* delayed = nullptr)
{
    const int nCh = (int) in.size(), n = (int) in[0].size();
    std::vector<std::vector<float>> out ((size_t) nCh, std::vector<float> ((size_t) n));
    if (delayed != nullptr) delayed->assign ((size_t) nCh, std::vector<float> ((size_t) n));
    for (int pos = 0; pos < n; pos += block)
    {
        const int len = std::min (block, n - pos);
        if (st.sampleWanted (len) >= 0)
            st.pendingSnapshot() = snap;
        const float* ip[2] { in[0].data() + pos, in[(size_t) (nCh - 1)].data() + pos };
        float* op[2] { out[0].data() + pos, out[(size_t) (nCh - 1)].data() + pos };
        float* dp[2] {};
        if (delayed != nullptr) { dp[0] = (*delayed)[0].data() + pos; dp[1] = (*delayed)[(size_t) (nCh - 1)].data() + pos; }
        st.process (ip, op, delayed != nullptr ? dp : nullptr, nCh, len);
    }
    return out;
}

std::vector<double> spectrumOf (const std::vector<float>& h, int start, int length)
{
    std::vector<cd> a ((size_t) length);
    for (int i = 0; i < length; ++i)
        a[(size_t) i] = cd ((double) h[(size_t) (start + i)], 0.0);
    fftInPlace (a);
    std::vector<double> m ((size_t) length / 2 + 1);
    for (size_t k = 0; k < m.size(); ++k)
        m[k] = std::abs (a[k]);
    return m;
}
} // namespace

TEST_CASE ("RealFft matches a direct DFT and inverts", "[lp]")
{
    dsp::RealFft fft (4096);
    for (int n : { 4, 16, 256, 4096 })
    {
        std::vector<float> x ((size_t) n + 2), y;
        for (int i = 0; i < n; ++i)
            x[(size_t) i] = (float) std::sin (0.37 * i) + 0.25f * (float) ((i * 7919) % 13) - 1.0f;
        y = x;
        fft.setSize (n);
        fft.forward (y.data());
        double err = 0.0;
        for (int k = 0; k <= n / 2; k += std::max (1, n / 64))
        {
            std::complex<double> s {};
            for (int i = 0; i < n; ++i)
                s += (double) x[(size_t) i] * std::polar (1.0, -2.0 * dsp::kPi * k * i / n);
            err = std::max (err, std::abs (s - std::complex<double> (y[(size_t) (2 * k)], y[(size_t) (2 * k + 1)])));
        }
        CHECK (err < 1.0e-5 * n);
        fft.inverse (y.data());
        for (int i = 0; i < n; ++i)
            REQUIRE (std::abs (y[(size_t) i] - x[(size_t) i]) < 1.0e-5f);
    }
}

TEST_CASE ("Linear-phase layout: 43 / 85 / 171 ms with a 5.3 ms hop at 48 kHz", "[lp]")
{
    const int taps[] { 4096, 8192, 16384 };
    for (int i = 0; i < 3; ++i)
    {
        const auto l = dsp::linearPhaseLayout (48000.0, i);
        CHECK (l.taps == taps[i]);
        CHECK (l.hop == 256);
        CHECK (l.latency() == taps[i] / 2);
    }
    // Other rates keep about the same times; the hop stays a power of two.
    const auto l441 = dsp::linearPhaseLayout (44100.0, 1);
    CHECK (l441.taps == 8192);
    CHECK (l441.hop == 256);
    const auto l96 = dsp::linearPhaseLayout (96000.0, 1);
    CHECK (l96.taps == 16384);
    CHECK (l96.hop == 512);
    const auto l192 = dsp::linearPhaseLayout (192000.0, 2);
    CHECK (l192.taps == 32768);   // capped: 171 ms would need 65536
    for (double fs : { 22050.0, 32000.0, 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0, 384000.0 })
        for (int c = 0; c < 3; ++c)
        {
            const auto l = dsp::linearPhaseLayout (fs, c);
            CHECK (l.taps / l.hop <= dsp::kLinearPhaseMaxPartitions);
            CHECK (l.centre() >= l.hop);
        }
}

TEST_CASE ("Linear-phase stage: all bands at unity is the input delayed by exactly half the kernel", "[lp]")
{
    dsp::LinearPhaseStage st;
    st.prepare (kFs, 0, Kind::A3);
    const int D = st.latency();
    REQUIRE (D == 2048);

    dsp::LinearPhaseSnapshot snap;
    snap.numChannels = 2;
    snap.bands[0] = band (100.0, 1000.0, 1.0f);   // active, unity

    const int n = 4 * D + 1000;
    std::vector<std::vector<float>> in (2, std::vector<float> ((size_t) n));
    for (int i = 0; i < n; ++i)
    {
        in[0][(size_t) i] = (float) std::sin (0.01 * i) * 0.5f;
        in[1][(size_t) i] = (float) (((unsigned) i * 2654435761u) % 1000u) / 1000.0f - 0.5f;
    }
    std::vector<std::vector<float>> delayed;
    const auto out = runStage (st, in, snap, 100, &delayed);
    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
        {
            const float want = i >= D ? in[(size_t) c][(size_t) (i - D)] : 0.0f;
            REQUIRE (out[(size_t) c][(size_t) i] == want);
            REQUIRE (delayed[(size_t) c][(size_t) i] == want);
        }
    CHECK (st.designCount() == 0);
}

TEST_CASE ("Linear-phase stage: impulse response is symmetric and has the band model's magnitude", "[lp]")
{
    for (Kind kind : { Kind::A3, Kind::APrime })
        for (int length : { 0, 1, 2 })
        {
            dsp::LinearPhaseStage st;
            st.prepare (kFs, length, kind);
            const int D = st.latency(), L = st.layout().taps;

            dsp::LinearPhaseSnapshot snap;
            snap.numChannels = 1;
            snap.bands[0] = band (80.0, 400.0, (float) dbToGain (-12.0));
            snap.bands[2] = band (2000.0, 6000.0, (float) dbToGain (6.0));
            snap.bands[5] = band (500.0, 900.0, (float) dbToGain (-4.0));

            // Settle the kernel (snapshots arrive after the latency), then an impulse.
            const int lead = 2 * D + st.layout().hop;
            const int n = lead + 2 * L;
            std::vector<std::vector<float>> in (1, std::vector<float> ((size_t) n, 0.0f));
            in[0][(size_t) lead] = 1.0f;
            const auto out = runStage (st, in, snap, 256);
            const auto& h = out[0];

            // Linear phase: symmetric about the impulse + D.
            const int centre = lead + D;
            double asym = 0.0, peak = 0.0;
            for (int m = 1; m < D; ++m)
            {
                asym = std::max (asym, (double) std::abs (h[(size_t) (centre + m)] - h[(size_t) (centre - m)]));
                peak = std::max (peak, (double) std::abs (h[(size_t) (centre + m)]));
            }
            CHECK (asym < 1.0e-6 + 1.0e-5 * peak);

            // Magnitude against the model, on a log grid above where the length resolves.
            const auto mag = spectrumOf (h, centre - L / 2, L);
            const double fMin = length == 0 ? 120.0 : 60.0;
            double worst = 0.0;
            for (double f : logGrid (fMin, 18000.0, 200))
            {
                const int k = (int) std::lround (f * L / kFs);
                const double fk = k * kFs / L;
                const double want = std::abs (dsp::LinearPhaseStage::modelMatrixEntry (kind, snap, fk, kFs, 0, 0));
                worst = std::max (worst, std::abs (dbOf (mag[(size_t) k]) - dbOf (want)));
            }
            INFO ("kind " << (int) kind << " length " << length << " worst dB error " << worst);
            CHECK (worst < (length == 0 ? 0.4 : 0.1));
        }
}

TEST_CASE ("Linear-phase stage: mid/side bands act on mid and side only", "[lp]")
{
  for (bool mixed : { true, false })   // with an L/R band (full matrix), or mid/side bands only
  {
    INFO ("mixed " << mixed);
    dsp::LinearPhaseStage st;
    st.prepare (kFs, 1, Kind::APrime);
    const int D = st.latency(), L = st.layout().taps;

    dsp::LinearPhaseSnapshot snap;
    snap.numChannels = 2;
    snap.bands[0] = band (200.0, 2000.0, (float) dbToGain (-10.0), 1.0f, true);   // mid only
    snap.bands[1] = band (3000.0, 8000.0, (float) dbToGain (-3.0), (float) dbToGain (-6.0), ! mixed);
    snap.bands[3] = band (90.0, 180.0, (float) dbToGain (4.0));                    // same on both lanes

    const int lead = 2 * D + st.layout().hop;
    const int n = lead + 2 * L;
    std::vector<std::vector<float>> in (2, std::vector<float> ((size_t) n, 0.0f));
    in[0][(size_t) lead] = 1.0f;   // impulse on the left only
    const auto out = runStage (st, in, snap, 333);

    const int centre = lead + D;
    const auto ll = spectrumOf (out[0], centre - L / 2, L);
    const auto rl = spectrumOf (out[1], centre - L / 2, L);
    double worst = 0.0;
    for (double f : logGrid (80.0, 16000.0, 120))
    {
        const int k = (int) std::lround (f * L / kFs);
        const double fk = k * kFs / L;
        const double wantLL = std::abs (dsp::LinearPhaseStage::modelMatrixEntry (Kind::APrime, snap, fk, kFs, 0, 0));
        const double wantRL = std::abs (dsp::LinearPhaseStage::modelMatrixEntry (Kind::APrime, snap, fk, kFs, 1, 0));
        // Relative to the left output's level (the cross term is a difference of two gains).
        worst = std::max (worst, std::abs (ll[(size_t) k] - wantLL) / wantLL);
        worst = std::max (worst, std::abs (rl[(size_t) k] - wantRL) / wantLL);
    }
    CHECK (worst < 0.01);   // about 0.09 dB (the 90 to 180 Hz band is the hardest at Medium)

    // A side-only signal (L = -R) is not touched by the mid-only band at all.
    dsp::LinearPhaseSnapshot midOnly;
    midOnly.numChannels = 2;
    midOnly.bands[0] = snap.bands[0];
    dsp::LinearPhaseStage st2;
    st2.prepare (kFs, 1, Kind::APrime);
    std::vector<std::vector<float>> side (2, std::vector<float> ((size_t) n, 0.0f));
    for (int i = 0; i < n; ++i)
    {
        const float s = (float) std::sin (0.07 * i) * 0.3f;
        side[0][(size_t) i] = s;
        side[1][(size_t) i] = -s;
    }
    const auto so = runStage (st2, side, midOnly, 256);
    double dev = 0.0;
    for (int i = lead + 2 * D; i < n; ++i)
        dev = std::max (dev, (double) std::abs (so[0][(size_t) i] - side[0][(size_t) (i - D)]));
    CHECK (dev < 2.0e-6);
  }
}

TEST_CASE ("Linear-phase stage: gain changes are smooth and allocation-free", "[lp][alloc]")
{
    dsp::LinearPhaseStage st;
    st.prepare (kFs, 1, Kind::A3);
    const int D = st.latency();

    const int n = 40 * 256 + 4 * D;
    std::vector<float> x ((size_t) n), y ((size_t) n);
    for (int i = 0; i < n; ++i)
        x[(size_t) i] = 0.5f * (float) std::sin (2.0 * dsp::kPi * 300.0 * i / kFs);

    resetAllocationCount();
    {
        AudioCallbackScope scope;
        dsp::LinearPhaseSnapshot snap;
        snap.numChannels = 1;
        for (int pos = 0; pos < n; pos += 128)
        {
            // A gain that sweeps -20 dB .. 0 dB and back every 100 ms.
            const double t = pos / kFs;
            const float g = (float) dbToGain (-10.0 + 10.0 * std::sin (2.0 * dsp::kPi * 10.0 * t));
            snap.bands[0] = band (150.0, 600.0, g);
            if (st.sampleWanted (128) >= 0)
                st.pendingSnapshot() = snap;
            const float* ip[1] { x.data() + pos };
            float* op[1] { y.data() + pos };
            st.process (ip, op, nullptr, 1, 128);
        }
    }
    CHECK (allocationCount() == 0);
    CHECK (st.designCount() > 30);

    // No steps: the sample-to-sample change stays within what a 300 Hz sine at 0.5 can do.
    const double maxStep = 0.5 * 2.0 * dsp::kPi * 300.0 / kFs;
    double worst = 0.0;
    for (int i = 2 * D; i < n; ++i)
        worst = std::max (worst, (double) std::abs (y[(size_t) i] - y[(size_t) (i - 1)]));
    CHECK (worst < 1.02 * maxStep);
}

// ---- The plugin with Quality = Linear Phase --------------------------------------------------
namespace
{
struct Rig
{
    std::unique_ptr<PluginProcessor> proc = std::make_unique<PluginProcessor>();
    OfflineRenderer renderer { *proc };
    int channels;

    explicit Rig (int numChannels = 2, Kind kind = Kind::A3) : channels (numChannels)
    {
        proc->getBandSlots().createBand();
        proc->setEngineKind (kind);
        setBand (*proc, 1, "fLo", 100.0f);
        setBand (*proc, 1, "fHi", 400.0f);
        setBand (*proc, 1, "thresh", 0.0f);         // no compression unless a test asks
        restart();
    }
    void global (const std::string& id, float v) { setPlain (*proc, id, v); }
    void restart() { REQUIRE (renderer.prepare (kFs, 512, { channels, 0 })); }
    juce::AudioBuffer<float> render (const juce::AudioBuffer<float>& in, std::vector<int> blocks = { 128 },
                                     bool bypassed = false, const OfflineRenderer::BlockHook& hook = {})
    {
        return renderer.render (in, nullptr, blocks, bypassed, hook);
    }
};

juce::AudioBuffer<float> tone (int channels, int n, double f, double a)
{
    juce::AudioBuffer<float> b (channels, n);
    const auto s = sine (f, kFs, n, a);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < n; ++i)
            b.setSample (c, i, s[(size_t) i]);
    return b;
}

float largestStep (const juce::AudioBuffer<float>& b, int start, int end)
{
    float m = 0.0f;
    for (int c = 0; c < b.getNumChannels(); ++c)
        for (int i = std::max (1, start); i < end; ++i)
            m = std::max (m, std::abs (b.getSample (c, i) - b.getSample (c, i - 1)));
    return m;
}
} // namespace

TEST_CASE ("Linear phase: the latency is half the kernel, on top of the lookahead", "[lp][latency]")
{
    {
        Rig rig;
        CHECK (rig.proc->getLatencySamples() == 0);   // Zero Latency is the default
    }
    for (int length = 0; length < 3; ++length)
    {
        Rig rig;
        rig.global ("quality", 1.0f);
        rig.global ("lpLength", (float) length);
        rig.restart();
        const int want = dsp::linearPhaseLayout (kFs, length).latency();
        CHECK (rig.proc->getLatencySamples() == want);

        rig.global ("constantLatency", 1.0f);
        CHECK (rig.proc->getLatencySamples() == want + dsp::lookaheadSamples (10.0, kFs));
        rig.global ("quality", 0.0f);
        CHECK (rig.proc->getLatencySamples() == dsp::lookaheadSamples (10.0, kFs));
    }
}

TEST_CASE ("Linear phase: with nothing to do the output is the input delayed by the latency, dry mix included", "[lp]")
{
    const int n = 30000;
    const auto in = makeNoise (2, n, 11u, 0.1f);
    for (float mix : { 100.0f, 50.0f })
    {
        Rig rig;
        rig.global ("quality", 1.0f);
        rig.global ("mix", mix);
        rig.restart();
        const int D = rig.proc->getLatencySamples();
        REQUIRE (D == 4096);
        const auto out = rig.render (in, { 64, 300, 1, 1024 });
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < n; ++i)
                REQUIRE (out.getSample (c, i) == (i >= D ? in.getSample (c, i - D) : 0.0f));
    }
}

TEST_CASE ("Linear phase: same curve as zero latency, with no phase shift", "[lp]")
{
    for (Kind kind : { Kind::A3, Kind::APrime })
        for (double f : { 60.0, 200.0, 1000.0 })
        {
            INFO ("kind " << (int) kind << " f " << f);
            const int n = 48000;
            const auto in = tone (2, n, f, 0.25);

            Rig zl (2, kind);
            setBand (*zl.proc, 1, "makeup", -9.0f);       // a static band gain
            zl.restart();
            const auto a = zl.render (in);

            Rig lpr (2, kind);
            setBand (*lpr.proc, 1, "makeup", -9.0f);
            lpr.global ("quality", 1.0f);
            lpr.restart();
            const int D = lpr.proc->getLatencySamples();
            const auto b = lpr.render (in);

            // Amplitude of the zero-latency output (it is in phase only at the band edges' symmetry
            // point); the linear-phase output is the input scaled by it, delayed, with no phase shift.
            double peakA = 0.0;
            for (int i = n / 2; i < n; ++i) peakA = std::max (peakA, (double) std::abs (a.getSample (0, i)));
            const double gain = peakA / 0.25;
            double worst = 0.0;
            for (int i = n / 2; i < n; ++i)
                worst = std::max (worst, std::abs ((double) b.getSample (0, i) - gain * (double) in.getSample (0, i - D)));
            CHECK (worst < 0.25 * 0.004);     // within about 0.035 dB and 0.2 degrees
        }
}

TEST_CASE ("Linear phase: solo is the zero-latency band component, delayed with the output", "[lp][audition]")
{
    const int n = 40000;
    const auto in = makeNoise (2, n, 21u, 0.2f);

    Rig zl;
    zl.proc->setAudition (dsp::AuditionMode::Solo, 1);
    const auto a = zl.render (in);

    Rig lpr;
    lpr.global ("quality", 1.0f);
    lpr.restart();
    lpr.proc->setAudition (dsp::AuditionMode::Solo, 1);
    const int D = lpr.proc->getLatencySamples();
    const auto b = lpr.render (in);

    for (int c = 0; c < 2; ++c)
        for (int i = D + 4800; i < n; ++i)
            REQUIRE (b.getSample (c, i) == a.getSample (c, i - D));
}

TEST_CASE ("Linear phase: switching quality, length and engine is click-free and allocation-free", "[lp][alloc]")
{
    const int n = 120000;
    const auto in = tone (2, n, 200.0, 0.5);
    const float sineStep = (float) (0.5 * 2.0 * dsp::kPi * 200.0 / kFs);

    Rig rig;
    setBand (*rig.proc, 1, "makeup", -6.0f);
    setBand (*rig.proc, 1, "thresh", -20.0f);       // compressing as well
    setBand (*rig.proc, 1, "ratio", 4.0f);
    rig.restart();

    resetAllocationCount();
    const auto out = rig.render (in, { 256 }, false, [] (int b, PluginProcessor& p)
    {
        if (b == 40)  setPlain (p, "quality", 1.0f);
        if (b == 120) setPlain (p, "lpLength", 0.0f);
        if (b == 200) setPlain (p, "lpLength", 2.0f);
        if (b == 300) setPlain (p, "quality", 0.0f);
        if (b == 360) setPlain (p, "quality", 1.0f);
        if (b == 361) setPlain (p, "quality", 0.0f);   // changed back before the fade finished
    });
    CHECK (allocationCount() == 0);
    CHECK (largestStep (out, 1, n) < 1.05f * sineStep);

    for (int c = 0; c < 2; ++c)
        for (int i = 0; i < n; ++i)
            REQUIRE (std::isfinite (out.getSample (c, i)));
    // It ends in zero latency, exactly where a run that never left it is (the zero-latency band
    // section kept running all along).
    Rig plain;
    setBand (*plain.proc, 1, "makeup", -6.0f);
    setBand (*plain.proc, 1, "thresh", -20.0f);
    setBand (*plain.proc, 1, "ratio", 4.0f);
    plain.restart();
    const auto ref = plain.render (in, { 256 });
    for (int c = 0; c < 2; ++c)
        for (int i = 370 * 256; i < n; ++i)
            REQUIRE (out.getSample (c, i) == ref.getSample (c, i));
}

TEST_CASE ("Linear phase: switching engines while it runs keeps the latency and stays smooth", "[lp][engine]")
{
    const int n = 96000;
    const auto in = tone (2, n, 250.0, 0.5);
    const float sineStep = (float) (0.5 * 2.0 * dsp::kPi * 250.0 / kFs);

    Rig rig (2, Kind::A3);
    setBand (*rig.proc, 1, "makeup", -6.0f);
    rig.global ("quality", 1.0f);
    rig.restart();
    const int D = rig.proc->getLatencySamples();

    resetAllocationCount();
    const auto out = rig.render (in, { 256 }, false, [] (int b, PluginProcessor& p)
    {
        if (b == 100) p.setEngineKind (Kind::APrime);
    });
    CHECK (allocationCount() == 0);
    CHECK (rig.proc->getLatencySamples() == D);
    CHECK (largestStep (out, D + 1, n) < 1.05f * sineStep);

    // Settled on the new engine: the input scaled, delayed by the same latency, no phase shift.
    double peak = 0.0;
    for (int i = n - 9600; i < n; ++i) peak = std::max (peak, (double) std::abs (out.getSample (0, i)));
    double worst = 0.0;
    for (int i = n - 9600; i < n; ++i)
        worst = std::max (worst, std::abs ((double) out.getSample (0, i) - (peak / 0.5) * (double) in.getSample (0, i - D)));
    CHECK (worst < 0.002);
}

TEST_CASE ("Linear phase: mono, and host bypass keeps the latency and passes the input exactly", "[lp]")
{
    const int n = 30000;
    {
        Rig rig (1);
        rig.global ("quality", 1.0f);
        rig.global ("lpLength", 0.0f);
        rig.restart();
        const int D = rig.proc->getLatencySamples();
        REQUIRE (D == 2048);
        const auto in = makeNoise (1, n, 4u, 0.1f);
        const auto out = rig.render (in, { 100 });
        for (int i = 0; i < n; ++i)
            REQUIRE (out.getSample (0, i) == (i >= D ? in.getSample (0, i - D) : 0.0f));
    }
    {
        Rig rig;
        setBand (*rig.proc, 1, "thresh", -30.0f);    // compressing hard
        setBand (*rig.proc, 1, "makeup", -6.0f);
        rig.global ("quality", 1.0f);
        rig.restart();
        const int D = rig.proc->getLatencySamples();
        const auto in = tone (2, n, 200.0, 0.5);
        const auto out = rig.render (in, { 128 }, true);
        // After the 10 ms ramp to neutral has passed through the latency: exact.
        for (int c = 0; c < 2; ++c)
            for (int i = D + 2 * 480 + 512; i < n; ++i)
                REQUIRE (out.getSample (c, i) == in.getSample (c, i - D));
    }
}
