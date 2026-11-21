#pragma once

#include <atomic>

#include "DspConstants.h"

namespace tl::dsp
{
/** Gain-reduction meter data: per slot and channel, the minimum applied band gain of the last
    processed block in dB (<= 0; 0 for unused or fully bypassed bands). Written by the audio
    thread once per block, read lock-free from any thread. */
class GrMeter
{
public:
    GrMeter() noexcept { clear(); }

    void clear() noexcept
    {
        for (auto& v : values) v.store (0.0f, std::memory_order_relaxed);
    }

    void publish (int bandIndex, int channel, float grDb) noexcept
    {
        values[bandIndex * kMaxChannels + channel].store (grDb, std::memory_order_relaxed);
    }

    /** slot is 1-based (1..kMaxBands), channel 0/1. Out-of-range arguments read 0. */
    float getGainReductionDb (int slot, int channel) const noexcept
    {
        if (slot < 1 || slot > kMaxBands || channel < 0 || channel >= kMaxChannels) return 0.0f;
        return values[(slot - 1) * kMaxChannels + channel].load (std::memory_order_relaxed);
    }

private:
    static_assert (std::atomic<float>::is_always_lock_free);
    std::atomic<float> values[kMaxBands * kMaxChannels];
};
} // namespace tl::dsp
