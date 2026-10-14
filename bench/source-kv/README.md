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

Ours is measured as best of 7 loads in one process, so its string pool already holds the strings from earlier loads.
The first load in a fresh process (median of 9 processes, ours only) is slower than the table above:

| file | ours, best of 7 (warm pool) | ours, first load in a process | first load vs source-kv |
|---|---|---|---|
| wide | 11.1 ms | 18.0 ms | 0.4x |
| nested | 28.3 ms | 37.3 ms | 1.0x |
| unique | 21.8 ms | 44.4 ms | 0.3x |
| nested_big | 268 ms | 320 ms | 1.0x |

(source-kv column of "first load vs source-kv": the crate's best-of-7 time from the table below. Measured by hand, not by the script.)

## Load file -> tree (best of 7, page cache warm)

| file | source-kv | ours | ours vs source-kv |
|---|---|---|---|
| wide | 7.9 ms (344 MB/s) | 11.3 ms (240 MB/s) | 0.7x |
| nested | 38.7 ms (266 MB/s) | 28.0 ms (367 MB/s) | 1.4x |
| unique | 12.6 ms (601 MB/s) | 21.7 ms (349 MB/s) | 0.6x |
| nested_big | 346.6 ms (238 MB/s) | 270.7 ms (306 MB/s) | 1.3x |

## Parse only: source-kv `from_str::<IgnoredAny>` (the borrowed tape, no owned tree) vs our full load

| file | source-kv parse only | ours (full tree) | ours vs source-kv |
|---|---|---|---|
| wide | 1.9 ms (1387 MB/s) | 11.0 ms (246 MB/s) | 0.2x |
| nested | 10.7 ms (959 MB/s) | 28.8 ms (358 MB/s) | 0.4x |
| unique | 5.7 ms (1328 MB/s) | 21.9 ms (346 MB/s) | 0.3x |
| nested_big | 120.1 ms (688 MB/s) | 270.1 ms (306 MB/s) | 0.4x |

## Destroy tree

| file | source-kv | ours | ours vs source-kv |
|---|---|---|---|
| wide | 1.6 ms | 0.1 ms | 22.1x |
| nested | 9.8 ms | 1.9 ms | 5.3x |
| unique | 3.3 ms | 1.7 ms | 2.0x |
| nested_big | 82.5 ms | 16.8 ms | 4.9x |

## Save tree -> file (source-kv: `to_string`, then write)

| file | source-kv | ours | ours vs source-kv | output size source-kv / ours |
|---|---|---|---|---|
| wide | 1.9 ms (1371 MB/s) | 1.7 ms (1427 MB/s) | 1.1x | 2.7 / 2.5 MB |
| nested | 8.7 ms (828 MB/s) | 8.1 ms (845 MB/s) | 1.1x | 7.5 / 7.1 MB |
| unique | 4.6 ms (1332 MB/s) | 3.8 ms (1608 MB/s) | 1.2x | 6.4 / 6.4 MB |
| nested_big | 78.6 ms (737 MB/s) | 68.8 ms (797 MB/s) | 1.1x | 60.7 / 57.5 MB |

## Memory of one loaded tree (fresh process; heap = bytes the allocator holds live, RSS = resident growth)

| file | file size | heap source-kv | heap ours | RSS source-kv | RSS ours |
|---|---|---|---|---|---|
| wide | 2.8 MB | 7.6 MB (2.67x) | 8.9 MB (3.13x) | 12.1 MB | 8.1 MB |
| nested | 10.8 MB | 28.7 MB (2.66x) | 16.4 MB (1.52x) | 52.5 MB | 16.3 MB |
| unique | 7.9 MB | 15.8 MB (1.99x) | 27.4 MB (3.45x) | 21.7 MB | 24.0 MB |
| nested_big | 86.7 MB | 229.9 MB (2.65x) | 131.8 MB (1.52x) | 419.3 MB | 127.7 MB |

## Lookups (source-kv has no typed values: its GetInt is `get_string` + `parse::<i64>`)

| file | keys in the top object | top-level by name: source-kv | ours | 6-key object GetInt / GetString: source-kv | ours | values read |
|---|---|---|---|---|---|---|
| nested | 50000 | 133217 ns | 37 ns | 12 / 5 ns | 12 / 19 ns | same |
| wide | 100000 | 182507 ns | 100 ns | - | - | - |

## Threads loading nested.kv at the same time (total MB/s; own tree per thread)

| threads | source-kv | ours (thread-safe build) | ours vs source-kv |
|---|---|---|---|
| 1 | 238 MB/s | 304 MB/s | 1.3x |
| 2 | 259 MB/s | 568 MB/s | 2.2x |
| 4 | 302 MB/s | 917 MB/s | 3.0x |
| 8 | 307 MB/s | 868 MB/s | 2.8x |
| 16 | 399 MB/s | 944 MB/s | 2.4x |

## Summary

* **Load.** Ours is faster on `nested` and `nested_big` (1.4x with a warm pool, 1.0x on a first load) and slower on
  `wide` (0.7x; 0.4x on a first load) and `unique` (0.6x; 0.3x on a first load). The crate's parse-only time is
  2.1x–4.5x lower than our full load.
* **Destroy.** Ours is 2x–22x faster.
* **Save.** Ours is 1.1x–1.2x faster, with files up to 5% smaller.
* **Memory.** Ours uses less heap on `nested` and `nested_big` and more on `wide` and `unique`.
* **Lookups.** In a big object: 39–109 ns for ours against 130–180 µs for the crate. In a small object `GetInt` is equal
  (12 ns) and `GetString` is 19 ns for ours against 5 ns.
* **Threads.** Ours loads 1.3x–3.1x more megabytes per second at 1–16 threads.
