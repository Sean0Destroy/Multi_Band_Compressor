#include "AllocationCounter.h"

#include <cstdlib>
#include <new>

#if defined (_WIN32)
 #include <malloc.h>   // _aligned_malloc / _aligned_free
#endif

namespace
{
thread_local bool tInCallback = false;
std::atomic<std::size_t> gCount { 0 };

inline void note() noexcept
{
    if (tInCallback)
        gCount.fetch_add (1, std::memory_order_relaxed);
}

void* alloc (std::size_t n)
{
#if ! TL_TEST_MALLOC_INTERPOSED
    note();   // with the interposer the malloc call below is counted instead
#endif
    if (n == 0) n = 1;
    if (void* p = std::malloc (n)) return p;
    throw std::bad_alloc();
}

void* allocAligned (std::size_t n, std::size_t align)
{
    note();
    if (n == 0) n = 1;
    if (align < sizeof (void*)) align = sizeof (void*);
    void* p = nullptr;
#if defined (_WIN32)
    p = _aligned_malloc (n, align);   // must be released with _aligned_free
    if (p != nullptr) return p;
#else
    if (posix_memalign (&p, align, n) == 0) return p;
#endif
    throw std::bad_alloc();
}

void freeAligned (void* p) noexcept
{
#if defined (_WIN32)
    _aligned_free (p);   // memory from _aligned_malloc cannot go through free()
#else
    std::free (p);
#endif
}
} // namespace

namespace tl::test
{
bool& inAudioCallback() noexcept { return tInCallback; }
std::size_t allocationCount() noexcept { return gCount.load(); }
void resetAllocationCount() noexcept { gCount.store (0); }
} // namespace tl::test

void* operator new (std::size_t n)                                   { return alloc (n); }
void* operator new[] (std::size_t n)                                 { return alloc (n); }
void* operator new (std::size_t n, const std::nothrow_t&) noexcept   { try { return alloc (n); } catch (...) { return nullptr; } }
void* operator new[] (std::size_t n, const std::nothrow_t&) noexcept { try { return alloc (n); } catch (...) { return nullptr; } }
void* operator new (std::size_t n, std::align_val_t a)               { return allocAligned (n, (std::size_t) a); }
void* operator new[] (std::size_t n, std::align_val_t a)             { return allocAligned (n, (std::size_t) a); }

// Deallocation is counted too: freeing memory on the audio thread is as forbidden as allocating.
// With the glibc interposer, std::free below counts; otherwise only operator new is counted.
void operator delete (void* p) noexcept                              { std::free (p); }
void operator delete[] (void* p) noexcept                            { std::free (p); }
void operator delete (void* p, std::size_t) noexcept                 { std::free (p); }
void operator delete[] (void* p, std::size_t) noexcept               { std::free (p); }
void operator delete (void* p, std::align_val_t) noexcept            { freeAligned (p); }
void operator delete[] (void* p, std::align_val_t) noexcept          { freeAligned (p); }
void operator delete (void* p, std::size_t, std::align_val_t) noexcept   { freeAligned (p); }
void operator delete[] (void* p, std::size_t, std::align_val_t) noexcept { freeAligned (p); }

#if TL_TEST_MALLOC_INTERPOSED
// Interpose the C allocation functions for this test executable so that malloc-based
// allocations (e.g. juce::HeapBlock, used by juce::AudioBuffer) are also detected.
extern "C"
{
void* __libc_malloc (std::size_t);
void* __libc_calloc (std::size_t, std::size_t);
void* __libc_realloc (void*, std::size_t);
void  __libc_free (void*);

void* malloc (std::size_t n)                 { note(); return __libc_malloc (n); }
void* calloc (std::size_t a, std::size_t b)  { note(); return __libc_calloc (a, b); }
void* realloc (void* p, std::size_t n)       { note(); return __libc_realloc (p, n); }
void  free (void* p)                         { if (p != nullptr) note(); __libc_free (p); }
}
#endif
