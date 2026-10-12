// Compares the real KVStringPool against the alternative designs in pool_variants.h.
//   kv_pool_bench [st|mt|all] [scale]
// st: single-thread speed + exact heap usage (counting operator new), mt: lock policies under threads.
// `scale` (default 1.0) multiplies every workload size. Prints markdown tables.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "../tools/counting_alloc.h"
#include "kv_string_pool.h"
#include "pool_variants.h"

namespace {

using Clock = std::chrono::steady_clock;
double NsSince(Clock::time_point t) { return std::chrono::duration<double, std::nano>(Clock::now() - t).count(); }

volatile size_t g_sink;

struct RealPool {
    static constexpr const char* kName = "current (KVStringPool)";
    KVStringPool p;
    uint32_t Intern(std::string_view s) { return p.Intern(s); }
    uint32_t Find(std::string_view s) const { return p.Find(s); }
    std::string_view View(uint32_t slot) const { return p.View(slot); }
    size_t Allocated() const { return p.MemoryUsage(); }
};

// ---- data -----------------------------------------------------------------------------------
struct Data {
    std::vector<std::string> keys;     // short unique, config-key like ("key_123456")
    std::vector<std::string> paths;    // long unique, path like (40-60 chars)
    std::vector<std::string> misses;   // never interned
    std::vector<std::string> vocab;    // small vocabulary for the duplicate-heavy workload
    std::vector<uint32_t> dupSeq;      // indices into vocab
    std::vector<uint32_t> randKeys;    // random indices into keys
};

uint32_t Lcg(uint64_t& s) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<uint32_t>(s >> 33);
}

Data MakeData(double scale) {
    Data d;
    const size_t nKeys = static_cast<size_t>(1'000'000 * scale), nPaths = static_cast<size_t>(300'000 * scale);
    const size_t nVocab = 50'000, nDup = static_cast<size_t>(2'000'000 * scale), nRand = static_cast<size_t>(2'000'000 * scale);
    char buf[128];
    for (size_t i = 0; i < nKeys; ++i) d.keys.push_back("key_" + std::to_string(i));
    for (size_t i = 0; i < nPaths; ++i) {
        std::snprintf(buf, sizeof buf, "materials/models/props_%zu/texture_diffuse_%08x.vtf", i % 977, static_cast<unsigned>(i * 2654435761u));
        d.paths.push_back(buf);
    }
    for (size_t i = 0; i < nRand; ++i) d.misses.push_back("nokey_" + std::to_string(i));
    for (size_t i = 0; i < nVocab; ++i) d.vocab.push_back("word" + std::to_string(i * 31 % 100003));
    uint64_t s = 42;
    for (size_t i = 0; i < nDup; ++i) d.dupSeq.push_back(Lcg(s) % nVocab);
    for (size_t i = 0; i < nRand; ++i) d.randKeys.push_back(static_cast<uint32_t>(Lcg(s) % nKeys));
    return d;
}

// ---- single thread --------------------------------------------------------------------------
struct StResult {
    double internKeys = 1e300, internPaths = 1e300, internDups = 1e300, findHit = 1e300, findMiss = 1e300, view = 1e300;  // ns/op
    size_t bytesKeys = 0, bytesPaths = 0, peakKeys = 0;  // heap bytes incl. the pool object itself
};

template <class P>
StResult RunSt(const Data& d, int reps) {
    StResult r;
    for (int rep = 0; rep < reps; ++rep) {
        {  // short unique keys, then lookups on that pool
            const size_t before = counting_alloc::Live();
            counting_alloc::ResetPeak();
            auto t = Clock::now();
            auto p = std::make_unique<P>();
            for (const auto& k : d.keys) p->Intern(k);
            r.internKeys = std::min(r.internKeys, NsSince(t) / d.keys.size());
            r.bytesKeys = counting_alloc::Live() - before;
            r.peakKeys = counting_alloc::Peak() - before;

            size_t sink = 0;
            t = Clock::now();
            for (uint32_t i : d.randKeys) sink += p->Find(d.keys[i]);
            r.findHit = std::min(r.findHit, NsSince(t) / d.randKeys.size());
            t = Clock::now();
            for (const auto& m : d.misses) sink += p->Find(m);
            r.findMiss = std::min(r.findMiss, NsSince(t) / d.misses.size());
            t = Clock::now();
            for (uint32_t i : d.randKeys) sink += p->View(i).size();
            r.view = std::min(r.view, NsSince(t) / d.randKeys.size());
            g_sink = sink;
        }
        {
            const size_t before = counting_alloc::Live();
            auto t = Clock::now();
            auto p = std::make_unique<P>();
            for (const auto& k : d.paths) p->Intern(k);
            r.internPaths = std::min(r.internPaths, NsSince(t) / d.paths.size());
            r.bytesPaths = counting_alloc::Live() - before;
        }
        {
            auto t = Clock::now();
            auto p = std::make_unique<P>();
            for (uint32_t i : d.dupSeq) p->Intern(d.vocab[i]);
            r.internDups = std::min(r.internDups, NsSince(t) / d.dupSeq.size());
        }
    }
    return r;
}

template <class P>
void RowSt(const char* name, const Data& d, int reps) {
    const StResult r = RunSt<P>(d, reps);
    std::printf("| %-34s | %6.1f | %6.1f | %5.2f | %6.1f | %6.1f | %6.1f | %6.1f | %6.1f | %6.1f |\n", name,
                double(r.bytesKeys) / d.keys.size(), double(r.bytesPaths) / d.paths.size(), double(r.peakKeys) / r.bytesKeys,
                r.internKeys, r.internPaths, r.internDups, r.findHit, r.findMiss, r.view);
    std::fflush(stdout);
}

// ---- multi thread ---------------------------------------------------------------------------
template <class P, class Fn>
double TimeThreads(int threads, Fn fn) {
    std::atomic<int> ready{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&, t] {
            ready.fetch_add(1);
            while (!go.load()) {}
            fn(t);
        });
    while (ready.load() != threads) {}
    const auto start = Clock::now();
    go.store(true);
    for (auto& th : ts) th.join();
    return NsSince(start);
}

template <class P>
void RowMt(const char* name, const Data& d, const std::vector<int>& threadCounts) {
    const size_t opsPerThread = d.randKeys.size() / 2;
    for (int threads : threadCounts) {
        // read: Find + View of existing strings (what GetString does)
        P read;
        for (const auto& k : d.keys) read.Intern(k);
        double ns = TimeThreads<P>(threads, [&](int t) {
            size_t sink = 0;
            for (size_t i = 0; i < opsPerThread; ++i) {
                const uint32_t slot = read.Find(d.keys[d.randKeys[(i + t * 7919) % d.randKeys.size()]]);
                sink += read.View(slot).size();
            }
            g_sink = sink;
        });
        const double readMops = threads * opsPerThread / ns * 1e3;

        // dup: every thread interns the same vocabulary (threads loading the same file)
        P dup;
        ns = TimeThreads<P>(threads, [&](int t) {
            for (size_t i = 0; i < opsPerThread; ++i) dup.Intern(d.vocab[d.dupSeq[(i + t * 104729) % d.dupSeq.size()]]);
        });
        const double dupMops = threads * opsPerThread / ns * 1e3;

        // unique: every thread interns its own strings
        P uniq;
        const size_t perThread = d.keys.size() / threads;
        ns = TimeThreads<P>(threads, [&](int t) {
            for (size_t i = 0; i < perThread; ++i) uniq.Intern(d.keys[t * perThread + i]);
        });
        const double uniqMops = threads * perThread / ns * 1e3;

        std::printf("| %-38s | %3d | %7.1f | %7.1f | %7.1f |\n", name, threads, readMops, dupMops, uniqMops);
        std::fflush(stdout);
    }
}

}  // namespace

int main(int argc, char** argv) {
    using namespace pv;
    const std::string mode = argc > 1 ? argv[1] : "all";
    const double scale = argc > 2 ? std::atof(argv[2]) : 1.0;
    const Data d = MakeData(scale);

    if (mode == "st" || mode == "all") {
        std::printf("\n## Single thread (%zu short keys, %zu paths, %zu dup interns; best of 3)\n\n", d.keys.size(), d.paths.size(), d.dupSeq.size());
        std::printf("| variant                            | B/key  | B/path | peak  | int.key| int.pth| int.dup| find   | miss   | view   |\n");
        std::printf("|------------------------------------|--------|--------|-------|--------|--------|--------|--------|--------|--------|\n");
        RowSt<RealPool>("current (KVStringPool)", d, 3);
        RowSt<Pool<Layout::Ptr16, false, 50, Fnv1a, VectorStore, NoLock>>("ptr16 l50 vec (= current)", d, 3);
        RowSt<Pool<Layout::Ref12, false, 50, Fnv1a, VectorStore, NoLock>>("ref12 l50 vec", d, 3);
        RowSt<Pool<Layout::Ref12, false, 75, Fnv1a, VectorStore, NoLock>>("ref12 l75 vec", d, 3);
        RowSt<Pool<Layout::Ref8, true, 50, Fnv1a, VectorStore, NoLock>>("ref8 hash-in-table l50 vec", d, 3);
        RowSt<Pool<Layout::Ref8, true, 75, Fnv1a, VectorStore, NoLock>>("ref8 hash-in-table l75 vec", d, 3);
        RowSt<Pool<Layout::Ref12, false, 50, Fnv1a, SegmentedStore, NoLock>>("ref12 l50 seg", d, 3);
        RowSt<Pool<Layout::Ref12, false, 75, Fnv1a, SegmentedStore, NoLock>>("ref12 l75 seg", d, 3);
        RowSt<Pool<Layout::Ref12, false, 75, Fast, SegmentedStore, NoLock>>("ref12 l75 seg, fast hash", d, 3);
        std::printf("\nB/key, B/path: heap bytes per string (all arrays + pool object). peak: peak/final heap while building.\n"
                    "Other columns: ns per operation. Hash is FNV-1a unless noted.\n");
    }
    if (mode == "mt" || mode == "all") {
        const std::vector<int> tc = {1, 4, 8, 16};
        std::printf("\n## Threads (Mops/s total; read = Find+View, dup = shared vocabulary, unique = own strings)\n\n");
        std::printf("| variant                                | thr | read    | dup     | unique  |\n");
        std::printf("|----------------------------------------|-----|---------|---------|---------|\n");
        RowMt<Pool<Layout::Ptr16, false, 50, Fnv1a, VectorStore, MutexLock>>("ptr16 l50 vec, mutex (= current MT)", d, tc);
        RowMt<Pool<Layout::Ref12, false, 75, Fnv1a, SegmentedStore, MutexLock>>("ref12 l75 seg, mutex", d, tc);
        RowMt<Pool<Layout::Ref12, false, 75, Fnv1a, SegmentedStore, SharedLock>>("ref12 l75 seg, shared_mutex", d, tc);
        RowMt<Pool<Layout::Ref12, false, 75, Fnv1a, SegmentedStore, SharedLockFreeView>>("ref12 l75 seg, shared + lock-free View", d, tc);
    }
    return 0;
}
