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

The Rust crate `source-kv` is compared separately, see [bench/source-kv/](../source-kv/README.md).

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
* **Correction.** Earlier versions of `kv_compare.cpp` saved Source trees by calling `RecursiveSaveToFile` on every
  child of the root. That function writes a *container* (its name and its children), so for `wide` (whose children are
  scalars) Source wrote 100 000 empty blocks (1.9 MB) instead of the values: its save time for `wide` was too low
  (8.7 ms instead of 15 ms). It now saves the root, exactly like `SaveToFile`; the output is byte-identical to the one
  of Portal 2: Community Edition (see below).
* **Lookups**: top-level lookup by name of random keys in a large object, and `GetInt`/`GetString` in 6-key
  objects (the checksum of the values read must be equal in both libraries: the table says `same`).
* **Threads**: N threads each load `nested.kv` into their own tree; total MB/s.
* Not measured because Source cannot do it: conditional expressions beyond its fixed list, concurrent loading
  without a lock, enumeration by name, removal by name, `GetBool`.

## Hardware

AMD Ryzen 7 5800X (Zen 3, 8 cores/16 threads, 32 MB L3), GCC 16.2, glibc 2.44, Linux 7.2.8. One machine, one
session; ratios between the two libraries measured in the same process are more portable than MB/s.

## Results

Run of the final code (after several roots per file, the unnamed constructor, parsed numbers kept in the entry, `SetString` deciding the type, the faster parser and writer, and numbers without pooled text were added).
File sizes: wide 2.8 MB, nested 10.8 MB, unique 7.9 MB, nested_big 86.7 MB.

P2CE is Portal 2: Community Edition, measured through its own binaries (see the section below; the tools for it are not published).
The Source, P2CE and ours columns come from one run on the same machine; the `ours -O3 native` column and the
Threads table come from a second run of `run_compare.py`, which measured Source and ours again (the two runs of Source differ by a few
percent, for example `nested` load 153.4 vs 153.2 ms; lookups in big objects by more, cache misses).

#### Load file -> tree (best of 7, page cache warm)

| file | Source | P2CE | ours | ours -O3 native | ours vs Source | ours vs P2CE |
|---|---|---|---|---|---|---|
| wide | 133.2 ms (20 MB/s) | 117.0 ms (23 MB/s) | 10.7 ms (254 MB/s) | 10.6 ms (257 MB/s) | 12.5x | 11.0x |
| nested | 149.3 ms (69 MB/s) | 109.2 ms (94 MB/s) | 28.3 ms (364 MB/s) | 28.9 ms (356 MB/s) | 5.3x | 3.9x |
| unique | 264.6 ms (29 MB/s) | 262.3 ms (29 MB/s) | 20.4 ms (372 MB/s) | 19.9 ms (381 MB/s) | 13.0x | 12.9x |
| nested_big | 1367.7 ms (60 MB/s) | 1999.6 ms (41 MB/s) | 254.6 ms (325 MB/s) | 239.5 ms (345 MB/s) | 5.4x | 7.9x |

#### Destroy tree

| file | Source | P2CE | ours | ours -O3 native |
|---|---|---|---|---|
| wide | 1.5 ms | 0.7 ms | 0.1 ms | 0.1 ms |
| nested | 6.8 ms | 3.4 ms | 1.9 ms | 1.9 ms |
| unique | 3.2 ms | 1.6 ms | 1.7 ms | 1.7 ms |
| nested_big | 56.6 ms | 31.0 ms | 16.0 ms | 15.8 ms |

#### Save tree -> file (Source writes to a buffer, then fwrite)

| file | Source | P2CE | ours | ours -O3 native | ours vs Source | ours vs P2CE | output size Source / P2CE / ours |
|---|---|---|---|---|---|---|---|
| wide | 15.5 ms (175 MB/s) | 13.2 ms (205 MB/s) | 2.0 ms (1197 MB/s) | 1.6 ms (1522 MB/s) | 7.6x | 6.5x | 2.8 / 2.8 / 2.5 MB |
| nested | 55.5 ms (139 MB/s) | 41.4 ms (186 MB/s) | 10.0 ms (678 MB/s) | 7.8 ms (867 MB/s) | 5.5x | 4.1x | 8.1 / 8.1 / 7.1 MB |
| unique | 20.8 ms (300 MB/s) | 14.7 ms (425 MB/s) | 4.9 ms (1256 MB/s) | 3.8 ms (1639 MB/s) | 4.2x | 3.0x | 6.5 / 6.5 / 6.4 MB |
| nested_big | 431.2 ms (144 MB/s) | 338.5 ms (183 MB/s) | 83.4 ms (657 MB/s) | 65.4 ms (838 MB/s) | 5.2x | 4.1x | 65.1 / 65.1 / 57.5 MB |

#### Memory of one loaded tree (fresh process; heap = malloc in use, RSS = resident growth)

| file | file size | heap Source | heap P2CE | heap ours | RSS Source | RSS P2CE | RSS ours | peak RSS Source | peak RSS P2CE | peak RSS ours |
|---|---|---|---|---|---|---|---|---|---|---|
| wide | 2.8 MB | 10.7 MB (3.77x) | 10.9 MB (3.83x) | 8.9 MB (3.13x) | 12.9 MB | 16.9 MB | 8.0 MB | 15.5 MB | 16.9 MB | 9.7 MB |
| nested | 10.8 MB | 41.6 MB (3.86x) | 38.1 MB (3.53x) | 16.4 MB (1.52x) | 43.7 MB | 40.6 MB | 16.2 MB | 54.2 MB | 53.0 MB | 17.1 MB |
| unique | 7.9 MB | 22.4 MB (2.82x) | 22.8 MB (2.87x) | 27.4 MB (3.45x) | 26.6 MB | 27.4 MB | 23.4 MB | 34.3 MB | 35.6 MB | 24.8 MB |
| nested_big | 86.7 MB | 337.8 MB (3.90x) | 304.7 MB (3.51x) | 131.8 MB (1.52x) | 336.2 MB | 317.7 MB | 127.3 MB | 422.2 MB | 405.5 MB | 134.1 MB |

#### Lookups

| file | keys in the top object | top-level by name: Source | P2CE | ours | 6-key GetInt / GetString: Source | P2CE | ours | values read |
|---|---|---|---|---|---|---|---|---|
| nested | 50000 | 66064 ns | 77141 ns | 37 ns | 28 / 19 ns | 24 / 20 ns | 13 / 22 ns | same |
| wide | 100000 | 116766 ns | 74961 ns | 103 ns | - | - | - | - |

Source's default key-name table (4 MB) overflowed on: nested_big; its numbers for those files use KeyValues::SetUseGrowableStringTable(true).

#### Threads loading nested.kv at the same time (total MB/s; own tree per thread)

| threads | Source | ours (thread-safe build) | ours vs Source |
|---|---|---|---|
| 1 | 62 MB/s | 299 MB/s | 4.8x |
| 2 | 65 MB/s | 553 MB/s | 8.5x |
| 4 | 65 MB/s | 800 MB/s | 12.3x |
| 8 | 66 MB/s | 1011 MB/s | 15.4x |
| 16 | 64 MB/s | 829 MB/s | 13.0x |

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

## Portal 2: Community Edition

Portal 2: Community Edition (P2CE) is a modified CS:GO-branch Source engine. Its `KeyValues` differs from Source's
in a few places (number parsing, a real expression evaluator for `[...]` conditions, stricter error handling).
This section compares the **game's own binaries** with Source and with this library.

**How it was measured.** The `KeyValues` class of the game's own binaries was called directly by a local harness,
for one specific build of the game. Same generated files, same machine and the same measurements as above, a
separate run, best of 7.

Notes: the game's executable lists mimalloc before libc, so its `new`/`malloc` are mimalloc's; the harness links
the same way, and "heap" for P2CE is the sum of the used blocks of mimalloc (Source and ours: glibc `mallinfo2`).
The text P2CE writes is byte-for-byte what Source writes (checked on `nested` and `wide`). P2CE's `LoadFromBuffer`
has no escape-sequence switch that I could find; it behaves like Source here (see the probes).

> **Important: the P2CE measurement tools will not be published.** The harness and everything it needs
> are deliberately kept out of this repository (they are in `.gitignore`). They are tied to the game's
> proprietary binaries and publishing them would be harmful rather than useful. The P2CE numbers here are
> reported as results only and cannot be reproduced from this repository; the Source and "ours" numbers can
> (see *Reproduce*).

The P2CE columns are in the tables of the *Results* section above (Load, Destroy, Save, Memory, Lookups).

**What the numbers say.**

* **Load.** P2CE is 1.1x–1.4x faster than Source on `wide` and `nested` (117 vs 133 ms, 109 vs 149 ms), the same on
  `unique`, and **1.5x slower on `nested_big`** (2.00 s vs 1.37 s; Source needed its growable name table there). For both,
  the time per megabyte grows with the number of distinct key names (P2CE: `nested` 10 ms/MB, `nested_big` 23 ms/MB);
  I did not investigate the cause. This library is 3.9x–12.9x faster than P2CE on load.
* **Save.** P2CE is 1.2x–1.4x faster than Source; the output is byte-identical to Source's. This library is
  3.0x–6.5x faster than P2CE and writes up to 12% fewer bytes.
* **Memory.** On `nested` (repeated names and values) P2CE needs **8% less heap than Source** (3.5x the file size
  against 3.9x) and 2.3x more than this library (1.5x). With distinct strings (`unique`, `wide`) P2CE and Source are the
  same (2.9x / 3.8x); this library needs 22% more on `unique` and 17% less on `wide`. Peak RSS of P2CE is between 4% below and 9% above Source's.
* **Lookup by name.** The same design as Source (a linear walk over the siblings): 77 µs (P2CE) in a 50 000-key object
  and 75 µs in a 100 000-key one, against 66 µs and 117 µs for Source. In small objects all three are close: 24–28 ns for `GetInt` and 19–20 ns for `GetString` (ours: 13 / 22 ns).
* **Destroy.** About 2x faster than Source.

#### P2CE: behaviour on small probes

Every probe is loaded by the three libraries and the tree is compared (names lower-cased, numbers shown as
numbers). `returns` is what `LoadFromBuffer` / `LoadFromFile` returned.

| case | text | Source | P2CE | ours | compare |
|---|---|---|---|---|---|
| escape \n and \" in a quoted value | `"R" { s "x\ny" q "a\"b" z 1 }` | **returns true**<br>`r/s` string `x\\ny`<br>`r/q` string `a\\`<br>`r/b` string ` z 1 }` | **returns false**<br>`r/s` string `x\\ny`<br>`r/q` string `a\\`<br>`r/b` string ` z 1 }`<br>`r/ z 1 }` kv | **returns false** | P2CE differs Source; ours differs P2CE |
| backslash before the closing quote | `"R" { p "C:\games\" n 1 }` | **returns true**<br>`r/p` string `C:\\games\\`<br>`r/n` int `1` | **returns true**<br>`r/p` string `C:\\games\\`<br>`r/n` int `1` | **returns true**<br>`r/p` string `C:\\games\\`<br>`r/n` int `1` | P2CE = Source; ours = P2CE |
| /* block comment */ | `"R" { a 1 /* c */ b 2 }` | **returns true**<br>`r/a` int `1`<br>`r//*` string `c`<br>`r/*/` string `b`<br>`r/2` kv | **returns false**<br>`r/a` int `1`<br>`r//*` string `c`<br>`r/*/` string `b`<br>`r/2` kv | **returns true**<br>`r/a` int `1`<br>`r/b` int `2` | P2CE differs Source; ours differs P2CE |
| // inside an unquoted value | `"R" { w a//b n 1 }` | **returns true**<br>`r/w` string `a//b`<br>`r/n` int `1` | **returns true**<br>`r/w` string `a//b`<br>`r/n` int `1` | **returns false** | P2CE = Source; ours differs P2CE |
| // inside a quoted value | `"R" { w "a//b" n 1 }` | **returns true**<br>`r/w` string `a//b`<br>`r/n` int `1` | **returns true**<br>`r/w` string `a//b`<br>`r/n` int `1` | **returns true**<br>`r/w` string `a//b`<br>`r/n` int `1` | P2CE = Source; ours = P2CE |
| numbers: "5" "5.5" +5 0x10 12abc | `"R" { a "5" b "5.5" c +5 d 0x10 e 12abc }` | **returns true**<br>`r/a` int `5`<br>`r/b` float `5.5`<br>`r/c` int `5`<br>`r/d` string `0x10`<br>`r/e` string `12abc` | **returns true**<br>`r/a` int `5`<br>`r/b` float `5.5`<br>`r/c` int `5`<br>`r/d` string `0x10`<br>`r/e` string `12abc` | **returns true**<br>`r/a` int `5`<br>`r/b` float `5.5`<br>`r/c` int `5`<br>`r/d` string `0x10`<br>`r/e` string `12abc` | P2CE = Source; ours = P2CE |
| number out of int range | `"R" { big 3000000000 }` | **returns true**<br>`r/big` int `-1294967296` | **returns true**<br>`r/big` int `0` | **returns true**<br>`r/big` float `3e+09` | P2CE differs Source; ours differs P2CE |
| duplicate keys | `"R" { a 1 a 2 }` | **returns true**<br>`r/a` int `1`<br>`r/a` int `2` | **returns true**<br>`r/a` int `1`<br>`r/a` int `2` | **returns true**<br>`r/a` int `1`<br>`r/a` int `2` | P2CE = Source; ours = P2CE |
| keys differing in case | `"R" { Key 1 key 2 }` | **returns true**<br>`r/key` int `1`<br>`r/key` int `2` | **returns true**<br>`r/key` int `1`<br>`r/key` int `2` | **returns true**<br>`r/key` int `1`<br>`r/key` int `2` | P2CE = Source; ours = P2CE |
| condition [$X360] / [!$X360] | `"R" { a 1 [$X360] a 2 [!$X360] }` | **returns true**<br>`r/a` int `2` | **returns true**<br>`r/a` int `2` | **returns true**<br>`r/a` int `2` | P2CE = Source; ours = P2CE |
| condition [$LINUX] | `"R" { a 1 [$LINUX] b 2 [$WIN32] }` | **returns true**<br>`r/a` int `1`<br>`r/b` int `2` | **returns true**<br>`r/a` int `1` | **returns true**<br>`r/a` int `1` | P2CE differs Source; ours = P2CE |
| condition [$A && $B] (expression) | `"R" { a 1 [$LINUX && $POSIX] b 2 [$LINUX && $X360] }` | **returns true**<br>`r/a` int `1`<br>`r/[$linux` string `&&`<br>`r/$posix]` string `b`<br>`r/2` string `[$LINUX`<br>`r/&&` string `$X360]` | **returns true**<br>`r/a` int `1` | **returns true**<br>`r/a` int `1` | P2CE differs Source; ours = P2CE |
| condition with a blank [ $LINUX ] | `"R" { a 1 [ $LINUX ] }` | **returns true**<br>`r/a` int `1`<br>`r/[` string `$LINUX`<br>`r/]` kv | **returns true**<br>`r/a` int `1` | **returns true**<br>`r/a` int `1` | P2CE differs Source; ours = P2CE |
| empty key | `"R" { "" 1 b 2 }` | **returns true** | **returns false** | **returns false** | P2CE differs Source; ours = P2CE |
| missing closing brace | `"R" { a 1 b 2` | **returns true**<br>`r/a` int `1`<br>`r/b` int `2` | **returns false**<br>`r/a` int `1`<br>`r/b` int `2`<br>`r/2` kv | **returns false** | P2CE differs Source; ours differs P2CE |
| extra closing brace | `"R" { a 1 } }` | **returns true**<br>`r/a` int `1` | **returns false**<br>`r/a` int `1` | **returns false** | P2CE differs Source; ours differs P2CE |
| two root keys | `"A" { a 1 } "B" { b 2 }` | **returns true**<br>`a/a` int `1`<br>`b/b` int `2` | **returns true**<br>`a/a` int `1`<br>`b/b` int `2` | **returns true**<br>`a/a` int `1` | P2CE = Source; ours differs P2CE |
| #base before the root | `#base "x.txt" "R" { a 1 }` | **returns true**<br>`r/a` int `1` | **returns true**<br>`r/a` int `1` | **returns false** | P2CE = Source; ours differs P2CE |
| #include inside the root | `"R" { #include "x.txt" a 1 }` | **returns true**<br>`r/#include` string `x.txt`<br>`r/a` int `1` | **returns true**<br>`r/#include` string `x.txt`<br>`r/a` int `1` | **returns true**<br>`r/#include` string `x.txt`<br>`r/a` int `1` | P2CE = Source; ours = P2CE |
| key without a value | `"R" { a }` | **returns true**<br>`r/a` kv | **returns false**<br>`r/a` kv | **returns false** | P2CE differs Source; ours differs P2CE |
| unterminated quote | `"R" { a "abc }` | **returns true**<br>`r/a` string `abc }` | **returns false**<br>`r/a` string `abc }`<br>`r/abc }` kv | **returns false** | P2CE differs Source; ours differs P2CE |
| tab inside an unquoted token | `"R" { a b c 1 }` | **returns true**<br>`r/a` string `b`<br>`r/c` int `1` | **returns true**<br>`r/a` string `b`<br>`r/c` int `1` | **returns true**<br>`r/a` string `b`<br>`r/c` int `1` | P2CE = Source; ours = P2CE |

**What stands out.**

* P2CE keeps Source's tokenizer: `\` is an ordinary character (strings end at the next quote), `//` inside an
  unquoted value is kept, there is no `/* */`, several root keys are read, `#base` before the root is recognised, and
  `#include` inside a block is a plain key here only because the file does not exist.
* P2CE is **stricter than Source about errors**: an empty key, a missing or extra `}`, a key without a value and an
  unterminated quote make `LoadFromBuffer` return `false` (Source returns `true` and keeps what it read). It still
  leaves the part of the tree it read; this library returns `false` and an empty object.
* **Conditions** are a real expression evaluator in P2CE (`&&`, `||`, `!`, parentheses, blanks in `[ ... ]`), like this
  library, with a default symbol callback that knows `WIN32`, `WINDOWS`, `X360`, `PS3`, `OSX`, `LINUX`, `POSIX`,
  `GAMECONSOLE`, `DEMO` and `LOWVIOLENCE` (a null callback selects it). Source splits `[$LINUX && $POSIX]` into
  several string keys.
* **Numbers** are range-checked: `3000000000` does not fit and becomes the int `0` (Source: wraps to
  -1294967296; this library: a float). Everything else (`"5"`, `+5`, `0x10`, `12abc`) matches Source and this library.
* Duplicate keys and keys that differ only in case are kept side by side, as in Source (lookups ignore case through the symbol table).

## source-kv (Rust, SourceRS)

A comparison with the Rust crate `source-kv` (Load, Parse only, Destroy, Save, Memory, Lookups, Threads) lives in its
own folder: [bench/source-kv/](../source-kv/README.md) holds the crate, the Rust benchmark program, `run_sourcers.py` and the results.

## Reading the numbers

* **Load: 5.3x–13.0x faster.** On `nested` (typical data) 5.3x. The larger ratios on `wide` (12.5x) and `unique`
  (13.0x) come from a Source design limit: its key-name table (`CKeyValuesSystem`) has a fixed number of
  buckets with chains compared by `stricmp`, so with 100k+ distinct names every interning walks a long chain.
  Source also keeps the whole file in memory while parsing; ours streams.
* **Save: 4.2x–7.6x faster.** Outputs are not byte-equal: Source quotes every value and rewrites numbers
  (`7.11` -> `"7.110000"`); ours keeps the number text as read, unquoted, so ours writes up to 12% fewer bytes. Source also puts two tabs between a key and its value, ours one. Both indent with tabs and write the same number of lines.
* **Destroy: 1.9x–15x faster.**
* **Lookups by name in a large object: about 1000–2000x.** Source's `FindKey` walks the sibling list (66 µs with
  50 000 keys, 117 µs with 100 000); ours uses an index for objects over 16 keys (37 ns, 103 ns for
  100 000 keys, see the noise note). In small objects (6 keys), where games spend most of their time, `GetString` is
  19–22 ns for all three; `GetInt` is 13 ns for ours against 24–28 ns for Source and P2CE, because the number is kept
  in the entry and not parsed again.
* **Memory depends on the data.** On `nested` (repeated names and values) ours uses **2.5x less heap** (1.5x
  of the file size vs 3.9x) because every distinct string is stored once and a canonical number has no text at all; Source allocates a node and a separate
  string per value. On `unique` (every string distinct) **ours uses 22% more heap**: a pool entry,
  a hash bucket and a node cost more than Source's per-node string when there is nothing to deduplicate; on `wide` ours is 17% lower
  because a third of its values are numbers, which need no pooled text. Peak RSS is
  lower for ours on every file (up to 3x on `nested_big`), because Source holds the whole file in memory while parsing.
* **Threads.** Source cannot load two files at once: `LoadFromBuffer` parses into a process-wide static token
  buffer, and unsynchronised runs with 4 threads crashed. The Source column therefore serialises parsing with a
  mutex and stays at ~60 MB/s. Ours is thread-safe and 4.8x–15.4x faster in total and **scales up to 8 threads**
  (299 MB/s on one thread, 1011 MB/s on eight, 829 on 16 threads of the 8 cores): the parser keeps a small cache of
  recently seen strings in front of the pool, so repeated names and values never touch the shared pool or its lock;
  only strings seen for the first time take it.

## Memory of the library itself, per data type

`sizeof(CKeyValue)` is 40 bytes; an entry is 16 bytes (`nameSlot`, type, `hidden` flag, value slot plus the parsed number, or a child
pointer); the string pool stores each distinct string once (12 bytes of bookkeeping + the characters + a hash
bucket). Numbers are stored as the parsed `int` / `float` next to the value slot (in the same 16 bytes), so `Get*` does not parse; their text is stored in the pool only when it is not the canonical one (`+5`, `007`, `1.50`), otherwise it is made from the number. The
type of each value is decided at load time from the text, as Source does.

## Behaviour that is different from Source

Checked in the Source code (`tier1/KeyValues.cpp`), not on game files.

**Closed in this library (the same as Source now):** the type of a value comes from its text and ignores quotes
(`"5"` is an int, `+5` is an int); `[$WIN32]`-style conditions; escape sequences are **off by default** (`\` is an
ordinary character; `UsesEscapeSequences(true)` before loading turns them on); `//` starts a comment anywhere
outside quotes; an empty key is an error.

**Still different, can matter for foreign files:**

1. **Several root keys** in one file: Source reads them as siblings of one tree. Ours loads only one root into
   one object: the first one for an unnamed `CKeyValue`, or the first one with the object's own name (otherwise
   `root "name" not found`). The other roots are only checked for matching braces and are not stored.
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
9. **Writing**: Source indents with tabs, separates a key from its value with two tabs, quotes every value and rewrites numbers; ours indents with tabs, separates with one tab, keeps number text as read and does not quote numbers.
   With escapes off a quote inside a value cannot be written correctly (same as Source); with
   `UsesEscapeSequences(true)` the round trip is exact.
10. **Key names** are case-insensitive and stored in lower case (`GetName()` returns lower case); Source keeps the original case.
11. **Types**: no colour, pointer, 64-bit, wide-string types.

**Only in this library:** `UsesConditionals(bool)` (entries with a false condition are kept and hidden),
`SetCondition` expressions, `RemoveKey`/`RemoveKeys`, `GetBool`, `Children()`/`Children(name)` ranges, an index
for big objects, duplicate keys with first-wins `Get*`, a thread-safe global string pool.

## Where this library is weaker

* More heap than Source on files where almost every string is distinct and not numeric (`unique`: +22%).
* Parallel loading stops scaling beyond the physical cores, and strings seen for the first time still take the pool's lock (see *Threads*).
* No `#include`/`#base`, no several roots in one tree, or tolerant parsing (see the list above).
* Removing a key is O(n) (the duplicate chains and the name index are rebuilt).
