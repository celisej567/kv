// String-pool driver for tests/run_tests.py. Built twice from this file:
//   kv_stress     linked against kv_mt (KV_THREAD_SAFE): all commands
//   kv_stress_st  linked against kv (plain build):       poolmem, poolperf
// It replaces the global operator new with a counting one, which is why it is separate from
// kv_cli: kv_cli must keep the normal allocator so its timings and RSS numbers are not skewed.
//
//   kv_stress mtload <file> <threads>
//       <threads> threads load <file> at the same moment into their own trees while one more thread
//       keeps adding unrelated strings (forcing the pool to grow during the loads). On success
//       stdout is the dump (same format as `kv_cli dump`), exit 0. If every thread failed: exit 1
//       and stderr has the parser messages, like kv_cli. If threads disagree: exit 3.
//   kv_stress mt <file> <threads> <unique> <shared>
//       Threads load <file> into their own trees, read a shared tree loaded beforehand, and intern
//       overlapping ("shared") and private ("unique", >= 1) strings into the global pool.
//   kv_stress poolmem <strings> <length> <rounds>
//       Interns <strings> distinct strings of <length> chars, <rounds> times (rounds > 1 only
//       re-interns existing strings), into a private pool and prints the pool's own accounting
//       next to the real heap usage measured by the counting operator new.
//   kv_stress poolperf <strings> <threads>
//       Speed of a private pool: single-thread ns/op and multi-thread throughput (pool_perf.h).
// Except for mtload every command prints "name value" lines.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "counting_alloc.h"
#include "keyvalue.h"
#include "keyvalue_internal.h"
#include "kv_dump.h"
#include "pool_perf.h"

// --- poolmem -------------------------------------------------------------------------------------
static std::string MakeString(size_t i, size_t len) {
    std::string s = std::to_string(i);
    if (s.size() < len) s.insert(0, len - s.size(), '0');
    return s;
}

static void PrintStats(const char* prefix, const KVStringPool::Stats& st) {
    std::printf("%sstrings %zu\n", prefix, st.strings);
    std::printf("%schars %zu\n", prefix, st.chars);
    std::printf("%schunks %zu\n", prefix, st.chunks);
    std::printf("%schars_allocated %zu\n", prefix, st.charsAllocated);
    std::printf("%sentries_allocated %zu\n", prefix, st.entriesAllocated);
    std::printf("%stable_allocated %zu\n", prefix, st.tableAllocated);
    std::printf("%sallocated %zu\n", prefix, st.allocated);
    std::printf("%sused %zu\n", prefix, st.used);
    std::printf("%sdirectory %zu\n", prefix, st.directory);
}

static int PoolMem(size_t n, size_t len, size_t rounds) {
    // The pool object lives on the heap so the heap delta covers it too; the counting operator new
    // is the independent check that the pool's Stats really cover every array it owns.
    const size_t liveBefore = counting_alloc::Live();
    counting_alloc::ResetPeak();
    auto pool = std::make_unique<KVStringPool>();
    PrintStats("empty_", pool->GetStats());
    std::printf("empty_heap %zu\n", counting_alloc::Live() - liveBefore);
    std::printf("object_size %zu\n", sizeof(KVStringPool));

    bool ok = true;
    KVStringPool::Stats afterFirst;
    std::string s;
    for (size_t r = 0; r < rounds; ++r) {
        for (size_t i = 0; i < n; ++i) {
            s = MakeString(i, len);
            if (pool->Intern(s) != i) ok = false;  // slots follow insertion order; re-interning returns the same
        }
        if (r == 0) afterFirst = pool->GetStats();
    }
    for (size_t i = 0; i < n && ok; i += 1 + n / 1000)  // the content survived all the growth
        if (pool->View(static_cast<uint32_t>(i)) != MakeString(i, len)) ok = false;
    std::string().swap(s);  // release the scratch buffer so heap_live is the pool alone (= keeps capacity)

    const KVStringPool::Stats st = pool->GetStats();
    PrintStats("", st);
    std::printf("first_round_allocated %zu\n", afterFirst.allocated);
    std::printf("heap_live %zu\n", counting_alloc::Live() - liveBefore);
    std::printf("heap_peak %zu\n", counting_alloc::Peak() - liveBefore);
    std::printf("content_ok %d\n", ok ? 1 : 0);
    return ok ? 0 : 1;
}

#ifdef KV_THREAD_SAFE
// --- mtload --------------------------------------------------------------------------------------
static int MtLoad(const char* file, int threads) {
    std::vector<std::string> dumps(static_cast<size_t>(threads));
    std::vector<char> ok(static_cast<size_t>(threads), 0);
    std::atomic<int> ready{0}, done{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&, t] {
            CKeyValue kv("Unnamed");
            ready.fetch_add(1);
            while (!go.load()) {}
            ok[static_cast<size_t>(t)] = KvLoad(kv, file) ? 1 : 0;
            dumps[static_cast<size_t>(t)] = KvDump(kv);
            done.fetch_add(1);
        });
    // Noise: new strings while the loaders run, so chunks, entries and the table grow under them.
    std::thread noise([&] {
        ready.fetch_add(1);
        while (!go.load()) {}
        KVStringPool& pool = KVStringPool::Instance();
        for (unsigned i = 0; done.load() < threads || i < 2000; ++i) pool.Intern("mtload_noise_" + std::to_string(i));
    });
    while (ready.load() != threads + 1) {}
    go.store(true);
    for (auto& th : ts) th.join();
    noise.join();

    int okCount = 0;
    for (char c : ok) okCount += c;
    for (int t = 1; t < threads; ++t)
        if (dumps[static_cast<size_t>(t)] != dumps[0]) {
            std::fprintf(stderr, "mtload: thread %d built a different tree than thread 0\n", t);
            return 3;
        }
    if (okCount == threads) {
        std::fputs(dumps[0].c_str(), stdout);
        return 0;
    }
    if (okCount == 0) return 1;
    std::fprintf(stderr, "mtload: %d of %d threads loaded the file\n", okCount, threads);
    return 3;
}

// --- mt ------------------------------------------------------------------------------------------
struct Scalar {
    const CKeyValue* owner;
    std::string name, value;
};

static void CollectScalars(const CKeyValue& kv, std::vector<Scalar>& out) {
    KVStringPool& pool = KVStringPool::Instance();
    for (const auto& n : KVInternal::Nodes(kv)) {
        if (n.type == CKeyValue::KeyValue) CollectScalars(*n.child, out);
        else out.push_back({&kv, std::string(pool.View(n.nameSlot)), std::string(pool.View(n.valueSlot))});
    }
}

struct Worker {
    std::string dump;
    std::vector<uint32_t> shared;
    std::vector<uint32_t> unique;
    long errors = 0;
};

static int MultiThread(const char* file, int threads, int unique, int shared) {
    if (threads < 1 || unique < 1 || shared < 0) return 2;
    KVStringPool& pool = KVStringPool::Instance();
    CKeyValue ref("ref");
    if (!KvLoad(ref, file)) return 1;
    const std::string refDump = KvDump(ref);

    // Every scalar of the shared tree, at any depth; readers keep checking them during the inserts.
    std::vector<Scalar> expect;
    CollectScalars(ref, expect);

    pool.Intern("mine");  // the name every worker's tree gets; keep it out of the count below
    const size_t before = pool.Count();
    std::vector<Worker> w(static_cast<size_t>(threads));
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t) {
        ts.emplace_back([&, t] {
            Worker& me = w[static_cast<size_t>(t)];
            me.shared.assign(static_cast<size_t>(shared), kInvalidSlot);
            me.unique.assign(static_cast<size_t>(unique), kInvalidSlot);
            while (!go.load()) {}

            // A pointer taken early must still be valid after the pool has grown many times.
            const std::string firstName = "mt_t" + std::to_string(t) + "_0";
            const uint32_t firstSlot = pool.Intern(firstName);
            const char* firstPtr = pool.CStr(firstSlot);
            const std::string_view firstView = pool.View(firstSlot);

            CKeyValue mine("mine");
            if (!KvLoad(mine, file)) ++me.errors;
            me.dump = KvDump(mine);

            size_t reader = static_cast<size_t>(t);
            for (int k = 0; k < shared; ++k) {
                // Shared strings in a different (rotated) order per thread -> inserts of the same
                // string race with each other.
                const int i = (k + t * 37) % shared;
                me.shared[static_cast<size_t>(i)] = pool.Intern("mt_shared_" + std::to_string(i));
                if (!expect.empty() && k % 16 == 0) {  // read the shared tree while others insert
                    const Scalar& e = expect[reader++ % expect.size()];
                    if (e.owner->GetString(e.name.c_str(), "\x01missing") != e.value) ++me.errors;
                }
            }
            for (int i = 0; i < unique; ++i)
                me.unique[static_cast<size_t>(i)] = pool.Intern("mt_t" + std::to_string(t) + "_" + std::to_string(i));

            if (std::string_view(firstPtr) != firstName || firstView != firstName) ++me.errors;
        });
    }
    go.store(true);
    for (auto& th : ts) th.join();

    long errors = 0, dumpMismatch = 0, slotMismatch = 0, contentMismatch = 0;
    for (int t = 0; t < threads; ++t) {
        const Worker& me = w[static_cast<size_t>(t)];
        errors += me.errors;
        if (me.dump != refDump) ++dumpMismatch;
        for (int i = 0; i < shared; ++i) {
            if (me.shared[static_cast<size_t>(i)] != w[0].shared[static_cast<size_t>(i)]) ++slotMismatch;
            if (pool.View(me.shared[static_cast<size_t>(i)]) != "mt_shared_" + std::to_string(i)) ++contentMismatch;
        }
        for (int i = 0; i < unique; ++i)
            if (pool.View(me.unique[static_cast<size_t>(i)]) != "mt_t" + std::to_string(t) + "_" + std::to_string(i)) ++contentMismatch;
    }
    const size_t added = pool.Count() - before;
    // "mt_t<tid>_0" is the first of the unique range (interned twice), so it adds nothing extra.
    const size_t expected = static_cast<size_t>(shared) + static_cast<size_t>(threads) * static_cast<size_t>(unique);

    std::printf("threads %d\n", threads);
    std::printf("scalars_checked %zu\n", expect.size());
    std::printf("errors %ld\n", errors);
    std::printf("dump_mismatch %ld\n", dumpMismatch);
    std::printf("slot_mismatch %ld\n", slotMismatch);
    std::printf("content_mismatch %ld\n", contentMismatch);
    std::printf("new_strings %zu\n", added);
    std::printf("expected_new_strings %zu\n", expected);
    return (errors || dumpMismatch || slotMismatch || contentMismatch || added != expected) ? 1 : 0;
}

#endif  // KV_THREAD_SAFE

int main(int argc, char** argv) {
    CKeyValue::SetCondition("KVTRUE", true);  // same as kv_cli: true on every platform
    const std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "info") {
#ifdef KV_THREAD_SAFE
        std::printf("thread_safe 1\n");
#else
        std::printf("thread_safe 0\n");
#endif
        return 0;
    }
#ifdef KV_THREAD_SAFE
    if (cmd == "mtload" && argc == 4) return MtLoad(argv[2], std::atoi(argv[3]));
    if (cmd == "mt" && argc == 6) return MultiThread(argv[2], std::atoi(argv[3]), std::atoi(argv[4]), std::atoi(argv[5]));
#endif
    if (cmd == "poolmem" && argc == 5)
        return PoolMem(std::strtoul(argv[2], nullptr, 10), std::strtoul(argv[3], nullptr, 10), std::strtoul(argv[4], nullptr, 10));
    if (cmd == "poolperf" && argc == 4) {
        pool_perf::Run(std::strtoul(argv[2], nullptr, 10), std::atoi(argv[3]));
        return 0;
    }
    return 2;
}
