#pragma once

#include <atomic>
#include <cstddef>

// glibc malloc/calloc/realloc/free interposition (AllocationCounter.cpp) is only compiled when it
// cannot clash with a sanitizer runtime that owns the allocator itself (AddressSanitizer,
// ThreadSanitizer). Without it, only operator new / delete are counted.
#if defined (__has_feature)
 #if __has_feature (address_sanitizer) || __has_feature (thread_sanitizer)
  #define TL_TEST_UNDER_ASAN 1
 #endif
#endif
#if (defined (__SANITIZE_ADDRESS__) || defined (__SANITIZE_THREAD__)) && ! defined (TL_TEST_UNDER_ASAN)
 #define TL_TEST_UNDER_ASAN 1
#endif

#if defined (__GLIBC__) && ! defined (TL_TEST_UNDER_ASAN)
 #define TL_TEST_MALLOC_INTERPOSED 1
#else
 #define TL_TEST_MALLOC_INTERPOSED 0
#endif

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
