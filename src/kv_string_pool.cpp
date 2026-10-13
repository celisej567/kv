#include "kv_string_pool.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <stdexcept>

#ifdef KV_THREAD_SAFE
#define KV_READ_LOCK() std::shared_lock<std::shared_mutex> kvLock(mutex_)
#define KV_WRITE_LOCK() std::unique_lock<std::shared_mutex> kvLock(mutex_)
#else
#define KV_READ_LOCK() ((void)0)
#define KV_WRITE_LOCK() ((void)0)
#endif

namespace
{
// Byte-run equality; short runs use overlapping word loads instead of memcmp.
inline bool SameBytes(const char* a, const char* b, size_t n)
{
    if (n >= 8)
    {
        uint64_t x, y;

        for (; n > 8; a += 8, b += 8, n -= 8)
        {
            std::memcpy(&x, a, 8);
            std::memcpy(&y, b, 8);

            if (x != y)
                return false;
        }

        std::memcpy(&x, a + n - 8, 8);
        std::memcpy(&y, b + n - 8, 8);
        return x == y;
    }

    if (n >= 4)
    {
        uint32_t x1, y1, x2, y2;
        std::memcpy(&x1, a, 4);
        std::memcpy(&y1, b, 4);
        std::memcpy(&x2, a + n - 4, 4);
        std::memcpy(&y2, b + n - 4, 4);
        return x1 == y1 && x2 == y2;
    }

    for (size_t i = 0; i < n; ++i)
        if (a[i] != b[i])
            return false;

    return true;
}
}  // namespace

#ifdef KV_THREAD_SAFE
namespace
{
inline unsigned HighBit(uint32_t v)
{
#if defined(__GNUC__) || defined(__clang__)
    return 31u - static_cast<unsigned>(__builtin_clz(v));
#else
    unsigned r = 0;

    while (v >>= 1)
        ++r;

    return r;
#endif
}

inline void Locate(uint32_t i, unsigned kBaseBits, unsigned& k, uint32_t& off)
{
    k = HighBit((i >> kBaseBits) + 1);
    off = i - (((1u << k) - 1u) << kBaseBits);
}
}  // namespace

void KVStringPool::Entries::Push(const Entry& e)
{
    const size_t n = size.load(std::memory_order_relaxed);
    unsigned k;
    uint32_t off;
    Locate(static_cast<uint32_t>(n), kBaseBits, k, off);

    if (!seg[k])
    {
        const size_t count = size_t(1) << (kBaseBits + k);
        seg[k].reset(new Entry[count]);
        allocated += count * sizeof(Entry);
    }

    seg[k][off] = e;
    size.store(n + 1, std::memory_order_release);
}

const KVStringPool::Entry& KVStringPool::Entries::operator[](uint32_t i) const
{
    unsigned k;
    uint32_t off;
    Locate(i, kBaseBits, k, off);

    return seg[k][off];
}

const KVStringPool::Entry& KVStringPool::Entries::At(uint32_t i) const
{
    if (i >= Size())
        throw std::out_of_range("KVStringPool: unknown slot");

    return (*this)[i];
}
#endif

KVStringPool::KVStringPool(size_t reserveBytes)
    : nextChunk_(std::min(std::max<size_t>(reserveBytes, 64), kMaxChunk))
{
    table_.assign(1024, kInvalidSlot);
    curChunk_ = AddChunk(nextChunk_);
    curSize_ = nextChunk_;
    nextChunk_ = std::min(nextChunk_ * 2, kMaxChunk);
}

KVStringPool& KVStringPool::Instance()
{
    static KVStringPool pool;

    return pool;
}

uint32_t KVStringPool::FindLocked(std::string_view s, uint32_t hash) const
{
    const size_t mask = table_.size() - 1;

    for (size_t i = hash & mask;; i = (i + 1) & mask)
    {
        const uint32_t slot = table_[i];

        if (slot == kInvalidSlot)
            return kInvalidSlot;

        const Entry& e = entries_[slot];

        if (e.hash == hash && e.length == s.size() && SameBytes(Text(e.ref), s.data(), s.size()))
            return slot;
    }
}

uint32_t KVStringPool::Find(std::string_view s) const
{
    const uint32_t hash = KVHashString(s);
    KV_READ_LOCK();

    return FindLocked(s, hash);
}

void KVStringPool::Rehash(size_t newSize)
{
    table_.assign(newSize, kInvalidSlot);
    const size_t mask = newSize - 1;
    const uint32_t n = static_cast<uint32_t>(entries_.Size());

    for (uint32_t slot = 0; slot < n; ++slot)
    {
        size_t i = entries_[slot].hash & mask;

        while (table_[i] != kInvalidSlot)
            i = (i + 1) & mask;

        table_[i] = slot;
    }
}

size_t KVStringPool::AddChunk(size_t bytes)
{
    if (chunkCount_ == kMaxChunks)
        throw std::length_error("KVStringPool: chunk limit exceeded");

    chunks_[chunkCount_].reset(new char[bytes]);
    charsAllocated_ += bytes;

    return chunkCount_++;
}

uint32_t KVStringPool::Store(std::string_view s)
{
    const size_t need = s.size() + 1;
    size_t chunk, offset;

    if (need <= curSize_ - curUsed_)
    {
        chunk = curChunk_;
        offset = curUsed_;
        curUsed_ += need;
    }
    else if (need > kMaxChunk / 4)
    {
        chunk = AddChunk(need);
        offset = 0;
    }
    else
    {
        // Chunk grows until the string is at most a quarter of it (bounds the wasted tail).
        while (nextChunk_ < kMaxChunk && need > nextChunk_ / 4)
            nextChunk_ *= 2;

        chunk = curChunk_ = AddChunk(nextChunk_);
        curSize_ = nextChunk_;
        curUsed_ = need;
        offset = 0;
        nextChunk_ = std::min(nextChunk_ * 2, kMaxChunk);
    }

    char* dst = chunks_[chunk].get() + offset;

    if (!s.empty())
        std::memcpy(dst, s.data(), s.size());

    dst[s.size()] = '\0';
    chars_ += need;

    return static_cast<uint32_t>((chunk << kOffsetBits) | offset);
}

uint32_t KVStringPool::InsertLocked(std::string_view s, uint32_t hash)
{
    if (s.size() >= 0xFFFFFFFFu || entries_.Size() + 1 >= kInvalidSlot)
        throw std::length_error("KVStringPool: limit exceeded");

    if ((entries_.Size() + 1) * 100 > table_.size() * kMaxLoadPercent)
        Rehash(table_.size() * 2);

    const uint32_t slot = static_cast<uint32_t>(entries_.Size());
    const uint32_t ref = Store(s);
    entries_.Push({ref, static_cast<uint32_t>(s.size()), hash});

    const size_t mask = table_.size() - 1;
    size_t i = hash & mask;

    while (table_[i] != kInvalidSlot)
        i = (i + 1) & mask;

    table_[i] = slot;

    return slot;
}

uint32_t KVStringPool::Intern(std::string_view s)
{
    return Intern(s, KVHashString(s));
}

uint32_t KVStringPool::Intern(std::string_view s, uint32_t hash)
{
#ifdef KV_THREAD_SAFE
    {
        // Most interns hit an existing string: try under the shared lock first.
        KV_READ_LOCK();
        const uint32_t found = FindLocked(s, hash);

        if (found != kInvalidSlot)
            return found;
    }
#endif
    KV_WRITE_LOCK();
    const uint32_t found = FindLocked(s, hash);

    if (found != kInvalidSlot)
        return found;

    return InsertLocked(s, hash);
}

std::string_view KVStringPool::View(uint32_t slot) const
{
    const Entry& e = entries_.At(slot);

    return std::string_view(Text(e.ref), e.length);
}

const char* KVStringPool::CStr(uint32_t slot) const
{
    return Text(entries_.At(slot).ref);
}

size_t KVStringPool::Count() const
{
    return entries_.Size();
}

size_t KVStringPool::BytesUsed() const
{
    KV_READ_LOCK();

    return chars_;
}

KVStringPool::Stats KVStringPool::StatsLocked() const
{
    Stats st;
    st.strings = entries_.Size();
    st.chars = chars_;
    st.chunks = chunkCount_;
    st.charsAllocated = charsAllocated_;
    st.entriesAllocated = entries_.Allocated();
    st.tableAllocated = table_.capacity() * sizeof(uint32_t);
    st.allocated = st.charsAllocated + st.entriesAllocated + st.tableAllocated;
    st.used = st.chars + st.strings * (sizeof(Entry) + sizeof(uint32_t));
    st.directory = sizeof(chunks_);

    return st;
}

KVStringPool::Stats KVStringPool::GetStats() const
{
    KV_READ_LOCK();

    return StatsLocked();
}

size_t KVStringPool::MemoryUsage() const
{
    return GetStats().allocated;
}
