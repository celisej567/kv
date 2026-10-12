// Same benchmark, two backends: Source Engine KeyValues (-DBACKEND_SOURCE) and this library
// (-DBACKEND_OURS). Build with run_compare.py. Every command prints "name value" lines.
//
//   load   <file> <reps>            file -> tree, best of reps; ms and MB/s, plus tree destruction
//   save   <file> <out> <reps>      tree -> file, best of reps
//   mem    <file>                   heap and RSS cost of one loaded tree (run in a fresh process)
//   lookup <file> <n>               top-level lookups by name (random keys) and small-object lookups
//   mt     <file> <threads> <reps>  every thread loads the file reps times into its own tree
#include <malloc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(BACKEND_SOURCE)
#include "KeyValues.h"
#include "tier1/utlbuffer.h"
using Node = KeyValues;
#elif defined(BACKEND_OURS)
#include "keyvalue.h"
using Node = CKeyValue;
#else
#error define BACKEND_SOURCE or BACKEND_OURS
#endif

namespace {

using Clock = std::chrono::steady_clock;
double MsSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// ---- backend adapter ------------------------------------------------------------------------
#if defined(BACKEND_SOURCE)
const char* kBackend = "source";

std::vector<char> ReadAll(const char* path) {
    std::vector<char> data;
    FILE* f = std::fopen(path, "rb");
    if (!f) return data;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    data.resize(static_cast<size_t>(size) + 1);
    if (std::fread(data.data(), 1, static_cast<size_t>(size), f) != static_cast<size_t>(size)) data.clear();
    std::fclose(f);
    if (!data.empty()) data[static_cast<size_t>(size)] = '\0';
    return data;
}

// KeyValues::LoadFromBuffer uses a process-wide static token buffer (s_pTokenBuf in KeyValues.cpp), so
// two threads parsing at once corrupt each other's tokens (observed: glibc strtod assertion, garbage
// values). The only safe way to use it from several threads is to serialise the parsing. Setting
// KV_SOURCE_UNLOCKED=1 skips the lock to demonstrate the failure.
std::mutex g_parseMutex;
const bool g_unlocked = std::getenv("KV_SOURCE_UNLOCKED") != nullptr;

Node* Load(const char* path) {
    std::vector<char> text = ReadAll(path);  // what Source does under the hood: read it all, parse the buffer
    if (text.empty()) return nullptr;
    Node* kv = new KeyValues("tmp");
    std::unique_lock<std::mutex> lock(g_parseMutex, std::defer_lock);
    if (!g_unlocked) lock.lock();
    if (!kv->LoadFromBuffer("bench", text.data())) {
        kv->deleteThis();
        return nullptr;
    }
    return kv;
}

bool Save(Node* kv, const char* path) {
    CUtlBuffer buf(0, 0, CUtlBuffer::TEXT_BUFFER);
    kv->RecursiveSaveToFile(buf, 0);  // `"name" { children }`; scalar children are written by their parent
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    const bool ok = std::fwrite(buf.Base(), 1, static_cast<size_t>(buf.TellPut()), f) == static_cast<size_t>(buf.TellPut());
    return std::fclose(f) == 0 && ok;
}

void Destroy(Node* kv) { kv->deleteThis(); }
Node* Child(Node* kv, const char* name) { return kv->FindKey(name); }
int GetInt(Node* kv, const char* name) { return kv->GetInt(name, -1); }
size_t GetStrLen(Node* kv, const char* name) { return std::strlen(kv->GetString(name, "")); }

#else
const char* kBackend = "ours";

Node* Load(const char* path) {
    Node* kv = new CKeyValue("tmp");
    if (!kv->LoadFromFile(path)) {
        delete kv;
        return nullptr;
    }
    return kv;
}
bool Save(Node* kv, const char* path) { return kv->WriteToFile(path); }
void Destroy(Node* kv) { delete kv; }
Node* Child(Node* kv, const char* name) { return kv->GetKeyValue(name); }
int GetInt(Node* kv, const char* name) { return kv->GetInt(name, -1); }
size_t GetStrLen(Node* kv, const char* name) { return kv->GetString(name, "").size(); }
#endif

volatile size_t g_sink;

long FileSize(const char* path) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return -1;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fclose(f);
    return n;
}

// Bytes malloc currently holds for the program (small arena blocks + mmapped blocks).
size_t HeapInUse() {
    const struct mallinfo2 mi = mallinfo2();
    return mi.uordblks + mi.hblkhd;
}

size_t StatusKb(const char* field) {
    FILE* f = std::fopen("/proc/self/status", "r");
    if (!f) return 0;
    char line[256];
    size_t kb = 0;
    const size_t len = std::strlen(field);
    while (std::fgets(line, sizeof line, f))
        if (std::strncmp(line, field, len) == 0) {
            std::sscanf(line + len, " %zu", &kb);
            break;
        }
    std::fclose(f);
    return kb;
}

// ---- commands -------------------------------------------------------------------------------
int CmdLoad(const char* path, int reps) {
    const double mb = FileSize(path) / (1024.0 * 1024.0);
    double load = 1e300, destroy = 1e300;
    for (int i = 0; i < reps; ++i) {
        auto t = Clock::now();
        Node* kv = Load(path);
        if (!kv) { std::fprintf(stderr, "load failed\n"); return 1; }
        load = std::min(load, MsSince(t));
        t = Clock::now();
        Destroy(kv);
        destroy = std::min(destroy, MsSince(t));
    }
    std::printf("load_ms %.2f\nload_mbps %.1f\ndestroy_ms %.2f\n", load, mb / (load / 1000), destroy);
    return 0;
}

int CmdSave(const char* path, const char* out, int reps) {
    Node* kv = Load(path);
    if (!kv) return 1;
    double best = 1e300;
    for (int i = 0; i < reps; ++i) {
        auto t = Clock::now();
        if (!Save(kv, out)) return 1;
        best = std::min(best, MsSince(t));
    }
    const double mb = FileSize(out) / (1024.0 * 1024.0);
    std::printf("save_ms %.2f\nsave_mbps %.1f\nsaved_bytes %ld\n", best, mb / (best / 1000), FileSize(out));
    return 0;
}

int CmdMem(const char* path) {
    // Fresh process: nothing loaded before. The baseline is taken after the library's own start-up.
    const size_t heap0 = HeapInUse();
    const size_t rss0 = StatusKb("VmRSS:");
    Node* kv = Load(path);
    if (!kv) return 1;
    const size_t heap1 = HeapInUse();
    const size_t rss1 = StatusKb("VmRSS:");
    const size_t hwm = StatusKb("VmHWM:");
    const long file = FileSize(path);
    std::printf("file_bytes %ld\n", file);
    std::printf("heap_bytes %zu\n", heap1 - heap0);
    std::printf("heap_over_file %.2f\n", double(heap1 - heap0) / file);
    std::printf("rss_bytes %zu\n", (rss1 - rss0) * 1024);
    std::printf("peak_rss_bytes %zu\n", (hwm - rss0) * 1024);  // includes the transient input buffer
    std::printf("rss_over_file %.2f\n", double((rss1 - rss0) * 1024) / file);
    std::printf("peak_rss_over_file %.2f\n", double((hwm - rss0) * 1024) / file);
    g_sink = reinterpret_cast<size_t>(kv);
    return 0;
}

uint64_t Lcg(uint64_t& s) {
    s = s * 6364136223846793005ull + 1442695040888963407ull;
    return s >> 33;
}

int CmdLookup(const char* path, long n) {
    Node* kv = Load(path);
    if (!kv) return 1;
    // The key names the generators use: wide -> key_<i>, nested -> item_<i>, unique -> group_<i>_<..>.
    // The tree is probed with whichever prefix exists, so the same code serves every dataset.
    const char* prefix = nullptr;
    bool scalars = false;  // wide.kv: the keys hold plain values, nested.kv: the keys hold objects
    long count = 0;
    auto exists = [&](const std::string& name) { return Child(kv, name.c_str()) != nullptr || GetStrLen(kv, name.c_str()) > 0; };
    for (const char* p : {"key_", "item_"}) {
        if (!exists(std::string(p) + "0")) continue;
        prefix = p;
        scalars = std::strcmp(p, "key_") == 0;
        long lo = 0, hi = 1;  // keys are contiguous: find the first missing index
        while (exists(std::string(p) + std::to_string(hi))) { lo = hi; hi *= 2; }
        while (hi - lo > 1) {
            const long mid = (lo + hi) / 2;
            (exists(std::string(p) + std::to_string(mid)) ? lo : hi) = mid;
        }
        count = hi;
        break;
    }
    if (!prefix) { std::fprintf(stderr, "no key_/item_ keys in this file\n"); return 1; }

    std::vector<std::string> names;
    uint64_t seed = 99;
    for (long i = 0; i < n; ++i) names.push_back(prefix + std::to_string(Lcg(seed) % count));

    auto t = Clock::now();
    size_t sink = 0;
    for (const auto& s : names) sink += scalars ? GetStrLen(kv, s.c_str()) : reinterpret_cast<size_t>(Child(kv, s.c_str()));
    const double top = MsSince(t) * 1e6 / n;
    std::printf("top_level_lookup_ns %.1f\n", top);
    std::printf("top_level_objects %ld\n", count);

    // small objects: nested.kv items have "name", "kind", "count", "weight", "enabled", "pos"
    std::vector<Node*> items;
    for (long i = 0; i < std::min<long>(count, 4096); ++i) {
        Node* c = Child(kv, (std::string(prefix) + std::to_string(i)).c_str());
        if (c) items.push_back(c);
    }
    if (!items.empty() && GetInt(items[0], "count") != -1) {
        // Both backends must see the same data: sum of "count" and of strlen("name") over the items.
        long long sumCount = 0, sumName = 0;
        for (Node* it : items) { sumCount += GetInt(it, "count"); sumName += static_cast<long long>(GetStrLen(it, "name")); }
        std::printf("checksum %lld %lld %zu\n", sumCount, sumName, items.size());
        const long reps = 2000000;
        t = Clock::now();
        for (long i = 0; i < reps; ++i) sink += static_cast<size_t>(GetInt(items[i % items.size()], "count"));
        std::printf("small_object_getint_ns %.1f\n", MsSince(t) * 1e6 / reps);
        t = Clock::now();
        for (long i = 0; i < reps; ++i) sink += GetStrLen(items[i % items.size()], "name");
        std::printf("small_object_getstring_ns %.1f\n", MsSince(t) * 1e6 / reps);
    }
    g_sink = sink;
    return 0;
}

int CmdMt(const char* path, int threads, int reps) {
    {  // one load first so lazy initialisation is not part of the timing
        Node* warm = Load(path);
        if (!warm) return 1;
        Destroy(warm);
    }
    const double mb = FileSize(path) / (1024.0 * 1024.0);
    std::atomic<int> ready{0}, failed{0};
    std::atomic<bool> go{false};
    std::vector<std::thread> ts;
    for (int t = 0; t < threads; ++t)
        ts.emplace_back([&] {
            ready.fetch_add(1);
            while (!go.load()) {}
            for (int i = 0; i < reps; ++i) {
                Node* kv = Load(path);
                if (!kv) { failed.fetch_add(1); return; }
                Destroy(kv);
            }
        });
    while (ready.load() != threads) {}
    const auto start = Clock::now();
    go.store(true);
    for (auto& th : ts) th.join();
    const double ms = MsSince(start);
    std::printf("threads %d\nmt_ms %.2f\nmt_total_mbps %.1f\nfailed %d\n", threads, ms, mb * threads * reps / (ms / 1000), failed.load());
    return failed.load() ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
    const std::string cmd = argc > 1 ? argv[1] : "";
#if defined(BACKEND_SOURCE)
    // Source's default key-name table is a fixed 4 MB (Error("Out of keyvalue string space") when
    // full); the engine also ships a growable one.
    if (std::getenv("KV_SOURCE_GROWABLE")) KeyValues::SetUseGrowableStringTable(true);
#endif
    std::printf("backend %s\n", kBackend);
    if (cmd == "load" && argc == 4) return CmdLoad(argv[2], std::atoi(argv[3]));
    if (cmd == "save" && argc == 5) return CmdSave(argv[2], argv[3], std::atoi(argv[4]));
    if (cmd == "mem" && argc == 3) return CmdMem(argv[2]);
    if (cmd == "lookup" && argc == 4) return CmdLookup(argv[2], std::atol(argv[3]));
    if (cmd == "mt" && argc == 5) return CmdMt(argv[2], std::atoi(argv[3]), std::atoi(argv[4]));
    return 2;
}
