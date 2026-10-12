#pragma once
// Alternative string-pool designs, used only by kv_pool_bench to compare against KVStringPool.
// One template covers the whole design space; each axis targets one cost of the current pool:
//
//   Layout   Ptr16  entry = {char* text, len, hash}            16 B  (current pool)
//            Ref12  entry = {32-bit arena ref, len, hash}      12 B
//            Ref8   entry = {32-bit arena ref, len}, hash kept in the hash table
//   Table    slot-only (4 B per bucket) or slot+hash (8 B, probing never touches entries)
//   LoadPct  max fill of the hash table before it doubles (50 = current, 75 = less memory)
//   Hash     FNV-1a (byte at a time, current) or Fast (8 bytes at a time + final mix)
//   Seg      entries in a std::vector (doubling copies) or in segments that never move
//   Lock     none / mutex (current) / shared_mutex / shared_mutex + lock-free View
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

namespace pv {

constexpr uint32_t kNone = 0xFFFFFFFFu;

inline unsigned HighBit(uint32_t v) {  // floor(log2(v)), v > 0
#if defined(__GNUC__) || defined(__clang__)
    return 31u - static_cast<unsigned>(__builtin_clz(v));
#else
    unsigned r = 0;
    while (v >>= 1) ++r;
    return r;
#endif
}

// ---- hash functions -------------------------------------------------------------------------
struct Fnv1a {
    static constexpr const char* kName = "fnv";
    uint32_t operator()(std::string_view s) const {
        uint32_t h = 2166136261u;
        for (unsigned char c : s) {
            h ^= c;
            h *= 16777619u;
        }
        return h;
    }
};

struct Fast {
    static constexpr const char* kName = "fast";
    uint32_t operator()(std::string_view s) const {
        const char* p = s.data();
        size_t n = s.size();
        uint64_t h = 0x9E3779B97F4A7C15ull ^ (static_cast<uint64_t>(n) * 0xC2B2AE3D27D4EB4Full);
        while (n >= 8) {
            uint64_t v;
            std::memcpy(&v, p, 8);
            h = (h ^ v) * 0x9E3779B97F4A7C15ull;
            h ^= h >> 29;
            p += 8;
            n -= 8;
        }
        uint64_t v = 0;
        if (n) std::memcpy(&v, p, n);
        h = (h ^ v) * 0xBF58476D1CE4E5B9ull;
        h ^= h >> 31;
        h *= 0x94D049BB133111EBull;
        h ^= h >> 32;
        return static_cast<uint32_t>(h);
    }
};

// ---- locking policies -----------------------------------------------------------------------
struct NoLock {
    static constexpr const char* kName = "nolock";
    static constexpr bool kLockFreeView = false;
    struct M {};
    struct Read { explicit Read(M&) {} };
    struct Write { explicit Write(M&) {} };
    static constexpr bool kOptimisticIntern = false;
};

struct MutexLock {
    static constexpr const char* kName = "mutex";
    static constexpr bool kLockFreeView = false;
    using M = std::mutex;
    using Read = std::lock_guard<std::mutex>;
    using Write = std::lock_guard<std::mutex>;
    static constexpr bool kOptimisticIntern = false;
};

struct SharedLock {
    static constexpr const char* kName = "shared";
    static constexpr bool kLockFreeView = false;
    using M = std::shared_mutex;
    using Read = std::shared_lock<std::shared_mutex>;
    using Write = std::unique_lock<std::shared_mutex>;
    static constexpr bool kOptimisticIntern = true;  // look up under a shared lock first
};

struct SharedLockFreeView : SharedLock {
    static constexpr const char* kName = "shared+lfview";
    static constexpr bool kLockFreeView = true;
};

// ---- characters: chunks that never move, addressed by 32-bit refs ---------------------------
// ref = chunk index (10 bits) << 22 | offset (22 bits). Shared chunks are at most 4 MB; a string
// that would not fit gets its own chunk at offset 0, so any length is addressable.
class Arena {
public:
    static constexpr unsigned kOffBits = 22;
    static constexpr size_t kMaxChunk = size_t(1) << kOffBits;
    static constexpr size_t kMaxChunks = 1024;

    explicit Arena(size_t reserve) : next_(std::min(std::max<size_t>(reserve, 64), kMaxChunk)) {
        NewShared();
    }

    // Returns the ref; *text receives the stored pointer.
    uint32_t Store(std::string_view s, const char** text) {
        const size_t need = s.size() + 1;
        char* dst;
        uint32_t ref;
        if (need <= left_) {
            dst = cur_;
            ref = static_cast<uint32_t>((curIdx_ << kOffBits) | (curSize_ - left_));
            cur_ += need;
            left_ -= need;
        } else if (need > next_ / 4) {
            const size_t idx = Add(need);
            dst = chunks_[idx].get();
            ref = static_cast<uint32_t>(idx << kOffBits);
        } else {
            NewShared();
            dst = cur_;
            ref = static_cast<uint32_t>(curIdx_ << kOffBits);
            cur_ += need;
            left_ -= need;
        }
        if (!s.empty()) std::memcpy(dst, s.data(), s.size());
        dst[s.size()] = '\0';
        *text = dst;
        return ref;
    }

    const char* Get(uint32_t ref) const { return chunks_[ref >> kOffBits].get() + (ref & (kMaxChunk - 1)); }
    size_t Allocated() const { return allocated_; }
    size_t Count() const { return count_; }

private:
    size_t Add(size_t bytes) {
        if (count_ == kMaxChunks) throw std::length_error("Arena: chunk limit");
        chunks_[count_].reset(new char[bytes]);
        allocated_ += bytes;
        return count_++;
    }
    void NewShared() {
        curIdx_ = Add(next_);
        cur_ = chunks_[curIdx_].get();
        curSize_ = left_ = next_;
        next_ = std::min(next_ * 2, kMaxChunk);
    }

    std::unique_ptr<char[]> chunks_[kMaxChunks];  // fixed directory: never reallocated (lock-free reads)
    size_t count_ = 0;
    size_t curIdx_ = 0;
    char* cur_ = nullptr;
    size_t curSize_ = 0;
    size_t left_ = 0;
    size_t next_;
    size_t allocated_ = 0;
};

// ---- entry containers -----------------------------------------------------------------------
template <class E>
class VectorStore {
public:
    static constexpr const char* kName = "vec";
    void Push(const E& e) { v_.push_back(e); }
    const E& operator[](uint32_t i) const { return v_[i]; }
    size_t Size() const { return v_.size(); }
    size_t Allocated() const { return v_.capacity() * sizeof(E); }

private:
    std::vector<E> v_;
};

// Segment k holds kBase << k entries; segments are never moved or freed, so a reference to an
// entry stays valid while others are appended (no copy on growth, lock-free readers possible).
template <class E>
class SegmentedStore {
public:
    static constexpr const char* kName = "seg";
    static constexpr unsigned kBaseBits = 8;  // first segment: 256 entries

    void Push(const E& e) {
        unsigned k;
        uint32_t off;
        Locate(static_cast<uint32_t>(size_), k, off);
        if (!seg_[k]) {
            seg_[k].reset(new E[size_t(1) << (kBaseBits + k)]);
            allocated_ += (size_t(1) << (kBaseBits + k)) * sizeof(E);
        }
        seg_[k][off] = e;
        ++size_;
    }
    const E& operator[](uint32_t i) const {
        unsigned k;
        uint32_t off;
        Locate(i, k, off);
        return seg_[k][off];
    }
    size_t Size() const { return size_; }
    size_t Allocated() const { return allocated_; }

private:
    static void Locate(uint32_t i, unsigned& k, uint32_t& off) {
        k = HighBit((i >> kBaseBits) + 1);
        off = i - (((1u << k) - 1u) << kBaseBits);
    }
    std::unique_ptr<E[]> seg_[32];
    size_t size_ = 0;
    size_t allocated_ = 0;
};

// ---- the pool -------------------------------------------------------------------------------
enum class Layout { Ptr16, Ref12, Ref8 };

template <Layout L> struct EntryOf;
template <> struct EntryOf<Layout::Ptr16> { const char* text; uint32_t len; uint32_t hash; };
template <> struct EntryOf<Layout::Ref12> { uint32_t ref; uint32_t len; uint32_t hash; };
template <> struct EntryOf<Layout::Ref8> { uint32_t ref; uint32_t len; };

struct BucketSlot { uint32_t slot; };
struct BucketSlotHash { uint32_t slot; uint32_t hash; };

template <Layout L, bool kHashInTable, unsigned kLoadPct, class Hash, template <class> class Store, class Lock>
class Pool {
    static_assert(L != Layout::Ref8 || kHashInTable, "Ref8 keeps the hash only in the table");
    static_assert(!Lock::kLockFreeView || std::is_same<Store<EntryOf<L>>, SegmentedStore<EntryOf<L>>>::value,
                  "lock-free View needs entries that never move");

public:
    using Entry = EntryOf<L>;
    using Bucket = std::conditional_t<kHashInTable, BucketSlotHash, BucketSlot>;

    explicit Pool(size_t reserve = 64 * 1024) : arena_(reserve) { table_.assign(1024, Empty()); }

    uint32_t Find(std::string_view s) const {
        const uint32_t h = Hash()(s);
        typename Lock::Read lk(m_);
        return FindLocked(s, h);
    }

    uint32_t Intern(std::string_view s) {
        const uint32_t h = Hash()(s);
        if constexpr (Lock::kOptimisticIntern) {
            typename Lock::Read lk(m_);
            const uint32_t f = FindLocked(s, h);
            if (f != kNone) return f;
        }
        typename Lock::Write lk(m_);
        const uint32_t f = FindLocked(s, h);
        if (f != kNone) return f;
        if ((entries_.Size() + 1) * 100 > table_.size() * kLoadPct) Rehash(table_.size() * 2);
        const uint32_t slot = static_cast<uint32_t>(entries_.Size());
        const char* text;
        const uint32_t ref = arena_.Store(s, &text);
        Entry e;
        if constexpr (L == Layout::Ptr16) e = Entry{text, static_cast<uint32_t>(s.size()), h};
        else if constexpr (L == Layout::Ref12) e = Entry{ref, static_cast<uint32_t>(s.size()), h};
        else e = Entry{ref, static_cast<uint32_t>(s.size())};
        (void)ref;
        (void)text;
        entries_.Push(e);
        Place(slot, h);
        return slot;
    }

    std::string_view View(uint32_t slot) const {
        if constexpr (Lock::kLockFreeView) {
            return ViewLocked(slot);
        } else {
            typename Lock::Read lk(m_);
            return ViewLocked(slot);
        }
    }

    size_t Count() const { return entries_.Size(); }
    size_t Allocated() const {  // heap bytes of every array (the fixed chunk directory is inside the object)
        return arena_.Allocated() + entries_.Allocated() + table_.capacity() * sizeof(Bucket);
    }

private:
    static Bucket Empty() {
        Bucket b{};
        b.slot = kNone;
        return b;
    }

    std::string_view ViewLocked(uint32_t slot) const {
        const Entry& e = entries_[slot];
        return std::string_view(Text(e), e.len);
    }

    const char* Text(const Entry& e) const {
        if constexpr (L == Layout::Ptr16) return e.text;
        else return arena_.Get(e.ref);
    }

    uint32_t FindLocked(std::string_view s, uint32_t h) const {
        const size_t mask = table_.size() - 1;
        for (size_t i = h & mask;; i = (i + 1) & mask) {
            const Bucket& b = table_[i];
            if (b.slot == kNone) return kNone;
            if constexpr (kHashInTable) {
                if (b.hash != h) continue;
                const Entry& e = entries_[b.slot];
                if (e.len == s.size() && (s.empty() || std::memcmp(Text(e), s.data(), s.size()) == 0)) return b.slot;
            } else {
                const Entry& e = entries_[b.slot];
                if (e.hash == h && e.len == s.size() && (s.empty() || std::memcmp(Text(e), s.data(), s.size()) == 0))
                    return b.slot;
            }
        }
    }

    void Place(uint32_t slot, uint32_t h) {
        const size_t mask = table_.size() - 1;
        size_t i = h & mask;
        while (table_[i].slot != kNone) i = (i + 1) & mask;
        if constexpr (kHashInTable) table_[i] = Bucket{slot, h};
        else table_[i] = Bucket{slot};
    }

    void Rehash(size_t newSize) {
        std::vector<Bucket> old;
        if constexpr (kHashInTable) old.swap(table_);
        table_.assign(newSize, Empty());
        if constexpr (kHashInTable) {
            for (const Bucket& b : old)
                if (b.slot != kNone) Place(b.slot, b.hash);
        } else {
            for (uint32_t s = 0; s < entries_.Size(); ++s) Place(s, entries_[s].hash);
        }
    }

    Arena arena_;
    Store<Entry> entries_;
    std::vector<Bucket> table_;
    mutable typename Lock::M m_;
};

}  // namespace pv
