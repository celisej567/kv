# Source Engine KeyValues vs this library

Measured comparison with `KeyValues` from [nillerusr/source-engine](https://github.com/nillerusr/source-engine)
(commit `ed8209c`, 2025-11-25): speed, memory, threads, and a list of the places where the two
behave differently. Everything below can be reproduced with one command (see *Reproduce*).

## Reproduce

    # once: build the Source libraries the benchmark links against
    python3 waf configure -T release --build-games=hl2 --prefix=_BUILD --disable-warns
    python3 waf build --targets=tier0,tier1,vstdlib

    # then, from this repository
    python3 -I bench/compare/run_compare.py --source-engine=<checkout> [--out DIR] [--quick] > result.md

`--quick` uses files five times smaller and 3 repetitions instead of 7. The script compiles
`kv_compare.cpp` once against Source (`libtier1.a`, with the exact flags waf used: `-O2 -march=core2
-ffast-math -std=c++11`) and three times against this library (`ours`: the same flags; `ours_mt`: the same
plus `-DKV_THREAD_SAFE`, used only in the thread table; `ours -O3 native`: `-O3 -march=native`). Nothing is
written into the Source checkout.

## Where the test files come from

They are **generated, not stored**: `run_compare.py` calls the generators in `tests/gen_kv.py`
(deterministic, fixed random seed) and writes the files into the `--out` directory (default: a temp
directory). Four files are used:

| name | generator | shape | why |
|---|---|---|---|
| `wide` | `wide(100000)` | one object with 100 000 flat keys (`"key_N" value`; ints, floats and quoted strings) | worst case for per-object lookup by name |
| `nested` | `nested(50000)` | 50 000 objects of 6 keys plus a nested `pos` object; values drawn from a vocabulary of 200 words | closest to real game data: many small objects, few distinct names and values |
| `unique` | `unique(100000)` | 100 000 objects whose keys and values are all distinct | worst case for a string pool (nothing to deduplicate) |
| `nested_big` | `nested(400000)` | the same as `nested`, 8 times larger | scale (87 MB) |

Example of `nested` (first object):

    "Nested"
    {
        "item_0"
        {
            "name" "word199"
            "kind" "word14"
            "count" 119
            "weight" 7.11
            "enabled" true
            "pos" { "x" 85  "y" 88 }
        }
        ...

No real game files were used (none are available on the test machine), so the absolute numbers on real
data may differ; `nested` is the closest stand-in.

## What is measured, and how

* **Load**: Source `KeyValues::LoadFromBuffer` (the file is read whole with `fread` first, which is what
  its `LoadFromFile` does); ours `LoadFromFile` (streaming through a 256 KB window). Best of 7, page cache warm.
* **Destroy**: deleting the loaded tree.
* **Save**: Source `RecursiveSaveToFile` into a `CUtlBuffer`, then `fwrite`; ours `WriteToFile`. Best of 7.
* **Memory**: one loaded tree in a *fresh process* per file. *heap* = `mallinfo2()` bytes in use after the
  load (the string pool of ours is included); *RSS* = growth of VmRSS; *peak* = VmHWM.
* **Lookups**: top-level lookup by name of random keys in a large object, and `GetInt`/`GetString` in 6-key
  objects (the checksum of the values read must be equal in both libraries: the table says `same`).
* **Threads**: N threads each load `nested.kv` into their own tree; total MB/s.
* Not measured because Source cannot do it: conditional expressions beyond its fixed list, concurrent loading
  without a lock, enumeration by name, removal by name, `GetBool`.

## Hardware

AMD Ryzen 7 5800X (Zen 3, 8 cores/16 threads, 32 MB L3), GCC 16.2, glibc 2.44, Linux 7.2.8. One machine, one
session; ratios between the two libraries measured in the same process are more portable than MB/s.

## Results

Run of the final code (after escape sequences, empty-key error, `GetBool` and `RemoveKey` were added).
File sizes: wide 2.8 MB, nested 10.8 MB, unique 7.9 MB, nested_big 86.7 MB.

#### Load file -> tree (best of 7, page cache warm)

| file | Source | ours | ours -O3 native | ours vs Source |
|---|---|---|---|---|
| wide | 134.7 ms (20 MB/s) | 12.4 ms (218 MB/s) | 12.9 ms (210 MB/s) | 10.8x |
| nested | 154.6 ms (67 MB/s) | 43.3 ms (238 MB/s) | 45.2 ms (228 MB/s) | 3.6x |
| unique | 275.3 ms (28 MB/s) | 32.0 ms (237 MB/s) | 36.0 ms (210 MB/s) | 8.6x |
| nested_big | 1470.7 ms (56 MB/s) | 431.4 ms (192 MB/s) | 470.8 ms (176 MB/s) | 3.4x |

#### Destroy tree

| file | Source | ours | ours -O3 native |
|---|---|---|---|
| wide | 1.5 ms | 0.1 ms | 0.1 ms |
| nested | 6.8 ms | 2.4 ms | 2.3 ms |
| unique | 3.2 ms | 2.1 ms | 2.1 ms |
| nested_big | 57.0 ms | 17.1 ms | 16.9 ms |

#### Save tree -> file (Source writes to a buffer, then fwrite)

| file | Source | ours | ours -O3 native | output size Source / ours |
|---|---|---|---|---|
| wide | 8.7 ms (206 MB/s) | 2.5 ms (1092 MB/s) | 2.4 ms (1148 MB/s) | 1.9 / 2.8 MB |
| nested | 54.6 ms (141 MB/s) | 12.1 ms (860 MB/s) | 14.5 ms (714 MB/s) | 8.1 / 10.9 MB |
| unique | 20.7 ms (302 MB/s) | 6.3 ms (1207 MB/s) | 5.9 ms (1277 MB/s) | 6.5 / 7.9 MB |
| nested_big | 433.0 ms (143 MB/s) | 127.8 ms (653 MB/s) | 157.3 ms (530 MB/s) | 65.1 / 87.5 MB |

#### Memory of one loaded tree (fresh process; heap = malloc in use, RSS = resident growth)

| file | file size | heap Source | heap ours | RSS Source | RSS ours | peak RSS Source | peak RSS ours |
|---|---|---|---|---|---|---|---|
| wide | 2.8 MB | 10.7 MB (3.77x) | 12.1 MB (4.24x) | 12.8 MB | 10.5 MB | 15.4 MB | 12.3 MB |
| nested | 10.8 MB | 41.6 MB (3.86x) | 17.2 MB (1.60x) | 43.7 MB | 16.7 MB | 54.1 MB | 17.5 MB |
| unique | 7.9 MB | 22.4 MB (2.82x) | 27.4 MB (3.45x) | 26.6 MB | 23.2 MB | 34.3 MB | 24.9 MB |
| nested_big | 86.7 MB | 337.8 MB (3.90x) | 131.8 MB (1.52x) | 336.2 MB | 128.7 MB | 421.9 MB | 135.3 MB |

#### Lookups

| file | keys in the top object | top-level by name: Source | ours | 6-key object GetInt / GetString: Source | ours | values read |
|---|---|---|---|---|---|---|
| nested | 50000 | 52484 ns | 53 ns | 24 / 19 ns | 22 / 22 ns | same |
| wide | 100000 | 121372 ns | 149 ns | - | - | - |

Source's default key-name table (4 MB) overflowed on: nested_big; its numbers for those files use KeyValues::SetUseGrowableStringTable(true).

#### Threads loading nested.kv at the same time (total MB/s; own tree per thread)

| threads | Source | ours (thread-safe build) | ours vs Source |
|---|---|---|---|
| 1 | 62 MB/s | 200 MB/s | 3.3x |
| 2 | 64 MB/s | 271 MB/s | 4.2x |
| 4 | 63 MB/s | 235 MB/s | 3.7x |
| 8 | 63 MB/s | 214 MB/s | 3.4x |
| 16 | 65 MB/s | 203 MB/s | 3.1x |

### Noise in the `wide` lookup

The `wide` lookup (100 000 keys in one object, random access, so it is dominated by cache misses) moves a lot
between runs on this machine: **93 ns** in an earlier session, 128 and 149 ns in two full runs of the final code,
75 ns with `--quick` (20 000 keys). To find out whether the last changes caused it, an A/B was run: A = the final
sources with the new escape-sequence flag removed from the object layout and from the copy/move code; B = the
final build. Same compiler flags, eight alternating runs of `lookup wide.kv`:

| | wide lookup, ns (8 runs) | mean |
|---|---|---|
| A | 110, 115, 139, 122, 147, 104, 112, 106 | 119 |
| B | 148, 125, 125, 137, 121, 97, 106, 129 | 123 |

The spread inside one binary (97–148) is larger than the difference between the binaries, so this is machine
noise, not a regression (the lookup code was not touched; the flag lives in padding, `sizeof(CKeyValue)` is
unchanged). The same A/B for the rest: load of nested/wide/unique within 0.5 ms (42.1 vs 42.3 ms, 12.1 vs 12.6 ms,
33 vs 33 ms), heap bytes identical, nested lookup 57–62 vs 56–60 ns, `GetInt` in a small object 23.0 vs 23.2 ns.
Source's own number for the same lookup also drifted (118 µs earlier, 122–126 µs now).

## Reading the numbers

* **Load: 3.4x–11x faster.** On `nested` (typical data) 3.6x. The larger ratios on `wide` (10.8x) and `unique`
  (8.6x) come from a Source design limit: its key-name table (`CKeyValuesSystem`) has a fixed number of
  buckets with chains compared by `stricmp`, so with 100k+ distinct names every interning walks a long chain.
  Source also keeps the whole file in memory while parsing; ours streams.
* **Save: 3.3x–4.5x faster.** Outputs are not byte-equal: Source quotes every value, rewrites numbers
  (`7.11` -> `"7.110000"`) and indents with tabs; ours keeps the number text as read and indents with 4 spaces,
  so ours writes about 35–45% more bytes. Both write the same number of lines.
* **Destroy: 1.5x–15x faster.**
* **Lookups by name in a large object: about 1000x.** Source's `FindKey` walks the sibling list (51–54 µs with
  50 000 keys, 118–126 µs with 100 000); ours uses an index for objects over 16 keys (53–60 ns, 100–150 ns for
  100 000 keys, see the noise note). In small objects (6 keys), where games spend most of their time, both are
  about 20–30 ns: no difference.
* **Memory depends on the data.** On `nested` (repeated names and values) ours uses **2.4x–2.6x less heap** (1.5x–1.6x
  of the file size vs 3.9x) because every distinct string is stored once; Source allocates a node and a separate
  string per value. On `unique` and `wide` (every string distinct) **ours uses 13%–22% more heap**: a pool entry,
  a hash bucket and a node cost more than Source's per-node string when there is nothing to deduplicate. Peak RSS is
  lower for ours on every file (up to 3x on `nested_big`), because Source holds the whole file in memory while parsing.
* **Threads.** Source cannot load two files at once: `LoadFromBuffer` parses into a process-wide static token
  buffer, and unsynchronised runs with 4 threads crashed. The Source column therefore serialises parsing with a
  mutex and stays at ~63 MB/s. Ours is thread-safe and 3.1x–4.2x faster in total, but **does not scale**: about 200–270
  MB/s from 1 to 16 threads, because every interned token takes a shared lock on one pool (the reader counter is a
  cache line all threads fight over).

## Memory of the library itself, per data type

`sizeof(CKeyValue)` is 40 bytes; an entry is 16 bytes (`nameSlot`, type, `hidden` flag, value slot or child
pointer); the string pool stores each distinct string once (12 bytes of bookkeeping + the characters + a hash
bucket). Numbers are stored as their source text and parsed on `Get*` (a `from_chars` call, a few ns). The
type of each value is decided at load time from the text, as Source does.

## Behaviour that is different from Source

Checked in the Source code (`tier1/KeyValues.cpp`), not on game files.

**Closed in this library (the same as Source now):** the type of a value comes from its text and ignores quotes
(`"5"` is an int, `+5` is an int); `[$WIN32]`-style conditions; escape sequences are **off by default** (`\` is an
ordinary character; `UsesEscapeSequences(true)` before loading turns them on); `//` starts a comment anywhere
outside quotes; an empty key is an error.

**Still different, can matter for foreign files:**

1. **Several root keys** in one file: Source reads them as siblings; ours reads one root and reports
   `unexpected content after root object`.
2. **`#include` and `#base`**: Source loads other files; inside a block ours treats them as ordinary keys (nothing is included), and a `#base` line before the root name makes the file fail with `expected '{' after root name`.
3. **Error handling**: Source logs, stops the block, keeps what was read and returns true in most cases;
   ours returns false and leaves the object empty on any error.
4. **`GetInt` / `GetFloat` on a non-numeric string**: Source applies `atoi`/`atof` (`"12abc"` -> 12, `"abc"` -> 0, default ignored);
   ours returns the default. `GetInt` on a float truncates toward zero, as in Source.
5. **Integers outside `int`** (`3000000000`): Source stores a truncated int; ours stores a float and `GetInt` returns the default.
6. **Conditions**: ours evaluates `&&`, `||`, `!` and parentheses properly and the name list is extensible
   (`SetCondition`); Source searches a fixed list by substring, and `$OSX` is always false in it.
7. **Comments**: ours also accepts `/* ... */` (only at the start of a token); Source has only `//`.
8. **Token limit**: Source cuts tokens at 4096 characters; ours has no limit.
9. **Writing**: Source writes tabs, quotes every value and rewrites numbers; ours writes 4 spaces, keeps number text.
   With escapes off a quote inside a value cannot be written correctly (same as Source); with
   `UsesEscapeSequences(true)` the round trip is exact.
10. **Key names** are case-insensitive and stored in lower case (`GetName()` returns lower case); Source keeps the original case.
11. **Types**: no colour, pointer, 64-bit, wide-string types.

**Only in this library:** `UsesConditionals(bool)` (entries with a false condition are kept and hidden),
`SetCondition` expressions, `RemoveKey`/`RemoveKeys`, `GetBool`, `Children()`/`Children(name)` ranges, an index
for big objects, duplicate keys with first-wins `Get*`, a thread-safe global string pool.

## Where this library is weaker

* More heap than Source on files where almost every string is distinct.
* Parallel loading does not scale (see *Threads*).
* No `#include`/`#base`, several roots, or tolerant parsing (see the list above).
* Removing a key is O(n) (the duplicate chains and the name index are rebuilt).
