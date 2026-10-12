#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <vector>

#ifdef KV_THREAD_SAFE
#include <atomic>
#include <shared_mutex>
#endif

// Assisted and commented using Claude. (sry too lazy to do them myself).

constexpr uint32_t kInvalidSlot = 0xFFFFFFFFu;

// Append-only pool of unique strings. Strings are never removed or moved, so slots and the
// pointers returned by View/CStr stay valid for the lifetime of the pool.
// With KV_THREAD_SAFE: Find and the first lookup of Intern take a shared lock, inserts an
// exclusive one, and View/CStr no lock at all.
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
    // Segment k holds (256 << k) entries and never moves, so readers need no lock. `size` is
    // published with release/acquire: a reader that sees a slot below it sees the entry too.
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
