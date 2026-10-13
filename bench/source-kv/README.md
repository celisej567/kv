# This library vs source-kv (Rust)

[`source-kv`](https://github.com/LaVashikk/SourceRS/tree/main/crates/source-kv) is a serde-based KeyValues crate
(`crates/source-kv` of SourceRS). This folder holds the benchmark that compares it with this library; the crate itself is not stored here.

    crate/             put the crate here (not part of this repository, git ignores it; see crate/README.md)
    bench/             the Rust benchmark program (same commands and output keys as ../compare/kv_compare.cpp)
    run_sourcers.py    builds both sides, runs them on the same generated files, prints the tables below

    python3 -I bench/source-kv/run_sourcers.py [--crate=DIR] [--src=DIR] [--quick] > result.md

Needs `cargo` and the crate: copy `crates/source-kv` of [SourceRS](https://github.com/LaVashikk/SourceRS) into `crate/`
(or pass another checkout with `--crate`). Nothing in it is modified, the script builds a patched copy in a temp directory.
`--src` takes another copy of our `src/`, to compare an older state of this library.

## What is measured

Both sides load the same generated files (`tests/gen_kv.py`: `wide`, `nested`, `unique`, `nested_big`), best of 7,
page cache warm. Ours is built with `g++ -O3 -march=native`, the crate with `opt-level=3`, LTO and
`-C target-cpu=native`. The Rust side parses with `Deserializer::parse_root` into the untyped `Value` tree, writes with
`to_string`, and counts memory with a counting global allocator. `from_str::<Value>` is not used: the crate builds
a borrowed tape first and the owned tree from it, so "parse only" (`from_str::<IgnoredAny>`) is shown separately.

What differs between the two, which matters when reading the numbers:

* every value in `source-kv` is a `String` (no Int/Float: its "GetInt" is `get_string` plus `parse::<i64>`);
* objects are plain `Vec<(String, Value)>` with an owned `String` per key and value: no string pool, no name
  index, so a lookup scans the object;
* no conditionals, escapes are kept as they are, keys are compared case-insensitively on every lookup;
* its writer uses tabs, ours 4 spaces (ours writes up to 45% more bytes for the same tree);
* the first thing the crate does is read the whole file into a `String`; ours streams through a 256 KB window.

Our "best of 7" loads the same file several times in one process, so the string pool already holds every string from
the second load on. The crate has no such state. A first load in a fresh process is slower than the table says
for ours (roughly 1.2–1.4x on these files).

## Load file -> tree (best of 7, page cache warm)

| file | source-kv | ours | ours vs source-kv |
|---|---|---|---|
| wide | 8.4 ms (322 MB/s) | 10.5 ms (258 MB/s) | 0.8x |
| nested | 41.4 ms (249 MB/s) | 30.7 ms (336 MB/s) | 1.3x |
| unique | 12.0 ms (632 MB/s) | 22.9 ms (331 MB/s) | 0.5x |
| nested_big | 352.7 ms (234 MB/s) | 273.3 ms (302 MB/s) | 1.3x |

## Parse only: source-kv `from_str::<IgnoredAny>` (the borrowed tape, no owned tree) vs our full load

| file | source-kv parse only | ours (full tree) | ours vs source-kv |
|---|---|---|---|
| wide | 2.0 ms (1324 MB/s) | 12.4 ms (219 MB/s) | 0.2x |
| nested | 11.4 ms (905 MB/s) | 32.2 ms (320 MB/s) | 0.4x |
| unique | 6.1 ms (1238 MB/s) | 21.9 ms (345 MB/s) | 0.3x |
| nested_big | 122.4 ms (675 MB/s) | 297.3 ms (278 MB/s) | 0.4x |

## Destroy tree

| file | source-kv | ours | ours vs source-kv |
|---|---|---|---|
| wide | 1.7 ms | 0.1 ms | 21.2x |
| nested | 10.2 ms | 1.9 ms | 5.3x |
| unique | 3.3 ms | 1.7 ms | 1.9x |
| nested_big | 86.8 ms | 16.5 ms | 5.3x |

## Save tree -> file (source-kv: `to_string`, then write)

| file | source-kv | ours | ours vs source-kv | output size source-kv / ours |
|---|---|---|---|---|
| wide | 2.0 ms (1251 MB/s) | 1.7 ms (1600 MB/s) | 1.2x | 2.7 / 2.8 MB |
| nested | 9.7 ms (740 MB/s) | 9.3 ms (1112 MB/s) | 1.0x | 7.5 / 10.9 MB |
| unique | 4.9 ms (1252 MB/s) | 4.2 ms (1823 MB/s) | 1.2x | 6.4 / 7.9 MB |
| nested_big | 82.5 ms (702 MB/s) | 80.4 ms (1038 MB/s) | 1.0x | 60.7 / 87.5 MB |

## Memory of one loaded tree (fresh process; heap = bytes the allocator holds live, RSS = resident growth)

| file | file size | heap source-kv | heap ours | RSS source-kv | RSS ours |
|---|---|---|---|---|---|
| wide | 2.8 MB | 7.6 MB (2.67x) | 8.9 MB (3.13x) | 12.1 MB | 8.7 MB |
| nested | 10.8 MB | 28.7 MB (2.66x) | 16.4 MB (1.52x) | 52.5 MB | 16.2 MB |
| unique | 7.9 MB | 15.8 MB (1.99x) | 27.4 MB (3.45x) | 21.7 MB | 24.0 MB |
| nested_big | 86.7 MB | 229.9 MB (2.65x) | 131.8 MB (1.52x) | 419.2 MB | 127.7 MB |

## Lookups (source-kv has no typed values: its GetInt is `get_string` + `parse::<i64>`)

| file | keys in the top object | top-level by name: source-kv | ours | 6-key object GetInt / GetString: source-kv | ours | values read |
|---|---|---|---|---|---|---|
| nested | 50000 | 139551 ns | 51 ns | 12 / 5 ns | 15 / 21 ns | same |
| wide | 100000 | 192418 ns | 103 ns | - | - | - |

## Threads loading nested.kv at the same time (total MB/s; own tree per thread)

| threads | source-kv | ours (thread-safe build) | ours vs source-kv |
|---|---|---|---|
| 1 | 231 MB/s | 270 MB/s | 1.2x |
| 2 | 249 MB/s | 526 MB/s | 2.1x |
| 4 | 296 MB/s | 776 MB/s | 2.6x |
| 8 | 306 MB/s | 926 MB/s | 3.0x |
| 16 | 394 MB/s | 831 MB/s | 2.1x |

## Reading the numbers

* **Load.** Faster than the crate on `nested` and `nested_big` (1.3x), slower on `wide` (0.8x) and `unique` (0.5x).
  Where strings repeat, the pool makes a copy of every string unnecessary; where all strings are new, every one pays
  for the pool (hash, probe, store, table growth) and the crate only calls `malloc`. Parsing alone (the crate's tape,
  no owned tree) is 3–5x faster than our full load because it copies nothing and keeps no tables.
* **Destroy.** 2–25x faster: a few vectors instead of one allocation per string.
* **Save.** About equal to the crate (1.0–1.2x) although ours writes more bytes.
* **Memory.** Lower on `nested`/`nested_big` (repeated strings are stored once, numbers have no text) and higher on
  `wide`/`unique`, where strings do not repeat and the pool adds its entry, table slot and node. The crate also holds
  the whole file and the tape while it parses: its peak is several times the file size, ours is the tree plus 256 KB.
* **Lookups.** Finding a key in a big object takes ~50–100 ns for ours (hash index) and 140–190 µs for the crate
  (a scan). In a small object `GetInt` is about the same (the number is parsed once on load in ours, on every call in the
  crate) and `GetString` is 4x slower in ours (21 vs 5 ns): the name is hashed and looked up in the pool before the six entries
  are scanned.
* **Threads.** Our thread-safe build loads 2–3x more megabytes per second at 2–16 threads: a small per-thread cache
  in front of the pool means repeated strings never touch the shared pool.
