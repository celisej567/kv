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
| wide | 11.4 ms | 20.1 ms | 0.4x |
| nested | 28.6 ms | 39.0 ms | 1.1x |
| unique | 19.9 ms | 42.5 ms | 0.3x |
| nested_big | 259 ms | 310 ms | 1.2x |

(source-kv column of "first load vs source-kv": the crate's best-of-7 time from the table below. Measured by hand, not by the script.)

## Load file -> tree (best of 7, page cache warm)

| file | source-kv | ours | ours vs source-kv |
|---|---|---|---|
| wide | 8.2 ms (329 MB/s) | 11.4 ms (238 MB/s) | 0.7x |
| nested | 41.4 ms (248 MB/s) | 28.6 ms (360 MB/s) | 1.4x |
| unique | 12.7 ms (596 MB/s) | 19.9 ms (382 MB/s) | 0.6x |
| nested_big | 367.9 ms (225 MB/s) | 259.1 ms (319 MB/s) | 1.4x |

## Parse only: source-kv `from_str::<IgnoredAny>` (the borrowed tape, no owned tree) vs our full load

| file | source-kv parse only | ours (full tree) | ours vs source-kv |
|---|---|---|---|
| wide | 2.2 ms (1236 MB/s) | 11.5 ms (236 MB/s) | 0.2x |
| nested | 11.2 ms (918 MB/s) | 27.3 ms (377 MB/s) | 0.4x |
| unique | 6.3 ms (1202 MB/s) | 20.9 ms (363 MB/s) | 0.3x |
| nested_big | 120.0 ms (689 MB/s) | 245.5 ms (337 MB/s) | 0.5x |

## Destroy tree

| file | source-kv | ours | ours vs source-kv |
|---|---|---|---|
| wide | 1.7 ms | 0.1 ms | 15.3x |
| nested | 10.6 ms | 1.9 ms | 5.5x |
| unique | 3.3 ms | 1.7 ms | 1.9x |
| nested_big | 84.0 ms | 16.4 ms | 5.1x |

## Save tree -> file (source-kv: `to_string`, then write)

| file | source-kv | ours | ours vs source-kv | output size source-kv / ours |
|---|---|---|---|---|
| wide | 2.0 ms (1294 MB/s) | 2.4 ms (1008 MB/s) | 0.8x | 2.7 / 2.5 MB |
| nested | 9.4 ms (761 MB/s) | 10.4 ms (655 MB/s) | 0.9x | 7.5 / 7.1 MB |
| unique | 4.9 ms (1250 MB/s) | 5.8 ms (1067 MB/s) | 0.9x | 6.4 / 6.4 MB |
| nested_big | 81.1 ms (714 MB/s) | 81.7 ms (671 MB/s) | 1.0x | 60.7 / 57.5 MB |

## Memory of one loaded tree (fresh process; heap = bytes the allocator holds live, RSS = resident growth)

| file | file size | heap source-kv | heap ours | RSS source-kv | RSS ours |
|---|---|---|---|---|---|
| wide | 2.8 MB | 7.6 MB (2.67x) | 8.9 MB (3.13x) | 12.0 MB | 8.0 MB |
| nested | 10.8 MB | 28.7 MB (2.66x) | 16.4 MB (1.52x) | 52.5 MB | 16.2 MB |
| unique | 7.9 MB | 15.8 MB (1.99x) | 27.4 MB (3.45x) | 21.6 MB | 23.2 MB |
| nested_big | 86.7 MB | 229.9 MB (2.65x) | 131.8 MB (1.52x) | 419.3 MB | 127.3 MB |

## Lookups (source-kv has no typed values: its GetInt is `get_string` + `parse::<i64>`)

| file | keys in the top object | top-level by name: source-kv | ours | 6-key object GetInt / GetString: source-kv | ours | values read |
|---|---|---|---|---|---|---|
| nested | 50000 | 135778 ns | 46 ns | 10 / 5 ns | 12 / 19 ns | same |
| wide | 100000 | 191695 ns | 82 ns | - | - | - |

## Threads loading nested.kv at the same time (total MB/s; own tree per thread)

| threads | source-kv | ours (thread-safe build) | ours vs source-kv |
|---|---|---|---|
| 1 | 245 MB/s | 284 MB/s | 1.2x |
| 2 | 294 MB/s | 593 MB/s | 2.0x |
| 4 | 289 MB/s | 1074 MB/s | 3.7x |
| 8 | 309 MB/s | 1587 MB/s | 5.1x |
| 16 | 388 MB/s | 1464 MB/s | 3.8x |

## Summary

* **Load.** Ours is faster on `nested` and `nested_big` (1.4x with a warm pool, 1.1x–1.2x on a first load) and slower on
  `wide` (0.7x; 0.4x on a first load) and `unique` (0.6x; 0.3x on a first load). The crate's parse-only time is
  2.0x–5.2x lower than our full load.
* **Destroy.** Ours is 1.9x–15x faster.
* **Save.** Ours is 0.8x–1.0x (slower on the smaller files, equal on `nested_big`), with files up to 5% smaller.
  Ours escapes every name and value it writes, which costs a scan of each string.
* **Memory.** Ours uses less heap on `nested` and `nested_big` and more on `wide` and `unique`.
* **Lookups.** In a big object: 46–82 ns for ours against 136–192 µs for the crate. In a small object `GetInt` is
  12 ns for ours against 10 ns, and `GetString` is 19 ns for ours against 5 ns.
* **Threads.** Ours loads 1.2x–5.1x more megabytes per second at 1–16 threads.
