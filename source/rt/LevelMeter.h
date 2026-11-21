#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>

// Level metering data for one bus (up to two channels): written by the audio thread once per
// block, read lock-free by the editor.
//   peak: highest |x| since the previous read (max-hold until read); with no new block since
//         then, the last block's peak;
//   RMS:  mean square smoothed with a 300 ms time constant (updated per block).
namespace tl::rt
{
class LevelMeter
{
public:
    static constexpr int maxChannels = 2;
    static constexpr double rmsSeconds = 0.3;
    static constexpr float silenceDb = -120.0f;

    LevelMeter() noexcept { clear(); }

    void prepare (double sampleRate) noexcept
    {
        fs = sampleRate > 0.0 ? sampleRate : 48000.0;
        clear();
    }

    void clear() noexcept
    {
        for (int c = 0; c < maxChannels; ++c)
        {
            lastPeak[c].store (0.0f, std::memory_order_relaxed);
            heldPeak[c].store (kNoData, std::memory_order_relaxed);
            meanSquare[c].store (0.0f, std::memory_order_relaxed);
            state[c] = 0.0;
        }
        channels.store (0, std::memory_order_relaxed);
    }

    /** Audio thread: one block of one channel. */
    void process (int channel, const float* x, int numSamples) noexcept
    {
        if (channel < 0 || channel >= maxChannels || numSamples <= 0) return;

        float peak = 0.0f;
        double sum = 0.0;
        for (int i = 0; i < numSamples; ++i)
        {
            const float a = std::abs (x[i]);
            peak = std::max (peak, a);
            sum += (double) x[i] * (double) x[i];
        }

        lastPeak[channel].store (peak, std::memory_order_relaxed);
        float cur = heldPeak[channel].load (std::memory_order_relaxed);
        while ((cur < 0.0f || peak > cur) && ! heldPeak[channel].compare_exchange_weak (cur, peak, std::memory_order_relaxed))
        {
        }

        // One-pole over the block: m += (1 - exp(-n / (tau fs))) (blockMeanSquare - m).
        const double k = 1.0 - std::exp (-(double) numSamples / (rmsSeconds * fs));
        state[channel] += k * (sum / (double) numSamples - state[channel]);
        meanSquare[channel].store ((float) state[channel], std::memory_order_relaxed);
    }

    /** Audio thread: how many channels the bus has in this block (0 = bus off). */
    void setChannelCount (int n) noexcept { channels.store (std::clamp (n, 0, maxChannels), std::memory_order_relaxed); }

    // ---- Reader --------------------------------------------------------------------------------
    int getChannelCount() const noexcept { return channels.load (std::memory_order_relaxed); }

    /** Highest peak since the previous call (dBFS, can exceed 0); consumes the hold. */
    float readAndResetPeakDb (int channel) const noexcept
    {
        if (channel < 0 || channel >= maxChannels) return silenceDb;
        const float held = heldPeak[channel].exchange (kNoData, std::memory_order_relaxed);
        return toDb (held >= 0.0f ? held : lastPeak[channel].load (std::memory_order_relaxed));
    }

    float getRmsDb (int channel) const noexcept
    {
        if (channel < 0 || channel >= maxChannels) return silenceDb;
        const float ms = meanSquare[channel].load (std::memory_order_relaxed);
        return ms > 1.0e-12f ? std::max (silenceDb, 10.0f * std::log10 (ms)) : silenceDb;
    }

private:
    static float toDb (float linear) noexcept
    {
        return linear > 1.0e-6f ? std::max (silenceDb, 20.0f * std::log10 (linear)) : silenceDb;
    }

    static constexpr float kNoData = -1.0f;   // "nothing published since the last read" (peaks are >= 0)
    static_assert (std::atomic<float>::is_always_lock_free);

    double fs = 48000.0;
    double state[maxChannels] {};                       // audio thread only
    std::atomic<float> lastPeak[maxChannels];
    mutable std::atomic<float> heldPeak[maxChannels];   // reading consumes the hold
    std::atomic<float> meanSquare[maxChannels];
    std::atomic<int> channels { 0 };
};
} // namespace tl::rt
