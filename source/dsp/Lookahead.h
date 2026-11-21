#pragma once

#include <algorithm>
#include <cmath>

#include "IBandEngine.h"

// Per-band lookahead. The audio is delayed by D = the largest lookahead of the used bands (or
// the full 10 ms when "constant latency" is on); band k's gain is delayed by D - L_k, so that it
// leads the audio by exactly its own lookahead L_k. D is the latency the plugin reports.
namespace tl::dsp
{
inline constexpr double kMaxLookaheadMs = 10.0;
inline constexpr int kLookaheadRing = 2048;                   // samples; 10 ms at 192 kHz is 1920
inline constexpr double kLookaheadFadeSeconds = 0.010;        // crossfade when a delay changes

inline int lookaheadSamples (double ms, double sampleRate) noexcept
{
    const double clamped = std::clamp (std::isfinite (ms) ? ms : 0.0, 0.0, kMaxLookaheadMs);
    return (int) std::min<long> (kLookaheadRing - 1, std::lround (clamped * 1.0e-3 * sampleRate));
}

/** Audio delay (= reported latency, in samples) for these settings. Same function on the
    message thread (latency reporting) and on the audio thread (the delay itself). */
inline int lookaheadLatencySamples (const EngineSettings& s, double sampleRate) noexcept
{
    if (s.constantLatency)
        return lookaheadSamples (kMaxLookaheadMs, sampleRate);

    int d = 0;
    for (int i = 0; i < kMaxBands; ++i)
        if (((s.usedMask >> i) & 1u) != 0u)
            d = std::max (d, lookaheadSamples ((double) s.bands[(size_t) i].lookaheadMs, sampleRate));
    return d;
}

/** Ring buffer with two read taps that are crossfaded when the delay changes. With one tap
    (d0 == d1) the output is an exact copy of the input d0 samples ago. */
struct FadingDelayLine
{
    float buffer[kLookaheadRing] {};

    void reset() noexcept { std::fill (std::begin (buffer), std::end (buffer), 0.0f); }

    /** In place over n samples; `w` is the write index of the first sample; t[j] in [0, 1]
        blends from the tap at d0 to the tap at d1 (pass nullptr for d0 only). */
    void process (float* x, int n, int w, int d0, int d1, const float* t) noexcept
    {
        constexpr int mask = kLookaheadRing - 1;
        for (int j = 0; j < n; ++j)
        {
            const int wj = (w + j) & mask;
            buffer[wj] = x[j];
            const float a = buffer[(wj - d0) & mask];
            x[j] = (t == nullptr || d0 == d1) ? a : a + t[j] * (buffer[(wj - d1) & mask] - a);
        }
    }

    /** Write only (history for a delay that is about to be switched on). */
    void write (const float* x, int n, int w) noexcept
    {
        constexpr int mask = kLookaheadRing - 1;
        for (int j = 0; j < n; ++j)
            buffer[(w + j) & mask] = x[j];
    }
};
} // namespace tl::dsp
