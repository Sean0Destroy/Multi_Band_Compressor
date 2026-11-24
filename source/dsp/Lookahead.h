#pragma once

#include <algorithm>
#include <cmath>

#include "IBandEngine.h"
#include "Oversampler.h"

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

/** Everything the plugin reports as latency on top of the band filters themselves: the lookahead
    delay plus the oversampling filters of the coloration and of the output clipper (each
    whenever its oversampling is on, whether or not that stage is in use, so that automating the
    coloration or switching the clipper never changes the latency). */
inline int processingLatencySamples (const EngineSettings& s, double sampleRate) noexcept
{
    return lookaheadLatencySamples (s, sampleRate)
         + oversamplingLatency (std::clamp (s.oversamplingStages, 0, kMaxOversamplingStages))
         + oversamplingLatency (std::clamp (s.clipOversamplingStages, 0, kMaxOversamplingStages));
}

/** Ring buffer with two read taps that are crossfaded when the delay changes. With one tap
    (d0 == d1) the output is an exact copy of the input d0 samples ago. */
struct FadingDelayLine
{
    float buffer[kLookaheadRing] {};

    void reset() noexcept { fill (0.0f); }
    /** History of a constant: 0 for audio, 1 for a gain ("nothing was happening"). */
    void fill (float value) noexcept { std::fill (std::begin (buffer), std::end (buffer), value); }

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
/** Several parallel delay lines with one common delay that can change while running: the
    lines keep their history, and a change crossfades the read taps over 10 ms. Starting from
    "off" (no delay, nothing stored) first collects history: a crossfade to a tap only starts
    once that much history exists, however the target moves in the meantime. With delay 0 and
    nothing pending it does nothing at all. */
template <int NumLines>
class SwitchableDelay
{
public:
    void prepare (double sampleRate) noexcept
    {
        fadeLength = std::max (1, (int) std::lround (kLookaheadFadeSeconds * sampleRate));
        snapTo (0);
    }

    /** Start of a stream: the delay is in place immediately. */
    void snapTo (int delay) noexcept
    {
        for (auto& l : lines) l.reset();
        current = next = wanted = std::clamp (delay, 0, kLookaheadRing - 1);
        running = current != 0;
        fading = false;
        fadePosition = write = 0;
        valid = kLookaheadRing;      // the history is the silence before the stream
    }

    /** Call once per chunk before process(). Returns false if there is nothing to do. */
    bool begin (int target, int n) noexcept
    {
        target = std::clamp (target, 0, kLookaheadRing - 1);
        wanted = target;
        fade = nullptr;

        if (! running)
        {
            if (target == 0) return false;
            for (auto& l : lines) l.reset();
            current = next = 0;
            write = 0;
            valid = 0;
            running = true;
        }

        // A tap is only faded in once the lines hold that much real history.
        if (! fading && current != target && valid >= target)
        {
            next = target;
            fading = true;
            fadePosition = 0;
        }
        if (fading)
        {
            for (int j = 0; j < n; ++j)
                fadeBuffer[j] = std::min (1.0f, (float) (fadePosition + j + 1) / (float) fadeLength);
            fade = fadeBuffer;
        }
        return true;
    }

    void process (int line, float* x, int n) noexcept
    {
        lines[line].process (x, n, write, current, fading ? next : current, fade);
    }

    /** Call once per chunk after process(). */
    void end (int target, int n) noexcept
    {
        write = (write + n) & (kLookaheadRing - 1);
        valid = std::min (kLookaheadRing, valid + n);
        if (fading)
        {
            fadePosition += n;
            if (fadePosition >= fadeLength) { current = next; fading = false; }
        }
        if (! fading && current == 0 && target == 0)
            running = false;
    }

    /** Delay in effect (the old one until a crossfade has finished). */
    int currentDelay() const noexcept { return current; }
    /** True if the delay is `delay` and nothing else is wanted or on its way. */
    bool isSettledAt (int delay) const noexcept { return ! fading && current == delay && wanted == delay; }

private:
    FadingDelayLine lines[(size_t) NumLines];
    int current = 0, next = 0, wanted = 0, fadeLength = 480, fadePosition = 0, write = 0;
    int valid = 0;               // samples of real history in the lines (capped at the ring size)
    bool running = false, fading = false;
    const float* fade = nullptr;
    float fadeBuffer[kChunk] {};
};
} // namespace tl::dsp
