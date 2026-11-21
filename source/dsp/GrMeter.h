#pragma once

#include <atomic>
#include <limits>

#include "DspConstants.h"

namespace tl::dsp
{
/** Gain-reduction meter data, per slot and channel, in dB (<= 0; 0 for unused or fully bypassed
    bands). Written by the audio thread once per block, read lock-free from any thread:
    - getGainReductionDb(): the minimum applied band gain of the LAST block (non-destructive peek);
    - readAndResetHeldGainReductionDb(): the deepest value since the previous call (min-hold until
      read), so a UI polling slower than the block rate cannot miss transients; with no new
      block since the previous call it returns the last block's value. */
class GrMeter
{
public:
    GrMeter() noexcept { clear(); }

    void clear() noexcept
    {
        for (auto& v : last) v.store (0.0f, std::memory_order_relaxed);
        for (auto& v : held) v.store (kNoData, std::memory_order_relaxed);
    }

    /** Audio thread. Lock-free: the compare-exchange only retries if a reader reset the value
        in between (at most once per read). */
    void publish (int bandIndex, int channel, float grDb) noexcept
    {
        const int i = bandIndex * kMaxChannels + channel;
        last[i].store (grDb, std::memory_order_relaxed);

        float cur = held[i].load (std::memory_order_relaxed);
        while (grDb < cur && ! held[i].compare_exchange_weak (cur, grDb, std::memory_order_relaxed))
        {
        }
    }

    /** slot is 1-based (1..kMaxBands), channel 0/1. Out-of-range arguments read 0. */
    float getGainReductionDb (int slot, int channel) const noexcept
    {
        if (! valid (slot, channel)) return 0.0f;
        return last[(slot - 1) * kMaxChannels + channel].load (std::memory_order_relaxed);
    }

    /** Deepest GR published since the previous call for this slot/channel (consumes the hold).
        If no block was published since the previous call (a UI polling faster than the block
        rate), returns the last block's value instead of a spurious 0 dB. */
    float readAndResetHeldGainReductionDb (int slot, int channel) const noexcept
    {
        if (! valid (slot, channel)) return 0.0f;
        const int i = (slot - 1) * kMaxChannels + channel;
        const float v = held[i].exchange (kNoData, std::memory_order_relaxed);
        return v < kNoData ? v : last[i].load (std::memory_order_relaxed);
    }

private:
    static bool valid (int slot, int channel) noexcept
    {
        return slot >= 1 && slot <= kMaxBands && channel >= 0 && channel < kMaxChannels;
    }

    // "Nothing published since the last read": above every real value, so publish()'s
    // min-accumulate replaces it with the first published block.
    static constexpr float kNoData = std::numeric_limits<float>::infinity();

    static_assert (std::atomic<float>::is_always_lock_free);
    std::atomic<float> last[kMaxBands * kMaxChannels];
    mutable std::atomic<float> held[kMaxBands * kMaxChannels];   // reading consumes the hold
};
} // namespace tl::dsp
