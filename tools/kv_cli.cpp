// Command-line driver used by tests/run_tests.py.
//   kv_cli dump <file>                      load, print every node as "path<TAB>type<TAB>value"
//   kv_cli dumpall <file>                   dump with UsesConditionals(false): entries with a false condition too
//   kv_cli write <in> <out>                 load, write back
//   kv_cli get <file> <path> <type> [def]   type: string|int|float|kv; path is '/'-separated
//   kv_cli list <file> <path> [name]        children of the object at <path> ('.' = root), only those called
//                                           [name] if given: "name<TAB>type<TAB>value" lines, then "#count N"
//   kv_cli api                              run the API scenarios (see Api()), "name 1|0" lines
//   kv_cli info                             "optimized=0|1" (perf numbers are only meaningful when 1)
//   kv_cli bench <file> <out> <lookups>     load once, then look keys up, then write; prints "name value" lines
// Exit code: 0 ok, 1 load/write failed, 2 usage.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <sys/resource.h>
#endif

#include "keyvalue.h"
#include "keyvalue_internal.h"
#include "kv_dump.h"

// Peak resident set size in bytes (0 if unknown).
static size_t PeakRssBytes() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) return pmc.PeakWorkingSetSize;
    return 0;
#elif defined(__APPLE__)
    rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return static_cast<size_t>(ru.ru_maxrss);
#else
    if (FILE* f = std::fopen("/proc/self/status", "r")) {
        char line[256];
        size_t kb = 0;
        while (std::fgets(line, sizeof line, f))
            if (std::sscanf(line, "VmHWM: %zu kB", &kb) == 1) break;
        std::fclose(f);
        return kb * 1024;
    }
    return 0;
#endif
}

static bool Optimized() {
#if defined(NDEBUG) && !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    return false;
#endif
#endif
    return true;
#else
    return false;
#endif
}

using Clock = std::chrono::steady_clock;
static double MsSince(Clock::time_point t) {
    return std::chrono::duration<double, std::milli>(Clock::now() - t).count();
}

// Collects every key name found in the tree (as C strings stable enough for the bench).
static void CollectKeys(const CKeyValue& kv, std::vector<std::pair<const CKeyValue*, uint32_t>>& out) {
    for (const auto& n : KVInternal::Nodes(kv)) {
        out.emplace_back(&kv, n.nameSlot);
        if (n.type == CKeyValue::KeyValue) CollectKeys(*n.child, out);
    }
}

static int Bench(const char* in, const char* outPath, long lookups) {
    KVStringPool& pool = KVStringPool::Instance();
    const size_t rssStart = PeakRssBytes();
    CKeyValue kv;

    auto t = Clock::now();
    if (!KvLoad(kv, in)) return 1;
    const double loadMs = MsSince(t);
    const size_t rssLoad = PeakRssBytes();

    std::vector<std::pair<const CKeyValue*, uint32_t>> keys;
    CollectKeys(kv, keys);

    // Look up keys spread over the whole tree (stride walk, deterministic).
    volatile long sink = 0;
    std::vector<std::string> names;  // names resolved up front so the timing only covers Get*
    const size_t picks = keys.empty() ? 0 : std::min<size_t>(keys.size(), 4096);
    for (size_t i = 0; i < picks; ++i) names.emplace_back(pool.View(keys[i * keys.size() / picks].second));
    t = Clock::now();
    for (long i = 0; i < lookups && picks; ++i) {
        const size_t k = static_cast<size_t>(i) % picks;
        sink = sink + keys[k * keys.size() / picks].first->GetInt(names[k].c_str());
    }
    const double lookupMs = MsSince(t);

    t = Clock::now();
    const bool wrote = kv.WriteToFile(outPath);
    const double writeMs = MsSince(t);

    // Loading the same file again must not add a single string to the pool.
    const size_t stringsAfterLoad = pool.Count();
    const size_t poolBytesAfterLoad = pool.MemoryUsage();
    CKeyValue again;
    if (!KvLoad(again, in)) return 1;

    std::printf("load_ms %.3f\n", loadMs);
    std::printf("lookup_ns %.1f\n", lookups ? lookupMs * 1e6 / static_cast<double>(lookups) : 0.0);
    std::printf("write_ms %.3f\n", writeMs);
    std::printf("tree_bytes %zu\n", kv.MemoryUsage());
    std::printf("pool_bytes %zu\n", pool.MemoryUsage());
    std::printf("pool_strings %zu\n", pool.Count());
    std::printf("pool_chars %zu\n", pool.BytesUsed());
    std::printf("reload_new_strings %zu\n", pool.Count() - stringsAfterLoad);
    std::printf("reload_new_pool_bytes %zu\n", pool.MemoryUsage() - poolBytesAfterLoad);
    std::printf("rss_start %zu\n", rssStart);
    std::printf("rss_peak %zu\n", rssLoad);
    return wrote ? 0 : 1;
}

// API scenarios that files alone cannot reach. Prints "name 1|0" per scenario; exit 1 if any failed.
static int Api() {
    int failed = 0;
    auto report = [&](const char* name, bool ok) {
        std::printf("%s %d\n", name, ok ? 1 : 0);
        if (!ok) ++failed;
    };
    auto sample = [] {
        CKeyValue kv("Root");
        kv.SetString("s", "text");
        kv.SetInt("i", -42);
        kv.SetFloat("f", 1.02f);
        CKeyValue child("unused");
        child.SetInt("x", 1);
        CKeyValue grand("unused");
        grand.SetString("deep", "yes");
        child.SetKeyValue("grand", &grand);
        kv.SetKeyValue("child", &child);
        return kv;
    };

    {   // kv = std::move(*kv.GetKeyValue(...)): the source lives inside the destination
        CKeyValue kv = sample();
        kv = std::move(*kv.GetKeyValue("child"));
        const CKeyValue* g = kv.GetKeyValue("grand");
        report("move_assign_from_descendant", kv.GetInt("x") == 1 && g && g->GetString("deep") == "yes" && !kv.GetKeyValue("child"));
    }
    {   // copy-assign from a descendant
        CKeyValue kv = sample();
        kv = *kv.GetKeyValue("child");
        report("copy_assign_from_descendant", kv.GetInt("x") == 1 && kv.GetKeyValue("grand") != nullptr);
    }
    {   // SetKeyValue(name, this): inserts a copy of the tree as it was
        CKeyValue kv = sample();
        kv.SetKeyValue("self", &kv);
        const CKeyValue* self = kv.GetKeyValue("self");
        report("set_self", self && self->GetInt("i") == -42 && self->GetKeyValue("child") && !self->GetKeyValue("self"));
    }
    {   // SetKeyValue(name, descendant) replacing that descendant's own parent key
        CKeyValue kv = sample();
        kv.SetKeyValue("child", kv.GetKeyValue("child")->GetKeyValue("grand"));
        const CKeyValue* c = kv.GetKeyValue("child");
        report("set_replace_with_descendant", c && c->GetString("deep") == "yes" && !c->GetKeyValue("grand"));
    }
    {   // copies are independent in both directions
        CKeyValue a = sample();
        CKeyValue b(a);
        b.GetKeyValue("child")->SetInt("x", 2);
        a.SetString("s", "changed");
        report("copy_independent", a.GetKeyValue("child")->GetInt("x") == 1 && b.GetString("s") == "text");
    }
    {   // move construction leaves the source empty and usable
        CKeyValue a = sample();
        CKeyValue b(std::move(a));
        a.SetInt("again", 3);
        report("move_construct", b.GetInt("i") == -42 && b.GetKeyValue("child") && a.GetInt("again") == 3 && !a.GetKeyValue("child"));
    }
    {   // Set* keeps the key's position and may change its type
        CKeyValue kv = sample();
        kv.SetInt("child", 5);  // KeyValue -> Int
        kv.SetKeyValue("s", kv.GetKeyValue("missing", nullptr));  // null value: ignored
        const auto& nodes = KVInternal::Nodes(kv);
        report("set_replaces_in_place", nodes.size() == 4 && nodes[3].type == CKeyValue::Int && kv.GetInt("child") == 5 && kv.GetString("s") == "text");
    }
    {   // type rules of the getters
        CKeyValue kv = sample();
        report("getters", kv.GetString("i") == "-42" && kv.GetString("f") == "1.02" && kv.GetInt("f") == 1 &&
                              kv.GetFloat("i") == -42.f && kv.GetInt("s", 7) == 7 && kv.GetString("child", "d") == "d" &&
                              kv.GetFloat("child", 2.f) == 2.f && kv.GetInt("missing") == 0 && kv.GetString("missing", "x") == "x");
    }
    {   // GetKeyValue with a fallback: wrong type behaves like missing and returns an owned copy
        CKeyValue kv = sample();
        CKeyValue fb("fb");
        fb.SetInt("v", 1);
        CKeyValue* found = kv.GetKeyValue("child", &fb);
        CKeyValue* copy = kv.GetKeyValue("s", &fb);
        const bool ok = found == kv.GetKeyValue("child") && copy && copy != &fb && copy->GetInt("v") == 1 &&
                        std::string(copy->GetName()) == "fb" && kv.GetKeyValue("s", nullptr) == nullptr;
        delete copy;
        report("getkeyvalue_fallback", ok);
    }
    {   // GetName stays valid while the pool grows by many strings
        CKeyValue kv("stable_name_check");
        const char* name = kv.GetName();
        for (int i = 0; i < 200000; ++i) kv.SetInt(("grow_" + std::to_string(i)).c_str(), i);
        report("name_pointer_stable", std::string(name) == "stable_name_check" && kv.GetInt("grow_199999") == 199999);
    }
    {   // Clear keeps the name, drops the keys
        CKeyValue kv = sample();
        kv.Clear();
        report("clear", std::string(kv.GetName()) == "root" && !kv.GetKeyValue("child") && kv.GetInt("i", 9) == 9);
    }

    // ---- repeated names -------------------------------------------------------------------
    auto names = [](const CKeyValue& kv, const char* name) {  // values of every occurrence, joined
        std::string out;
        for (const auto c : kv.Children(name)) out += (out.empty() ? "" : ",") + c.GetString();
        return out;
    };
    {   // Add* keeps every occurrence, in order, interleaved with other keys
        CKeyValue kv("R");
        kv.AddString("g", "1");
        kv.AddInt("other", 5);
        kv.AddString("g", "2");
        kv.AddInt("g", 3);
        kv.AddFloat("g", 4.5f);
        std::string all;
        for (const auto c : kv.Children()) all += std::string(c.Name()) + "=" + c.GetString() + ";";
        report("dups_order", names(kv, "g") == "1,2,3,4.5" && all == "g=1;other=5;g=2;g=3;g=4.5;" && kv.Count() == 5 &&
                                 kv.Count("g") == 4 && kv.Count("other") == 1 && kv.Count("nope") == 0);
    }
    {   // Get* sees the first occurrence, like Source's FindKey; Set* replaces only that one
        CKeyValue kv("R");
        kv.AddString("g", "1");
        kv.AddString("g", "2");
        kv.AddString("g", "3");
        const bool first = kv.GetString("g") == "1";
        kv.SetString("g", "X");
        report("dups_get_set_first", first && names(kv, "g") == "X,2,3" && kv.Count("g") == 3);
    }
    {   // Set* on a name that has duplicates, but where the first one is a KeyValue block
        CKeyValue kv("R"), blk("b");
        blk.SetInt("k", 1);
        kv.AddKeyValue("b", &blk);
        kv.AddString("b", "scalar");
        kv.AddKeyValue("b", &blk);
        const CKeyValue* first = kv.GetKeyValue("b");
        kv.SetInt("b", 9);  // replaces (and frees) the first block, keeps the other two
        report("dups_replace_first_block", first && kv.Count("b") == 3 && kv.GetInt("b") == 9 && names(kv, "b") == "9,scalar," &&  // the last one is a block: empty text
                                               kv.GetKeyValues("b").size() == 1);
    }
    {   // the first occurrence decides what Get* returns: a scalar first hides a block with the same name
        CKeyValue kv("R"), blk("b");
        kv.AddString("b", "scalar");
        kv.AddKeyValue("b", &blk);
        report("dups_first_wins_type", kv.GetKeyValue("b") == nullptr && kv.GetString("b") == "scalar" && kv.GetKeyValues("b").size() == 1);
    }
    {   // GetStrings / GetKeyValues skip the other kind
        CKeyValue kv("R"), blk("b");
        blk.SetInt("k", 1);
        kv.AddString("n", "a");
        kv.AddKeyValue("n", &blk);
        kv.AddInt("n", 7);
        const auto strs = kv.GetStrings("n");
        const auto blks = kv.GetKeyValues("n");
        report("dups_getstrings", strs.size() == 2 && strs[0] == "a" && strs[1] == "7" && blks.size() == 1 && blks[0]->GetInt("k") == 1 &&
                                      kv.GetStrings("none").empty());
    }
    {   // an object with an index (> 16 keys) and duplicates of early, middle and late keys
        CKeyValue kv("R");
        for (int i = 0; i < 100; ++i) kv.AddInt(("k" + std::to_string(i)).c_str(), i);
        for (int i : {0, 50, 99, 0, 50}) kv.AddInt(("k" + std::to_string(i)).c_str(), 1000 + i);
        bool ok = kv.Count() == 105 && kv.Count("k0") == 3 && kv.Count("k50") == 3 && kv.Count("k99") == 2 && kv.Count("k7") == 1;
        for (int i = 0; i < 100; ++i) ok = ok && kv.GetInt(("k" + std::to_string(i)).c_str(), -1) == i;  // first wins
        ok = ok && names(kv, "k0") == "0,1000,1000" && names(kv, "k99") == "99,1099";
        report("dups_with_index", ok);
    }
    {   // copy and move keep the chains; the copy is independent
        CKeyValue a("R");
        for (int i = 0; i < 40; ++i) a.AddInt("x", i);
        a.AddInt("y", 1);
        CKeyValue b(a);
        b.AddInt("x", 100);
        CKeyValue c(std::move(b));
        CKeyValue d("D");
        d = a;
        CKeyValue e("E");
        e = std::move(c);
        report("dups_copy_move", a.Count("x") == 40 && d.Count("x") == 40 && e.Count("x") == 41 && names(d, "x") == names(a, "x") &&
                                     e.GetInt("y") == 1 && c.Count() == 0);
    }
    {   // a tree with duplicates survives write and reload in the same order
        CKeyValue kv("R"), inner("i");
        inner.AddString("p", "1");
        inner.AddString("p", "2");
        kv.AddKeyValue("blk", &inner);
        kv.AddString("g", "a");
        kv.AddKeyValue("blk", &inner);
        kv.AddString("g", "b");
        const std::string tmp = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/kv_api_dups.kv";
        CKeyValue back;
        const bool ok = kv.WriteToFile(tmp.c_str()) && back.LoadFromFile(tmp.c_str());
        std::remove(tmp.c_str());
        const auto blocks = back.GetKeyValues("blk");
        report("dups_write_reload", ok && names(back, "g") == "a,b" && blocks.size() == 2 && names(*blocks[1], "p") == "1,2" && back.Count() == 4);
    }
    {   // 200 000 duplicates of one name stay linear (no walking the chain on every add)
        CKeyValue kv("R");
        auto t = std::chrono::steady_clock::now();
        for (int i = 0; i < 200000; ++i) kv.AddInt("same", i);
        size_t n = 0;
        long long sum = 0;
        for (const auto c : kv.Children("same")) { ++n; sum += c.GetInt(); }
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        report("dups_many_linear", n == 200000 && sum == 199999LL * 200000 / 2 && kv.GetInt("same") == 0 && ms < 2000);
    }
    {   // Clear forgets the chains
        CKeyValue kv("R");
        kv.AddInt("x", 1);
        kv.AddInt("x", 2);
        kv.Clear();
        kv.AddInt("x", 3);
        report("dups_clear", kv.Count() == 1 && kv.Count("x") == 1 && names(kv, "x") == "3");
    }
    {   // application-defined conditions, overriding a built-in one, "$" optional, case-insensitive
        const std::string tmp = std::string(std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp") + "/kv_api_cond.kv";
        if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
            std::fputs("R { a 1 [$MYGAME] b 1 [!$MYGAME] c 1 [$X360] d 1 [$MyGame&&$KVTRUE] }\n", f);
            std::fclose(f);
        }
        auto load = [&] {
            CKeyValue kv("R");
            kv.LoadFromFile(tmp.c_str());
            std::string r;
            for (const auto c : kv.Children()) r += c.Name();
            return r;
        };
        const std::string before = load();
        CKeyValue::SetCondition("$mygame", true);
        const std::string on = load();
        CKeyValue::SetCondition("MYGAME", false);
        const std::string off = load();
        CKeyValue::SetCondition("X360", true);
        const std::string x360 = load();
        CKeyValue::SetCondition("X360", false);
        std::remove(tmp.c_str());
        report("conditions_custom", before == "b" && on == "ad" && off == "b" && x360 == "bc");
    }
    {   // UsesConditionals: hidden entries exist but accessors skip them; false shows everything
        const std::string dir = std::getenv("TMPDIR") ? std::getenv("TMPDIR") : "/tmp";
        const std::string tmp = dir + "/kv_api_uc.kv";
        const std::string out = dir + "/kv_api_uc_out.kv";
        if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
            std::fputs("R { a 1 [$X360] a 2 [$KVTRUE] a 3 b [$X360] \"hid\" c [$X360] { z 1 } d 1 [$X360] "
                       "e [$X360] { z 1 } e { z 2 } f 1 }\n", f);
            std::fclose(f);
        }
        CKeyValue kv("R");
        bool ok = kv.UsesConditionals() && kv.LoadFromFile(tmp.c_str());
        const std::string vis = names(kv, "a");
        ok = ok && vis == "2,3" && kv.GetInt("a") == 2 && kv.GetString("b", "none") == "none" &&
             kv.GetKeyValue("c") == nullptr && kv.GetInt("d", -1) == -1 && kv.Count() == 4 && kv.Count("a") == 2 &&
             kv.Count("b") == 0 && kv.Children("b").empty() && kv.GetKeyValue("e") && kv.GetKeyValue("e")->GetInt("z") == 2 &&
             kv.GetStrings("b").empty() && kv.GetKeyValues("e").size() == 1;
        std::string order;
        for (const auto c : kv.Children()) order += c.Name();
        ok = ok && order == "aaef";
        report("uc_hidden_skipped", ok);

        kv.UsesConditionals(false);  // everything is there again, in file order, first entry wins
        std::string all;
        for (const auto c : kv.Children()) all += c.Name();
        report("uc_false_shows_all", kv.Count() == 9 && all == "aaabcdeef" && kv.GetInt("a") == 1 &&
               kv.GetString("b") == "hid" && kv.GetKeyValue("c") && kv.GetKeyValue("c")->GetInt("z") == 1 &&
               kv.Count("a") == 3 && kv.GetKeyValue("e")->GetInt("z") == 1 && names(kv, "a") == "1,2,3");
        kv.UsesConditionals(true);
        report("uc_toggle_back", kv.Count() == 4 && kv.GetInt("a") == 2);

        // Set* replaces the first VISIBLE entry; with none visible a new one is added after the hidden ones
        kv.SetInt("a", 20);
        kv.SetString("b", "new");
        kv.SetString("d", "dd");
        std::string seen = names(kv, "a") + "|" + kv.GetString("b") + "|" + kv.GetString("d");
        kv.UsesConditionals(false);
        std::string everything = names(kv, "a") + "|" + names(kv, "b") + "|" + names(kv, "d");
        kv.UsesConditionals(true);
        report("uc_set_on_hidden", seen == "20,3|new|dd" && everything == "1,20,3|hid,new|1,dd");

        // copy and move keep the hidden entries and the flag; a child added to a parent adopts the parent's flag
        CKeyValue copy(kv);
        CKeyValue moved{CKeyValue(kv)};
        CKeyValue off("O");
        off.UsesConditionals(false);
        off.AddKeyValue("sub", &kv);
        const bool subShowsAll = off.GetKeyValue("sub") && off.GetKeyValue("sub")->Count() == 11;
        CKeyValue on("N");
        on.AddKeyValue("sub", &kv);
        report("uc_copy_move_adopt", copy.Count() == 6 && moved.Count() == 6 && copy.UsesConditionals() &&
               subShowsAll && on.GetKeyValue("sub")->Count() == 6 && !off.UsesConditionals());
        copy.UsesConditionals(false);
        report("uc_copy_independent", copy.Count() == 11 && kv.Count() == 6);

        // write: hidden entries are not written while UsesConditionals is true, everything when false
        CKeyValue back, backAll;
        backAll.UsesConditionals(false);
        const bool w1 = kv.WriteToFile(out.c_str()) && back.LoadFromFile(out.c_str());
        kv.UsesConditionals(false);
        const bool w2 = kv.WriteToFile(out.c_str()) && backAll.LoadFromFile(out.c_str());
        kv.UsesConditionals(true);
        report("uc_write", w1 && w2 && back.Count() == kv.Count() && backAll.Count() == 11);

        // the flag survives LoadFromFile
        CKeyValue off2("R");
        off2.UsesConditionals(false);
        const bool l = off2.LoadFromFile(tmp.c_str());
        CKeyValue on2("R");
        const bool l2 = on2.LoadFromFile(tmp.c_str());
        report("uc_survives_load", l && l2 && !off2.UsesConditionals() && off2.Count() == 9 && on2.Count() == 4 &&
               !off2.GetKeyValue("c")->UsesConditionals());
        // Clear forgets what was hidden
        on2.Clear();
        on2.AddInt("q", 1);
        report("uc_clear", on2.Count() == 1);
        std::remove(tmp.c_str());
        std::remove(out.c_str());
    }
    {   // names are case-insensitive and stored in lower case; values are not touched
        CKeyValue kv("MiXed");
        kv.SetString("Key", "Value");
        kv.SetInt("KEY", 5);            // same name: replaces
        kv.AddString("kEy", "Dup");     // same name: another entry
        bool ok = std::string(kv.GetName()) == "mixed" && kv.Count() == 2 && kv.Count("KEY") == 2 && kv.GetInt("key") == 5;
        std::string names;
        for (const auto c : kv.Children("kEY")) names += std::string(c.Name()) + "=" + c.GetString() + ";";
        ok = ok && names == "key=5;key=Dup;";
        CKeyValue sub("SUB");
        sub.SetString("X", "MixedValue");
        kv.SetKeyValue("Child", &sub);
        ok = ok && kv.GetKeyValue("CHILD") && std::string(kv.GetKeyValue("child")->GetName()) == "child" &&
             kv.GetKeyValue("cHiLd")->GetString("x") == "MixedValue";
        report("names_case_insensitive", ok);
    }
    {   // GetBool: numbers (non-zero), true/false in any case, else the default
        CKeyValue kv("R");
        kv.SetInt("i1", 1); kv.SetInt("i0", 0); kv.SetInt("neg", -3);
        kv.SetFloat("f", 0.5f); kv.SetFloat("fz", 0.f);
        kv.SetString("t", "true"); kv.SetString("T2", "TrUe"); kv.SetString("f1", "false"); kv.SetString("f2", "FALSE");
        kv.SetString("w", "yes"); kv.SetString("e", ""); kv.SetString("tt", "truee");
        CKeyValue sub("S");
        kv.SetKeyValue("sub", &sub);
        bool ok = kv.GetBool("i1") && !kv.GetBool("i0", true) && kv.GetBool("neg") && kv.GetBool("f") && !kv.GetBool("fz", true) &&
                  kv.GetBool("t") && kv.GetBool("t2") && !kv.GetBool("f1", true) && !kv.GetBool("f2", true);
        ok = ok && kv.GetBool("w", true) && !kv.GetBool("w", false) && kv.GetBool("e", true) && kv.GetBool("tt", true) &&
             kv.GetBool("sub", true) && !kv.GetBool("sub") && kv.GetBool("missing", true) && !kv.GetBool("missing");
        bool viaChild = true;
        for (const auto c : kv.Children("t")) viaChild = viaChild && c.GetBool();
        report("getbool", ok && viaChild);
    }
    {   // GetInt on a float keeps the part before the point
        CKeyValue kv("R");
        kv.SetFloat("a", 1.9f); kv.SetFloat("b", -1.9f); kv.SetFloat("c", 0.99f); kv.SetFloat("d", -0.5f);
        report("int_truncates_float", kv.GetInt("a") == 1 && kv.GetInt("b") == -1 && kv.GetInt("c") == 0 && kv.GetInt("d") == 0);
    }
    {   // RemoveKey / RemoveKeys
        CKeyValue kv("R");
        kv.AddString("a", "1"); kv.AddString("b", "2"); kv.AddString("a", "3");
        CKeyValue sub("S"); sub.SetInt("x", 1);
        kv.AddKeyValue("c", &sub); kv.AddString("a", "4");
        bool ok = kv.RemoveKey("A") && kv.Count("a") == 2 && kv.GetString("a") == "3" && kv.Count() == 4;
        ok = ok && !kv.RemoveKey("zzz") && kv.RemoveKey("c") && !kv.GetKeyValue("c") && kv.Count() == 3;
        ok = ok && kv.RemoveKeys("a") == 2 && kv.Count() == 1 && kv.GetString("a", "none") == "none" && kv.GetString("b") == "2";
        ok = ok && kv.RemoveKeys("a") == 0 && kv.RemoveKey("b") && kv.Count() == 0 && kv.Children().empty();
        kv.AddString("a", "again");  // the object is still usable
        report("remove_key", ok && kv.GetString("a") == "again" && kv.Count() == 1);
    }
    {   // removal in a big object (name index) with duplicates; order and lookups survive
        CKeyValue kv("R");
        for (int i = 0; i < 100; ++i) kv.AddInt(("k" + std::to_string(i % 50)).c_str(), i);  // every name twice
        bool ok = kv.Count() == 100;
        for (int i = 0; i < 50; i += 2) ok = ok && kv.RemoveKey(("k" + std::to_string(i)).c_str());  // the first of 25 names
        ok = ok && kv.Count() == 75 && kv.GetInt("k0", -1) == 50 && kv.GetInt("k1") == 1 && kv.Count("k0") == 1 && kv.Count("k1") == 2;
        for (int i = 1; i < 50; i += 2) ok = ok && kv.RemoveKeys(("K" + std::to_string(i)).c_str()) == 2;
        ok = ok && kv.Count() == 25 && kv.GetInt("k1", -1) == -1;
        std::string order;
        for (const auto c : kv.Children()) order += std::to_string(c.GetInt()) + ",";
        std::string want;
        for (int i = 50; i < 100; i += 2) want += std::to_string(i) + ",";
        report("remove_key_big", ok && order == want);
        for (int i = 0; i < 50; i += 2) kv.RemoveKeys(("k" + std::to_string(i)).c_str());
        report("remove_key_all", kv.Count() == 0 && kv.GetInt("k0", -1) == -1);
    }
    {   // removal and conditions: switched-off entries are absent, and stay when removing
        const std::string tmp = "/tmp/kv_cli_remove.kv";
        if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
            std::fputs("\"R\"\n{\n a 1\n a 2 [$X360]\n a 3\n}\n", f);
            std::fclose(f);
        }
        CKeyValue kv("R");
        bool ok = kv.LoadFromFile(tmp.c_str()) && kv.Count("a") == 2 && kv.RemoveKeys("a") == 2 && kv.Count() == 0;
        kv.UsesConditionals(false);
        ok = ok && kv.Count() == 1 && kv.GetInt("a") == 2;
        report("remove_key_hidden", ok);
        std::remove(tmp.c_str());
    }
    {   // UsesEscapeSequences: off by default, propagates, survives LoadFromFile, controls writing
        const std::string tmp = "/tmp/kv_cli_esc.kv", out = "/tmp/kv_cli_esc.out";
        if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
            std::fputs("\"R\"\n{\n a \"x\\ny\"\n sub { b \"p\\\\q\" }\n}\n", f);
            std::fclose(f);
        }
        CKeyValue off("R"), on("R");
        on.UsesEscapeSequences(true);
        bool ok = !off.UsesEscapeSequences() && on.UsesEscapeSequences();
        ok = ok && off.LoadFromFile(tmp.c_str()) && on.LoadFromFile(tmp.c_str());
        ok = ok && off.GetString("a") == "x\\ny" && on.GetString("a") == "x\ny";
        ok = ok && off.GetKeyValue("sub")->GetString("b") == "p\\\\q" && !off.GetKeyValue("sub")->UsesEscapeSequences();
        ok = ok && on.UsesEscapeSequences() && on.GetKeyValue("sub")->UsesEscapeSequences();
        // writing: on escapes (a value with a newline survives the round trip), off writes the text as it is
        on.SetString("q", "he said \"hi\"\n");
        CKeyValue back("R");
        back.UsesEscapeSequences(true);
        ok = ok && on.WriteToFile(out.c_str()) && back.LoadFromFile(out.c_str()) && back.GetString("q") == "he said \"hi\"\n";
        off.SetString("path", "C:\\dir\\");
        CKeyValue back2("R");
        ok = ok && off.WriteToFile(out.c_str()) && back2.LoadFromFile(out.c_str()) && back2.GetString("path") == "C:\\dir\\" &&
             back2.GetString("a") == "x\\ny";
        // copies and adopted children take the flag of the parent
        CKeyValue parent("P");
        parent.UsesEscapeSequences(true);
        parent.SetKeyValue("c", &off);
        ok = ok && parent.GetKeyValue("c")->UsesEscapeSequences() && parent.GetKeyValue("c")->GetKeyValue("sub")->UsesEscapeSequences() &&
             !off.UsesEscapeSequences();
        report("uses_escape_sequences", ok);
        std::remove(tmp.c_str());
        std::remove(out.c_str());
    }
    {   // several roots: an unnamed object takes the first, a named one looks for its own
        const std::string tmp = "/tmp/kv_cli_roots.kv";
        if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
            std::fputs("\"First\" { a 1 }\n\"Second\" { b 2 sub { c 3 } }\n\"third\" { d 4 }\n\"Second\" { e 5 }\n", f);
            std::fclose(f);
        }
        CKeyValue unnamed;
        bool ok = std::string(unnamed.GetName()).empty();
        ok = ok && unnamed.LoadFromFile(tmp.c_str()) && std::string(unnamed.GetName()) == "first" && unnamed.GetInt("a") == 1 && unnamed.Count() == 1;
        CKeyValue empty("");
        ok = ok && empty.LoadFromFile(tmp.c_str()) && std::string(empty.GetName()) == "first";
        report("root_unnamed_takes_first", ok);

        CKeyValue second("SECOND");  // case-insensitive; the first root with this name wins
        ok = second.LoadFromFile(tmp.c_str()) && std::string(second.GetName()) == "second" && second.GetInt("b") == 2 &&
             second.GetKeyValue("sub")->GetInt("c") == 3 && second.Count("e") == 0;
        CKeyValue third("third"), first("first");
        ok = ok && third.LoadFromFile(tmp.c_str()) && third.GetInt("d") == 4 && first.LoadFromFile(tmp.c_str()) && first.GetInt("a") == 1;
        report("root_named_finds_own", ok);

        CKeyValue missing("nothing");
        missing.SetInt("old", 1);
        ok = !missing.LoadFromFile(tmp.c_str()) && std::string(missing.GetName()) == "nothing" && missing.Count() == 0;
        report("root_named_not_found", ok);

        if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
            std::fputs("\"first\" { a 1 }\n\"broken\" { b 2\n", f);
            std::fclose(f);
        }
        CKeyValue skipped("first"), skippedUnnamed;
        ok = !skipped.LoadFromFile(tmp.c_str()) && !skippedUnnamed.LoadFromFile(tmp.c_str()) && std::string(skippedUnnamed.GetName()).empty();
        report("root_other_roots_checked", ok);
        std::remove(tmp.c_str());
    }
    {   // a name made of a space: copy, write, reload by name and as an unnamed object
        const std::string tmp = "/tmp/kv_cli_space.kv";
        CKeyValue sp(" ");
        sp.SetInt("x", 1);
        CKeyValue copy(sp), assigned;
        assigned = sp;
        bool ok = std::string(copy.GetName()) == " " && std::string(assigned.GetName()) == " " && copy.GetInt("x") == 1;
        CKeyValue unnamedCopy{CKeyValue()};
        ok = ok && std::string(unnamedCopy.GetName()).empty();
        CKeyValue back, byName(" "), other("  ");
        ok = ok && sp.WriteToFile(tmp.c_str()) && back.LoadFromFile(tmp.c_str()) && std::string(back.GetName()) == " " && back.GetInt("x") == 1;
        ok = ok && byName.LoadFromFile(tmp.c_str()) && byName.GetInt("x") == 1 && !other.LoadFromFile(tmp.c_str());
        report("name_with_space", ok);
        std::remove(tmp.c_str());
    }
    {   // numbers are parsed once (at load / Set*) and kept in the node; Get* must agree with the text
        const std::string tmp = "/tmp/kv_cli_numbers.kv";
        if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
            std::fputs("\"R\" { a 2.99999999 b -2.99999999 c 7.5 d 5.0 e 16777217.0 f 3000000000.5 g -0 h 0.0 i 1.5e3 j 42 k -7 l +9 }\n", f);
            std::fclose(f);
        }
        CKeyValue kv;
        bool ok = kv.LoadFromFile(tmp.c_str());
        ok = ok && kv.GetInt("a", -1) == 2 && kv.GetInt("b", -1) == -2 && kv.GetInt("c", -1) == 7 && kv.GetInt("d", -1) == 5;
        ok = ok && kv.GetInt("e", -1) == 16777217 && kv.GetInt("f", -1) == -1 && kv.GetInt("i", -1) == 1500;
        ok = ok && kv.GetFloat("c") == 7.5f && kv.GetFloat("j") == 42.f && kv.GetFloat("k") == -7.f && kv.GetInt("l") == 9;
        ok = ok && kv.GetBool("g", true) == false && kv.GetBool("h", true) == false && kv.GetBool("a") && kv.GetBool("k");
        kv.SetInt("n", -123);
        kv.SetFloat("x", 0.1f);
        kv.AddInt("n", 5);
        kv.AddFloat("x", -2.5f);
        ok = ok && kv.GetInt("n") == -123 && kv.GetFloat("x") == 0.1f && kv.GetInt("x", -1) == 0 && kv.GetFloat("n") == -123.f;
        int sum = 0;
        for (const auto c : kv.Children("n")) sum += c.GetInt();
        ok = ok && sum == -118;
        CKeyValue copy(kv), moved(std::move(copy));
        ok = ok && moved.GetInt("a") == 2 && moved.GetFloat("x") == 0.1f && moved.GetInt("j") == 42;
        kv.SetFloat("j", 1.25f);  // an Int entry becomes a Float
        ok = ok && kv.GetFloat("j") == 1.25f && kv.GetInt("j") == 1;
        report("numbers_kept_in_node", ok);
        std::remove(tmp.c_str());
    }
    {   // SetString / AddString decide the type from the text, like a file read does
        CKeyValue kv("R");
        kv.SetString("i", "42");
        kv.SetString("f", "3.5");
        kv.SetString("s", "hello");
        kv.SetString("e", "");
        kv.SetString("sp", " 42");
        kv.SetString("hex", "0x10");
        kv.AddString("a", "7");
        kv.AddString("a", "7.25");
        auto type = [&](const char* n) { return (*kv.Children(n).begin()).GetType(); };
        bool ok = type("i") == CKeyValue::Int && type("f") == CKeyValue::Float && type("s") == CKeyValue::String && type("e") == CKeyValue::String;
        ok = ok && type("sp") == CKeyValue::String && type("hex") == CKeyValue::String;
        ok = ok && kv.GetInt("i", -1) == 42 && kv.GetFloat("f", -1) == 3.5f && kv.GetInt("f", -1) == 3 && kv.GetInt("s", -1) == -1;
        ok = ok && kv.GetBool("i") && kv.GetInt("a") == 7;
        report("setstring_classifies", ok);
    }
    {   // GetString on every type: numbers give their text as written, a block gives the default
        const std::string tmp = "/tmp/kv_cli_getstring.kv";
        if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
            std::fputs("\"R\" { i 007 f 1.50 q \"5\" s abc b { x 1 } }\n", f);
            std::fclose(f);
        }
        CKeyValue kv;
        bool ok = kv.LoadFromFile(tmp.c_str());
        ok = ok && kv.GetString("i") == "007" && kv.GetString("f") == "1.50" && kv.GetString("q") == "5" && kv.GetString("s") == "abc";
        ok = ok && kv.GetString("b").empty() && kv.GetString("b", "dflt") == "dflt" && kv.GetString("b", nullptr).empty();
        ok = ok && kv.GetString("none", "dflt") == "dflt";
        kv.SetInt("n", -5);
        kv.SetFloat("g", 2.0f);
        kv.SetFloat("h", 0.1f);
        ok = ok && kv.GetString("n") == "-5" && kv.GetString("g") == "2.0" && kv.GetString("h") == "0.1";
        for (const auto c : kv.Children()) {
            if (std::string(c.Name()) == "b") ok = ok && c.GetString("x") == "x" && c.GetString().empty();
            if (std::string(c.Name()) == "i") ok = ok && c.GetString("x") == "007";
        }
        report("getstring_on_types", ok);
        std::remove(tmp.c_str());
    }
    {   // empty ranges
        CKeyValue kv("R");
        bool any = false;
        for (const auto c : kv.Children()) { (void)c; any = true; }
        for (const auto c : kv.Children("x")) { (void)c; any = true; }
        report("children_empty", !any && kv.Children().empty() && kv.Children("x").empty() && kv.Count() == 0);
    }
    return failed ? 1 : 0;
}

int main(int argc, char** argv) {
    CKeyValue::SetCondition("KVTRUE", true);  // a condition that is true on every platform, for the tests
    if (argc == 2 && std::string(argv[1]) == "info") {
        std::printf("optimized=%d\n", Optimized() ? 1 : 0);
        return 0;
    }
    if (argc == 5 && std::string(argv[1]) == "bench") return Bench(argv[2], argv[3], std::atol(argv[4]));
    if (argc == 2 && std::string(argv[1]) == "api") return Api();

    if (argc < 3) return 2;
    const std::string cmd = argv[1];
    CKeyValue kv;

    if (cmd == "dump" && argc == 3) {
        if (!KvLoad(kv, argv[2])) return 1;
        std::fputs(KvDump(kv).c_str(), stdout);
        return 0;
    }
    if (cmd == "dumpall" && argc == 3) {  // like dump, but UsesConditionals(false): nothing is hidden
        kv.UsesConditionals(false);
        if (!KvLoad(kv, argv[2])) return 1;
        std::fputs(KvDump(kv).c_str(), stdout);
        return 0;
    }
    if (cmd == "write" && argc == 4) {
        if (!KvLoad(kv, argv[2])) return 1;
        return kv.WriteToFile(argv[3]) ? 0 : 1;
    }
    if (cmd == "list" && (argc == 4 || argc == 5)) {
        if (!KvLoad(kv, argv[2])) return 1;
        const std::string path = argv[3];
        const CKeyValue* cur = &kv;
        size_t pos = 0;
        while (cur && path != "." && pos <= path.size()) {
            size_t slash = path.find('/', pos);
            const std::string part = path.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
            cur = cur->GetKeyValue(part.c_str());
            if (slash == std::string::npos) break;
            pos = slash + 1;
        }
        if (!cur) {
            std::printf("#count 0\n#no such object\n");
            return 0;
        }
        const char* name = argc == 5 ? argv[4] : nullptr;
        size_t n = 0;
        auto print = [&](const CKeyValue::Child& c) {
            const char* t = c.GetType() == CKeyValue::String ? "string" : c.GetType() == CKeyValue::Int ? "int"
                          : c.GetType() == CKeyValue::Float ? "float" : "kv";
            std::printf("%s\t%s\t%s\n", KvEscape(c.Name()).c_str(), t,
                        c.GetType() == CKeyValue::KeyValue ? "" : KvEscape(c.GetString()).c_str());
            ++n;
        };
        if (name) for (const auto c : cur->Children(name)) print(c);
        else for (const auto c : cur->Children()) print(c);
        std::printf("#count %zu\n", n);
        std::printf("#api_count %zu\n", name ? cur->Count(name) : cur->Count());
        return 0;
    }
    if (cmd == "get" && (argc == 5 || argc == 6)) {
        if (!KvLoad(kv, argv[2])) return 1;
        const std::string path = argv[3], type = argv[4];
        const char* def = argc == 6 ? argv[5] : nullptr;

        CKeyValue empty("empty");
        const CKeyValue* cur = &kv;
        size_t pos = 0;
        std::string leaf;
        for (;;) {
            size_t slash = path.find('/', pos);
            if (slash == std::string::npos) { leaf = path.substr(pos); break; }
            const CKeyValue* next = cur->GetKeyValue(path.substr(pos, slash - pos).c_str());
            cur = next ? next : &empty;
            pos = slash + 1;
        }

        if (type == "string") std::printf("%s\n", KvEscape(cur->GetString(leaf.c_str(), def ? def : "")).c_str());
        else if (type == "int") std::printf("%d\n", cur->GetInt(leaf.c_str(), def ? std::atoi(def) : 0));
        else if (type == "float") std::printf("%g\n", static_cast<double>(cur->GetFloat(leaf.c_str(), def ? static_cast<float>(std::atof(def)) : 0.f)));
        else if (type == "kv") std::printf("%s\n", cur->GetKeyValue(leaf.c_str()) ? "kv" : "null");
        else if (type == "kvdef") {
            // GetKeyValue with a fallback: found -> the tree's own node; missing -> an independent copy.
            CKeyValue fallback("fb");
            fallback.SetInt("x", 7);
            const CKeyValue* inTree = cur->GetKeyValue(leaf.c_str());
            const CKeyValue* r = cur->GetKeyValue(leaf.c_str(), &fallback);
            if (inTree) {
                std::printf("%s\n", r == inTree ? "tree" : "BAD");
            } else if (!r || r == &fallback) {
                std::printf("BAD\n");
            } else {
                CKeyValue* mine = const_cast<CKeyValue*>(r);
                mine->SetInt("x", 9);  // must not touch the fallback
                std::printf("copy %s x=%d fallback_x=%d\n", mine->GetName(), r->GetInt("x"), fallback.GetInt("x"));
                delete r;
            }
            std::printf("%s\n", cur->GetKeyValue(leaf.c_str(), nullptr) ? "nonnull" : "null");
        }
        else return 2;
        return 0;
    }
    return 2;
}
