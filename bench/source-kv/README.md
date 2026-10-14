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
| wide | 8.0 ms (338 MB/s) | 10.5 ms (259 MB/s) | 0.8x |
| nested | 41.3 ms (249 MB/s) | 30.1 ms (342 MB/s) | 1.4x |
| unique | 12.8 ms (592 MB/s) | 23.1 ms (327 MB/s) | 0.6x |
| nested_big | 342.6 ms (241 MB/s) | 266.9 ms (310 MB/s) | 1.3x |

## Parse only: source-kv `from_str::<IgnoredAny>` (the borrowed tape, no owned tree) vs our full load

| file | source-kv parse only | ours (full tree) | ours vs source-kv |
|---|---|---|---|
| wide | 2.3 ms (1167 MB/s) | 10.8 ms (251 MB/s) | 0.2x |
| nested | 11.6 ms (890 MB/s) | 29.7 ms (347 MB/s) | 0.4x |
| unique | 5.8 ms (1318 MB/s) | 23.6 ms (321 MB/s) | 0.2x |
| nested_big | 120.0 ms (689 MB/s) | 280.3 ms (295 MB/s) | 0.4x |

## Destroy tree

| file | source-kv | ours | ours vs source-kv |
|---|---|---|---|
| wide | 1.8 ms | 0.1 ms | 25.9x |
| nested | 9.9 ms | 2.0 ms | 5.0x |
| unique | 3.4 ms | 1.7 ms | 2.0x |
| nested_big | 83.8 ms | 16.1 ms | 5.2x |

## Save tree -> file (source-kv: `to_string`, then write)

| file | source-kv | ours | ours vs source-kv | output size source-kv / ours |
|---|---|---|---|---|
| wide | 1.9 ms (1368 MB/s) | 1.6 ms (1474 MB/s) | 1.1x | 2.7 / 2.5 MB |
| nested | 9.1 ms (789 MB/s) | 7.8 ms (878 MB/s) | 1.2x | 7.5 / 7.1 MB |
| unique | 4.6 ms (1327 MB/s) | 4.2 ms (1480 MB/s) | 1.1x | 6.4 / 6.4 MB |
| nested_big | 80.7 ms (717 MB/s) | 68.6 ms (799 MB/s) | 1.2x | 60.7 / 57.5 MB |

## Memory of one loaded tree (fresh process; heap = bytes the allocator holds live, RSS = resident growth)

| file | file size | heap source-kv | heap ours | RSS source-kv | RSS ours |
|---|---|---|---|---|---|
| wide | 2.8 MB | 7.6 MB (2.67x) | 8.9 MB (3.13x) | 12.1 MB | 8.8 MB |
| nested | 10.8 MB | 28.7 MB (2.66x) | 16.4 MB (1.52x) | 52.5 MB | 16.3 MB |
| unique | 7.9 MB | 15.8 MB (1.99x) | 27.4 MB (3.45x) | 21.7 MB | 23.1 MB |
| nested_big | 86.7 MB | 229.9 MB (2.65x) | 131.8 MB (1.52x) | 419.3 MB | 127.3 MB |

## Lookups (source-kv has no typed values: its GetInt is `get_string` + `parse::<i64>`)

| file | keys in the top object | top-level by name: source-kv | ours | 6-key object GetInt / GetString: source-kv | ours | values read |
|---|---|---|---|---|---|---|
| nested | 50000 | 144810 ns | 40 ns | 12 / 5 ns | 13 / 21 ns | same |
| wide | 100000 | 191996 ns | 126 ns | - | - | - |

## Threads loading nested.kv at the same time (total MB/s; own tree per thread)

| threads | source-kv | ours (thread-safe build) | ours vs source-kv |
|---|---|---|---|
| 1 | 240 MB/s | 287 MB/s | 1.2x |
| 2 | 257 MB/s | 557 MB/s | 2.2x |
| 4 | 303 MB/s | 880 MB/s | 2.9x |
| 8 | 310 MB/s | 872 MB/s | 2.8x |
| 16 | 393 MB/s | 943 MB/s | 2.4x |

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
