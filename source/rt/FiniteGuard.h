#pragma once

#include <atomic>
#include <bit>
#include <cstdint>

// Real-time safe building blocks. Nothing in this folder may allocate, lock,
// log or perform I/O.
namespace tl::rt
{
/** Replaces NaN / +-Inf samples by 0 and records that it did so. */
class FiniteGuard
{
public:
    static constexpr bool isFinite (float x) noexcept
    {
        return (std::bit_cast<std::uint32_t> (x) & 0x7f800000u) != 0x7f800000u;
    }

    /** Sanitises numSamples of one channel. Returns true if anything was replaced. */
    bool process (float* data, int numSamples) noexcept
    {
        bool replaced = false;

        for (int i = 0; i < numSamples; ++i)
        {
            if (! isFinite (data[i]))
            {
                data[i] = 0.0f;
                replaced = true;
            }
        }

        if (replaced)
            triggered.store (true, std::memory_order_relaxed);

        return replaced;
    }

    bool wasTriggered() const noexcept          { return triggered.load (std::memory_order_relaxed); }
    bool exchangeTriggered() noexcept           { return triggered.exchange (false, std::memory_order_relaxed); }

private:
    std::atomic<bool> triggered { false };
};
} // namespace tl::rt
