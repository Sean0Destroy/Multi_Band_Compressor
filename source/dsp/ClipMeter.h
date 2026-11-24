#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace tl::dsp
{
/** What the output clipper does, for the editor's clip scope and "Shaved" readout.

    The audio thread folds each processed chunk into a column of about 1/120 s: the highest |x|
    going into the clipper, the highest |x| coming out, and whether the clipper was on. Finished
    columns go into a ring of kRingSize slots; a single reader (the editor timer) pulls the new
    ones with readColumns().

    Real-time safety: the writer only stores relaxed atomics into a fixed ring and then publishes
    the column count with a release store. No allocation, no locks, no waiting. The reader loads
    the count with acquire; if it fell more than the ring behind it skips ahead, and it re-checks
    the count after copying so a slot the writer overwrote meanwhile is dropped, never shown torn.

    Shaved dB = 20 log10 (inPeak / outPeak). Both clip curves are memoryless and monotonic in |x|,
    so the column's loudest input sample is also its loudest output sample: the ratio of the two
    peaks is exactly how far the clipper pulled that peak down. */
class ClipMeter
{
public:
    static constexpr int kRingSize = 256;          // about 2 s of columns at 120 per second
    static constexpr double kColumnsPerSecond = 120.0;

    struct Column
    {
        float inPeak = 0.0f;   // linear
        float outPeak = 0.0f;  // linear
        bool clipperOn = false;

        /** dB the clipper took off this column's peak (>= 0). */
        float shavedDb() const noexcept
        {
            if (! clipperOn || outPeak <= 0.0f || inPeak <= outPeak) return 0.0f;
            return 20.0f * std::log10 (inPeak / outPeak);
        }
    };

    ClipMeter() noexcept { clear(); }

    /** Message thread (prepare). */
    void prepare (double sampleRate) noexcept
    {
        columnLength = std::max (1, (int) std::lround ((sampleRate > 0.0 ? sampleRate : 48000.0) / kColumnsPerSecond));
        clear();
    }

    /** Audio thread (reset) or message thread. Readers see the count restart; see readColumns. */
    void clear() noexcept
    {
        accIn = accOut = 0.0f;
        accOn = false;
        accCount = 0;
        for (auto& s : ring)
        {
            s.in.store (0.0f, std::memory_order_relaxed);
            s.out.store (0.0f, std::memory_order_relaxed);
            s.on.store (0, std::memory_order_relaxed);
        }
        written.store (0, std::memory_order_release);
    }

    /** Audio thread: one chunk's peaks (linear, already the max over channels). */
    void add (float inPeak, float outPeak, bool clipperOn, int numSamples) noexcept
    {
        accIn = std::max (accIn, sanitise (inPeak));
        accOut = std::max (accOut, sanitise (outPeak));
        accOn = accOn || clipperOn;
        accCount += numSamples;
        if (accCount < columnLength) return;

        const auto w = written.load (std::memory_order_relaxed);
        auto& s = ring[w % (std::uint32_t) kRingSize];
        s.in.store (accIn, std::memory_order_relaxed);
        s.out.store (accOut, std::memory_order_relaxed);
        s.on.store (accOn ? 1 : 0, std::memory_order_relaxed);
        written.store (w + 1, std::memory_order_release);

        accIn = accOut = 0.0f;
        accOn = false;
        accCount = 0;
    }

    // ---- Reader (one thread) -------------------------------------------------------------------
    /** Total number of columns written since the last clear. */
    std::uint32_t getColumnCount() const noexcept { return written.load (std::memory_order_acquire); }

    /** Copies the columns written after `cursor` (at most maxColumns, the newest ones if more
        are waiting) into dest, oldest first, and advances `cursor`. Returns how many were copied.
        A cursor ahead of the writer (after a clear) restarts from 0. */
    int readColumns (std::uint32_t& cursor, Column* dest, int maxColumns) const noexcept
    {
        if (maxColumns <= 0) return 0;
        const auto end = written.load (std::memory_order_acquire);
        if (cursor > end) cursor = 0;

        // Keep a margin below the ring size: the writer may add columns while we copy.
        constexpr std::uint32_t usable = (std::uint32_t) kRingSize - 16;
        const std::uint32_t limit = std::min (usable, (std::uint32_t) maxColumns);
        if (end - cursor > limit) cursor = end - limit;

        int count = 0;
        for (std::uint32_t i = cursor; i != end; ++i)
        {
            const auto& s = ring[i % (std::uint32_t) kRingSize];
            dest[count++] = { s.in.load (std::memory_order_relaxed),
                              s.out.load (std::memory_order_relaxed),
                              s.on.load (std::memory_order_relaxed) != 0 };
        }

        // Anything the writer may have overwritten while we copied is dropped.
        const auto after = written.load (std::memory_order_acquire);
        if (after >= end && after - cursor > (std::uint32_t) kRingSize)
        {
            const auto lost = (int) std::min<std::uint32_t> ((std::uint32_t) count, after - cursor - (std::uint32_t) kRingSize);
            std::copy (dest + lost, dest + count, dest);
            count -= lost;
        }
        cursor = end;
        return count;
    }

    int getColumnLength() const noexcept { return columnLength; }

private:
    static float sanitise (float v) noexcept { return std::isfinite (v) ? std::abs (v) : 0.0f; }

    struct Slot
    {
        std::atomic<float> in { 0.0f }, out { 0.0f };
        std::atomic<int> on { 0 };
    };

    Slot ring[kRingSize];
    std::atomic<std::uint32_t> written { 0 };

    // Writer state (audio thread only).
    float accIn = 0.0f, accOut = 0.0f;
    bool accOn = false;
    int accCount = 0;
    int columnLength = 400;
};

} // namespace tl::dsp
