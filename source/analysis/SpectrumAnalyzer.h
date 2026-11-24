#pragma once

#include <complex>
#include <vector>

#include "rt/AnalyzerFifo.h"

// Editor-side spectrum analysis (message thread only). The audio thread only feeds the FIFO.
namespace tl::analysis
{
/** In-place iterative radix-2 FFT with precomputed twiddles (size = 2^order). */
class Fft
{
public:
    explicit Fft (int order);
    int size() const noexcept { return n; }
    void forward (std::vector<std::complex<float>>& data) const;   // data.size() == size()

private:
    int n = 0;
    std::vector<std::complex<float>> twiddles;
    std::vector<int> reversed;
};

/** Sliding Hann-windowed magnitude spectrum of a mono stream, in dBFS (a full-scale sine reads
    0 dB), with per-bin ballistics: instant rise, exponential fall. */
class SpectrumAnalyzer
{
public:
    static constexpr int order = 12;             // 4096-point FFT
    static constexpr float floorDb = -120.0f;

    SpectrumAnalyzer();

    void setSampleRate (double sampleRate) noexcept { fs = sampleRate > 0.0 ? sampleRate : 48000.0; }
    double sampleRate() const noexcept { return fs; }

    /** Drains the FIFO into the sliding window. Returns the number of samples taken. */
    int pull (rt::AnalyzerFifo&);
    /** Appends samples directly (tests, offline use). */
    void push (const float* samples, int numSamples);

    /** Recomputes the spectrum from the current window. `fallDb` is how far a bin may fall
        since the previous call (ballistics); pass a large value for no smoothing. */
    void compute (float fallDb = 3.0f);

    /** Clears the window and the display values. */
    void reset();

    int numBins() const noexcept { return (int) levels.size(); }
    float binLevelDb (int bin) const noexcept { return levels[(size_t) bin]; }
    double binFrequency (int bin) const noexcept { return fs * (double) bin / (double) fft.size(); }

    /** Level for a frequency range [f0, f1] (Hz): the highest bin inside it, or the interpolated
        value when the range is narrower than a bin. */
    float levelDb (double f0, double f1) const noexcept;

    /** Power average over [f0, f1] in dB (smoother than levelDb for noise-like material); falls
        back to the interpolated value when the range is narrower than a bin. */
    float averageLevelDb (double f0, double f1) const noexcept;

private:
    Fft fft;
    double fs = 48000.0;
    std::vector<float> window, ring, levels;
    std::vector<std::complex<float>> work;
    int writeIndex = 0;
    std::vector<float> scratch;
};
} // namespace tl::analysis
