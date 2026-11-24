#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>

// Single-producer / single-consumer ring buffer that carries a mono copy of the audio from the
// audio thread to the editor. Wait-free on both sides; fixed storage; when the reader falls
// behind, whole blocks are dropped (the audio thread never waits and never overwrites).
namespace tl::rt
{
class AnalyzerFifo
{
public:
    static constexpr int capacity = 1 << 14;   // 16384 samples (power of two)

    /** Audio thread. Writes the mono mix (average) of up to two channels. Returns false if the
        block did not fit and was dropped. `right` may be nullptr (mono). */
    bool push (const float* left, const float* right, int numSamples) noexcept
    {
        const auto w = writePos.load (std::memory_order_relaxed);
        const auto r = readPos.load (std::memory_order_acquire);
        const auto freeSpace = (std::size_t) capacity - (w - r);

        if (numSamples <= 0 || (std::size_t) numSamples > freeSpace)
            return false;

        for (int i = 0; i < numSamples; ++i)
        {
            const float v = right != nullptr ? 0.5f * (left[i] + right[i]) : left[i];
            data[(w + (std::size_t) i) & mask] = v;
        }

        writePos.store (w + (std::size_t) numSamples, std::memory_order_release);
        return true;
    }

    /** Reader thread. Copies up to maxSamples into dest and returns how many were read. */
    int pull (float* dest, int maxSamples) noexcept
    {
        const auto r = readPos.load (std::memory_order_relaxed);
        const auto w = writePos.load (std::memory_order_acquire);
        const int n = (int) std::min ((std::size_t) std::max (0, maxSamples), w - r);

        for (int i = 0; i < n; ++i)
            dest[i] = data[(r + (std::size_t) i) & mask];

        readPos.store (r + (std::size_t) n, std::memory_order_release);
        return n;
    }

    /** Reader thread: discards everything currently queued. */
    void clear() noexcept { readPos.store (writePos.load (std::memory_order_acquire), std::memory_order_release); }

    int available() const noexcept
    {
        return (int) (writePos.load (std::memory_order_acquire) - readPos.load (std::memory_order_acquire));
    }

private:
    static constexpr std::size_t mask = (std::size_t) capacity - 1;
    static_assert (std::atomic<std::size_t>::is_always_lock_free);

    float data[capacity] {};
    std::atomic<std::size_t> writePos { 0 }, readPos { 0 };
};
} // namespace tl::rt
