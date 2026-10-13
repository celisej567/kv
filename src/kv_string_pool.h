#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

#ifdef KV_THREAD_SAFE
#include <atomic>
#include <shared_mutex>
#endif

// Assisted and commented using Claude. (sry too lazy to do them myself).

constexpr uint32_t kInvalidSlot = 0xFFFFFFFFu;

inline uint64_t KVLoad64(const char* p)
{
    uint64_t v;
    std::memcpy(&v, p, 8);
    return v;
}

inline uint64_t KVLoad32(const char* p)
{
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

inline uint64_t KVMix(uint64_t a, uint64_t b)
{
#if defined(__SIZEOF_INT128__)
    const unsigned __int128 r = static_cast<unsigned __int128>(a) * b;
    return static_cast<uint64_t>(r) ^ static_cast<uint64_t>(r >> 64);
#else
    const uint64_t lo = (a & 0xFFFFFFFFu) * (b & 0xFFFFFFFFu);
    const uint64_t mid = (a >> 32) * (b & 0xFFFFFFFFu) + (a & 0xFFFFFFFFu) * (b >> 32);
    return (lo ^ (mid << 32)) + (a >> 32) * (b >> 32) + (mid >> 32);
#endif
}

// The pool's string hash (word at a time). Exposed so a caller hashes once for repeated lookups.
inline uint32_t KVHashString(std::string_view s)
{
    constexpr uint64_t k0 = 0xa0761d6478bd642full, k1 = 0xe7037ed1a0b428dbull, k2 = 0x8ebc6af09c88c6e3ull;
    const char* p = s.data();
    size_t n = s.size();
    uint64_t h = k0 ^ n;

    while (n > 16)
    {
        h = KVMix(KVLoad64(p) ^ k1, KVLoad64(p + 8) ^ h);
        p += 16;
        n -= 16;
    }

    uint64_t a = 0, b = 0;

    if (n >= 8)
    {
        a = KVLoad64(p);
        b = KVLoad64(p + n - 8);
    }
    else if (n >= 4)
    {
        a = KVLoad32(p);
        b = KVLoad32(p + n - 4);
    }
    else if (n > 0)
    {
        a = (uint64_t(static_cast<unsigned char>(p[0])) << 16) | (uint64_t(static_cast<unsigned char>(p[n >> 1])) << 8) |
            static_cast<unsigned char>(p[n - 1]);
    }

    const uint64_t f = KVMix(KVMix(a ^ k1, b ^ h), k2);

    return static_cast<uint32_t>(f ^ (f >> 32));
}

// Append-only pool of unique strings: slots and View/CStr pointers stay valid for the pool's life.
// KV_THREAD_SAFE: Find and the first Intern lookup take a shared lock, inserts an exclusive one,
// View/CStr none.
class KVStringPool
{
public:
    // Bytes. `directory` (the fixed chunk table) lives inside the pool object, not on the heap.
    struct Stats
    {
        size_t strings = 0;
        size_t chars = 0;
        size_t chunks = 0;
        size_t charsAllocated = 0;
        size_t entriesAllocated = 0;
        size_t tableAllocated = 0;
        size_t allocated = 0;
        size_t used = 0;
        size_t directory = 0;
    };

    explicit KVStringPool(size_t reserveBytes = 64 * 1024);
    KVStringPool(const KVStringPool&) = delete;
    KVStringPool& operator=(const KVStringPool&) = delete;

    static KVStringPool& Instance();

    uint32_t Intern(std::string_view s);
    uint32_t Intern(std::string_view s, uint32_t hash);  // hash = KVHashString(s)
    uint32_t Find(std::string_view s) const;     // kInvalidSlot if absent
    std::string_view View(uint32_t slot) const;  // throws std::out_of_range for an unknown slot
    const char* CStr(uint32_t slot) const;
    size_t Count() const;
    size_t BytesUsed() const;
    size_t MemoryUsage() const;
    Stats GetStats() const;

private:
    struct Entry
    {
        uint32_t ref;
        uint32_t length;
        uint32_t hash;
    };

    static constexpr unsigned kOffsetBits = 22;
    static constexpr size_t kMaxChunk = size_t(1) << kOffsetBits;
    static constexpr size_t kMaxChunks = size_t(1) << (32 - kOffsetBits);
    static constexpr unsigned kMaxLoadPercent = 75;

#ifdef KV_THREAD_SAFE
    // Segment k holds (256 << k) entries and never moves; `size` is published release/acquire.
    struct Entries
    {
        static constexpr unsigned kBaseBits = 8;
        std::unique_ptr<Entry[]> seg[32];
        std::atomic<size_t> size{0};
        size_t allocated = 0;
        void Push(const Entry& e);
        const Entry& At(uint32_t i) const;  // throws std::out_of_range
        const Entry& operator[](uint32_t i) const;
        size_t Size() const { return size.load(std::memory_order_acquire); }
        size_t Allocated() const { return allocated; }
    };
#else
    struct Entries
    {
        std::vector<Entry> v;
        void Push(const Entry& e) { v.push_back(e); }
        const Entry& At(uint32_t i) const { return v.at(i); }
        const Entry& operator[](uint32_t i) const { return v[i]; }
        size_t Size() const { return v.size(); }
        size_t Allocated() const { return v.capacity() * sizeof(Entry); }
    };
#endif

    const char* Text(uint32_t ref) const { return chunks_[ref >> kOffsetBits].get() + (ref & (kMaxChunk - 1)); }
    uint32_t FindLocked(std::string_view s, uint32_t hash) const;
    uint32_t InsertLocked(std::string_view s, uint32_t hash);
    uint32_t Store(std::string_view s);
    size_t AddChunk(size_t bytes);
    void Rehash(size_t newSize);
    Stats StatsLocked() const;

    std::unique_ptr<char[]> chunks_[kMaxChunks];
    size_t chunkCount_ = 0;
    size_t curChunk_ = 0;
    size_t curUsed_ = 0;
    size_t curSize_ = 0;
    size_t nextChunk_;
    size_t charsAllocated_ = 0;
    size_t chars_ = 0;

    Entries entries_;
    std::vector<uint32_t> table_;

#ifdef KV_THREAD_SAFE
    mutable std::shared_mutex mutex_;
#endif
};
