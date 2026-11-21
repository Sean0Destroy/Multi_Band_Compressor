#include "SpectrumAnalyzer.h"

#include <algorithm>
#include <cmath>

namespace tl::analysis
{
namespace { constexpr double kPi = 3.14159265358979323846; }

Fft::Fft (int order) : n (1 << order)
{
    twiddles.resize ((size_t) n / 2);
    for (int k = 0; k < n / 2; ++k)
    {
        const double a = -2.0 * kPi * (double) k / (double) n;
        twiddles[(size_t) k] = { (float) std::cos (a), (float) std::sin (a) };
    }

    reversed.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
    {
        int r = 0;
        for (int b = 0; b < order; ++b)
            if ((i >> b) & 1) r |= 1 << (order - 1 - b);
        reversed[(size_t) i] = r;
    }
}

void Fft::forward (std::vector<std::complex<float>>& a) const
{
    for (int i = 0; i < n; ++i)
        if (i < reversed[(size_t) i])
            std::swap (a[(size_t) i], a[(size_t) reversed[(size_t) i]]);

    for (int len = 2; len <= n; len <<= 1)
    {
        const int half = len / 2, step = n / len;
        for (int start = 0; start < n; start += len)
            for (int k = 0; k < half; ++k)
            {
                const auto t = a[(size_t) (start + k + half)] * twiddles[(size_t) (k * step)];
                const auto u = a[(size_t) (start + k)];
                a[(size_t) (start + k)] = u + t;
                a[(size_t) (start + k + half)] = u - t;
            }
    }
}

SpectrumAnalyzer::SpectrumAnalyzer() : fft (order)
{
    const int n = fft.size();
    window.resize ((size_t) n);
    for (int i = 0; i < n; ++i)
        window[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * kPi * (double) i / (double) n));   // periodic Hann

    ring.assign ((size_t) n, 0.0f);
    work.resize ((size_t) n);
    levels.assign ((size_t) n / 2 + 1, floorDb);
    scratch.resize (2048);
}

void SpectrumAnalyzer::reset()
{
    std::fill (ring.begin(), ring.end(), 0.0f);
    std::fill (levels.begin(), levels.end(), floorDb);
    writeIndex = 0;
}

void SpectrumAnalyzer::push (const float* samples, int numSamples)
{
    const int n = fft.size();
    for (int i = 0; i < numSamples; ++i)
    {
        ring[(size_t) writeIndex] = samples[i];
        writeIndex = (writeIndex + 1) % n;
    }
}

int SpectrumAnalyzer::pull (rt::AnalyzerFifo& fifo)
{
    int total = 0;
    for (;;)
    {
        const int got = fifo.pull (scratch.data(), (int) scratch.size());
        if (got <= 0) break;
        push (scratch.data(), got);
        total += got;
    }
    return total;
}

void SpectrumAnalyzer::compute (float fallDb)
{
    const int n = fft.size();
    for (int i = 0; i < n; ++i)
        work[(size_t) i] = { ring[(size_t) ((writeIndex + i) % n)] * window[(size_t) i], 0.0f };   // oldest first

    fft.forward (work);

    // Amplitude calibration: Hann coherent gain 0.5, single-sided spectrum.
    const float scale = 2.0f / ((float) n * 0.5f);
    for (size_t k = 0; k < levels.size(); ++k)
    {
        const float mag = std::abs (work[k]) * scale;
        const float db = mag > 1.0e-6f ? 20.0f * std::log10 (mag) : floorDb;
        levels[k] = std::max (std::max (db, floorDb), levels[k] - fallDb);
    }
}

float SpectrumAnalyzer::averageLevelDb (double f0, double f1) const noexcept
{
    if (f1 < f0) std::swap (f0, f1);
    const double binWidth = fs / (double) fft.size();
    const int last = (int) levels.size() - 1;
    const int b0 = std::max (0, (int) std::ceil (f0 / binWidth)), b1 = std::min (last, (int) std::floor (f1 / binWidth));

    if (b1 <= b0)
        return levelDb (f0, f1);

    double power = 0.0;
    for (int b = b0; b <= b1; ++b)
        power += std::pow (10.0, (double) levels[(size_t) b] / 10.0);
    return (float) std::max ((double) floorDb, 10.0 * std::log10 (power / (double) (b1 - b0 + 1)));
}

float SpectrumAnalyzer::levelDb (double f0, double f1) const noexcept
{
    if (f1 < f0) std::swap (f0, f1);
    const double binWidth = fs / (double) fft.size();
    const double p0 = f0 / binWidth, p1 = f1 / binWidth;
    const int last = (int) levels.size() - 1;

    const int b0 = (int) std::ceil (p0), b1 = (int) std::floor (p1);
    if (b1 >= b0)
    {
        float m = floorDb;
        for (int b = std::max (0, b0); b <= std::min (last, b1); ++b)
            m = std::max (m, levels[(size_t) b]);
        return m;
    }

    // Narrower than a bin: interpolate at the centre.
    const double p = std::clamp (0.5 * (p0 + p1), 0.0, (double) last);
    const int i = std::min (last - 1, (int) p);
    const float t = (float) (p - (double) i);
    return levels[(size_t) i] + t * (levels[(size_t) i + 1] - levels[(size_t) i]);
}
} // namespace tl::analysis
