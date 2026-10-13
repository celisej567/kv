#pragma once
// Replaces global operator new/delete to count live and peak heap bytes.
// Include in exactly ONE translation unit of an executable (it defines the global operators).
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>

// Not inlined: GCC otherwise emits false -Warray-bounds / -Wmismatched-new-delete warnings.
#if defined(__GNUC__) || defined(__clang__)
#define COUNTING_ALLOC_NOINLINE __attribute__((noinline))
#elif defined(_MSC_VER)
#define COUNTING_ALLOC_NOINLINE __declspec(noinline)
#else
#define COUNTING_ALLOC_NOINLINE
#endif

namespace counting_alloc {

inline std::atomic<size_t> g_live{0};
inline std::atomic<size_t> g_peak{0};
// Fault injection: k >= 0 makes the (k+1)-th allocation throw bad_alloc, then resets to -1 (off).
inline std::atomic<long> g_failIn{-1};
constexpr size_t kHeader = 16;  // keeps the 16-byte alignment malloc gives

COUNTING_ALLOC_NOINLINE inline void* Alloc(size_t n) {
    if (g_failIn.load(std::memory_order_relaxed) >= 0 && g_failIn.fetch_sub(1, std::memory_order_relaxed) == 0) return nullptr;
    void* p = std::malloc(n + kHeader);
    if (!p) return nullptr;
    *static_cast<size_t*>(p) = n;
    const size_t live = g_live.fetch_add(n, std::memory_order_relaxed) + n;
    size_t peak = g_peak.load(std::memory_order_relaxed);
    while (live > peak && !g_peak.compare_exchange_weak(peak, live, std::memory_order_relaxed)) {}
    return static_cast<char*>(p) + kHeader;
}

COUNTING_ALLOC_NOINLINE inline void Free(void* p) noexcept {
    if (!p) return;
    char* base = static_cast<char*>(p) - kHeader;
    g_live.fetch_sub(*reinterpret_cast<size_t*>(base), std::memory_order_relaxed);
    std::free(base);
}

inline size_t Live() { return g_live.load(); }
inline size_t Peak() { return g_peak.load(); }
inline void ResetPeak() { g_peak.store(g_live.load()); }

}  // namespace counting_alloc

void* operator new(size_t n) { if (void* p = counting_alloc::Alloc(n)) return p; throw std::bad_alloc(); }
void* operator new[](size_t n) { if (void* p = counting_alloc::Alloc(n)) return p; throw std::bad_alloc(); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return counting_alloc::Alloc(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return counting_alloc::Alloc(n); }
void operator delete(void* p) noexcept { counting_alloc::Free(p); }
void operator delete[](void* p) noexcept { counting_alloc::Free(p); }
void operator delete(void* p, size_t) noexcept { counting_alloc::Free(p); }
void operator delete[](void* p, size_t) noexcept { counting_alloc::Free(p); }
