#pragma once

#include <atomic>
#include <cstddef>

// Test-binary-wide global operator new/delete override (AllocationCounter.cpp).
// Allocations are counted only while the calling thread has the "in audio callback" flag set.
namespace tl::test
{
bool& inAudioCallback() noexcept;           // thread_local flag
std::size_t allocationCount() noexcept;     // allocations seen while the flag was set
void resetAllocationCount() noexcept;

struct AudioCallbackScope
{
    AudioCallbackScope() noexcept  { inAudioCallback() = true; }
    ~AudioCallbackScope() noexcept { inAudioCallback() = false; }
};
} // namespace tl::test
