#include "DspTestSupport.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace tl::test
{
namespace
{
constexpr double pi = 3.14159265358979323846;
}

double dbOf (double v) { return v > 0.0 ? 20.0 * std::log10 (v) : -400.0; }
double magDb (cd h) { return dbOf (std::abs (h)); }
double phaseDeg (cd h) { return std::arg (h) * 180.0 / pi; }
double dbToGain (double db) { return std::pow (10.0, db / 20.0); }

int nextPow2 (int n)
{
    int p = 1;
    while (p < n) p <<= 1;
    return p;
}

std::vector<double> logGrid (double fMin, double fMax, int points)
{
    std::vector<double> f ((size_t) points);
    for (int i = 0; i < points; ++i)
        f[(size_t) i] = fMin * std::pow (fMax / fMin, (double) i / (double) (points - 1));
    return f;
}

void fftInPlace (std::vector<cd>& a)
{
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; ++i)
    {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap (a[i], a[j]);
    }

    for (size_t len = 2; len <= n; len <<= 1)
    {
        const double ang = -2.0 * pi / (double) len;
        for (size_t i = 0; i < n; i += len)
            for (size_t k = 0; k < len / 2; ++k)
            {
                const cd w = std::polar (1.0, ang * (double) k);   // exact twiddles (no recurrence drift)
                const cd u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
            }
    }
}

MeasuredResponse::MeasuredResponse (const std::vector<float>& impulse, double sampleRate, int padFactor)
    : h (impulse), fs (sampleRate)
{
    const int n = nextPow2 ((int) h.size() * padFactor);
    bins.assign ((size_t) n, cd {});
    for (size_t i = 0; i < h.size(); ++i) bins[i] = (double) h[i];
    fftInPlace (bins);
}

cd MeasuredResponse::at (double f) const
{
    const double n = (double) bins.size();
    const double x = std::clamp (f / fs * n, 0.0, n / 2.0 - 1.0);
    const auto i = (size_t) x;
    const double t = x - (double) i;
    return bins[i] * (1.0 - t) + bins[i + 1] * t;
}

cd MeasuredResponse::exact (double f) const
{
    const double w = 2.0 * pi * f / fs;
    cd acc {};
    for (size_t i = 0; i < h.size(); ++i)
        if (h[i] != 0.0f)
            acc += (double) h[i] * std::polar (1.0, -w * (double) i);
    return acc;
}

double MeasuredResponse::crossing (double fA, double fB, double levelDb) const
{
    double a = std::log (fA), b = std::log (fB);
    const double sa = magDb (at (fA)) - levelDb;
    for (int it = 0; it < 60; ++it)
    {
        const double m = 0.5 * (a + b);
        const double sm = magDb (at (std::exp (m))) - levelDb;
        if ((sm < 0.0) == (sa < 0.0)) a = m; else b = m;
    }
    return std::exp (0.5 * (a + b));
}

std::vector<float> engineImpulseResponse (double fs, const std::vector<RegionSpec>& regions, int length)
{
    auto engine = std::make_unique<dsp::A3RegionEngine>();
    engine->prepare (fs, 512);

    dsp::EngineSettings s;
    std::vector<float> gains (regions.size());
    dsp::ExternalGains ext;
    ext.length = 1;

    for (size_t i = 0; i < regions.size(); ++i)
    {
        const int slot = regions[i].slot > 0 ? regions[i].slot : (int) i + 1;
        auto& b = s.bands[(size_t) slot - 1];
        b.fLoHz = (float) regions[i].fLo;
        b.fHiHz = (float) regions[i].fHi;
        b.thresholdDb = 12.0f;
        s.usedMask |= 1u << (slot - 1);
        gains[i] = (float) dbToGain (regions[i].gainDb);
        ext.data[slot - 1] = &gains[i];
    }

    engine->setSettings (s);
    engine->setExternalGains (&ext);

    std::vector<float> x ((size_t) length, 0.0f);
    x[0] = 1.0f;
    float* chans[1] { x.data() };
    engine->process (chans, 1, length);
    return x;
}

cd a3Model (double f, double fs, const std::vector<RegionSpec>& regions, bool analog)
{
    auto w = [&] (double fe) { return analog ? 2.0 * pi * fe : std::tan (pi * fe / fs); };
    const cd s = analog ? cd (0.0, 2.0 * pi * f) : cd (0.0, std::tan (pi * f / fs));
    cd H = 1.0;
    for (auto& r : regions)
    {
        const double wl = w (r.fLo), wh = w (r.fHi);
        const double g = std::sqrt (wl * wh), R = (wh - wl) / (2.0 * g);
        const cd sp = s / g;
        const cd D = sp * sp + 2.0 * R * sp + 1.0;
        H *= 1.0 + 2.0 * R * (dbToGain (r.gainDb) - 1.0) * sp / D;
    }
    return H;
}

double digitalCentre (double fLo, double fHi, double fs)
{
    const double g = std::sqrt (std::tan (pi * fLo / fs) * std::tan (pi * fHi / fs));
    return fs / pi * std::atan (g);
}

double detectorGain (double f, double fLo, double fHi, double fs)
{
    const cd s (0.0, std::tan (pi * f / fs));
    const double r2 = std::sqrt (2.0);
    const cd sl = s / std::tan (pi * fLo / fs), sh = s / std::tan (pi * fHi / fs);
    const cd hp = sl * sl / (sl * sl + r2 * sl + 1.0);
    const cd lp = 1.0 / (sh * sh + r2 * sh + 1.0);
    return std::abs (hp * lp);
}

EngineRig::EngineRig (double sampleRate) : fs (sampleRate)
{
    engine.prepare (fs, 512);
}

void EngineRig::use (int slot, double fLo, double fHi)
{
    settings.usedMask |= 1u << (slot - 1);
    band (slot).fLoHz = (float) fLo;
    band (slot).fHiHz = (float) fHi;
}

void EngineRig::run (std::vector<std::vector<float>>& channels, int blockSize)
{
    const int n = (int) channels[0].size();
    const int nCh = (int) channels.size();
    for (int pos = 0; pos < n; pos += blockSize)
    {
        const int len = std::min (blockSize, n - pos);
        float* ptr[dsp::kMaxChannels] {};
        for (int c = 0; c < nCh; ++c) ptr[c] = channels[(size_t) c].data() + pos;
        engine.setSettings (settings);
        engine.process (ptr, nCh, len);
    }
}

std::vector<float> sine (double f, double fs, int n, double amplitude, double phase)
{
    std::vector<float> x ((size_t) n);
    for (int i = 0; i < n; ++i)
        x[(size_t) i] = (float) (amplitude * std::sin (2.0 * pi * f * (double) i / fs + phase));
    return x;
}

void setPlain (PluginProcessor& p, const std::string& id, float plain)
{
    auto* prm = p.getState().getParameter (id);
    jassert (prm != nullptr);
    prm->setValueNotifyingHost (prm->convertTo0to1 (plain));
}

void setBand (PluginProcessor& p, int slot, const std::string& suffix, float plain)
{
    setPlain (p, schema::bandParamId (slot, suffix), plain);
}

namespace measure
{
bool enabled()
{
    const char* d = std::getenv ("TL_MEASURE_DIR");
    return d != nullptr && *d != 0;
}

static std::string pathFor (const std::string& name)
{
    std::filesystem::path dir (std::getenv ("TL_MEASURE_DIR"));
    std::filesystem::create_directories (dir);
    return (dir / name).string();
}

void writeCsv (const std::string& name, const std::vector<std::string>& header, const std::vector<std::vector<double>>& rows)
{
    if (! enabled()) return;
    std::ofstream o (pathFor (name));
    for (size_t i = 0; i < header.size(); ++i) o << (i ? "," : "") << header[i];
    o << '\n';
    char buf[64];
    for (auto& r : rows)
    {
        for (size_t i = 0; i < r.size(); ++i)
        {
            std::snprintf (buf, sizeof buf, "%.9g", r[i]);
            o << (i ? "," : "") << buf;
        }
        o << '\n';
    }
}

void writeText (const std::string& name, const std::string& text)
{
    if (! enabled()) return;
    std::ofstream (pathFor (name)) << text;
}
} // namespace measure
} // namespace tl::test
