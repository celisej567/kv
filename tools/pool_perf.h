#pragma once
// Speed of the real KVStringPool, shared by kv_cli (plain build: single-thread numbers) and
// kv_stress (KV_THREAD_SAFE build: single-thread numbers with locking + thread scaling).
// Prints "name value" lines; timings are best of 3 on private pools.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "kv_string_pool.h"

namespace pool_perf {

using Clock = std::chrono::steady_clock;
inline double NsSince(Clock::time_point t) { return std::chrono::duration<double, std::nano>(Clock::now() - t).count(); }
inline volatile size_t g_sink;

inline uint32_t Lcg(uint64_t& s) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return static_cast<uint32_t>(s >> 33);
}

struct Data {
    std::vector<std::string> keys, misses, vocab;
    std::vector<uint32_t> randKeys, dupSeq;
};

inline Data Make(size_t n) {
    Data d;
    for (size_t i = 0; i < n; ++i) d.keys.push_back("key_" + std::to_string(i));
    for (size_t i = 0; i < n; ++i) d.misses.push_back("nokey_" + std::to_string(i));
    for (size_t i = 0; i < 20000; ++i) d.vocab.push_back("word" + std::to_string(i * 31 % 100003));
    uint64_t s = 7;
    for (size_t i = 0; i < 2 * n; ++i) d.randKeys.push_back(Lcg(s) % static_cast<uint32_t>(n));
    for (size_t i = 0; i < 2 * n; ++i) d.dupSeq.push_back(Lcg(s) % static_cast<uint32_t>(d.vocab.size()));
    return d;
}

// Runs fn(t) on `threads` threads started together; returns wall time in ns.
template <class Fn>
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

inline void Run(size_t n, int threads) {
    const Data d = Make(n);
    double internUnique = 1e300, internDup = 1e300, findHit = 1e300, findMiss = 1e300, view = 1e300;
    for (int rep = 0; rep < 3; ++rep) {
        auto p = std::make_unique<KVStringPool>();
        auto t = Clock::now();
        for (const auto& k : d.keys) p->Intern(k);
        internUnique = std::min(internUnique, NsSince(t) / n);
        size_t sink = 0;
        t = Clock::now();
        for (uint32_t i : d.randKeys) sink += p->Find(d.keys[i]);
        findHit = std::min(findHit, NsSince(t) / d.randKeys.size());
        t = Clock::now();
        for (const auto& m : d.misses) sink += p->Find(m);
        findMiss = std::min(findMiss, NsSince(t) / d.misses.size());
        t = Clock::now();
        for (uint32_t i : d.randKeys) sink += p->View(i).size();
        view = std::min(view, NsSince(t) / d.randKeys.size());

        auto q = std::make_unique<KVStringPool>();
        t = Clock::now();
        for (uint32_t i : d.dupSeq) sink += q->Intern(d.vocab[i]);
        internDup = std::min(internDup, NsSince(t) / d.dupSeq.size());
        g_sink = sink;
    }
    std::printf("intern_unique_ns %.1f\nintern_dup_ns %.1f\nfind_hit_ns %.1f\nfind_miss_ns %.1f\nview_ns %.1f\n",
                internUnique, internDup, findHit, findMiss, view);

#ifdef KV_THREAD_SAFE
    // Throughput (million ops/s, all threads together) at 1 thread and at `threads` threads.
    const size_t ops = d.randKeys.size() / 2;
    KVStringPool read;
    for (const auto& k : d.keys) read.Intern(k);
    for (int tc : {1, threads}) {
        double readBest = 0, dupBest = 0, uniqueBest = 0;
        for (int rep = 0; rep < 3; ++rep) {
            double ns = TimeThreads(tc, [&](int t) {  // GetString: Find + View
                size_t sink = 0;
                for (size_t i = 0; i < ops; ++i) {
                    const std::string& k = d.keys[d.randKeys[(i + static_cast<size_t>(t) * 7919) % d.randKeys.size()]];
                    sink += read.View(read.Find(k)).size();
                }
                g_sink = sink;
            });
            readBest = std::max(readBest, tc * ops / ns * 1e3);

            KVStringPool dup;  // threads loading the same file: mostly existing strings
            ns = TimeThreads(tc, [&](int t) {
                for (size_t i = 0; i < ops; ++i) dup.Intern(d.vocab[d.dupSeq[(i + static_cast<size_t>(t) * 104729) % d.dupSeq.size()]]);
            });
            dupBest = std::max(dupBest, tc * ops / ns * 1e3);

            KVStringPool uniq;  // threads loading different files: every string is new
            const size_t per = n / static_cast<size_t>(tc);
            ns = TimeThreads(tc, [&](int t) {
                for (size_t i = 0; i < per; ++i) uniq.Intern(d.keys[static_cast<size_t>(t) * per + i]);
            });
            uniqueBest = std::max(uniqueBest, tc * per / ns * 1e3);
        }
        const char* suffix = tc == 1 ? "1" : "n";
        std::printf("read_mops_%s %.2f\ndup_mops_%s %.2f\nunique_mops_%s %.2f\n", suffix, readBest, suffix, dupBest, suffix, uniqueBest);
    }
    std::printf("threads %d\n", threads);
#else
    (void)threads;
#endif
}

}  // namespace pool_perf
