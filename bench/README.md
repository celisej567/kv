# String pool: designs compared

`kv_pool_bench [st|mt|all] [scale]` compares the real `KVStringPool` with the alternatives in
`pool_variants.h` (one template, one axis per cost). Memory is exact heap bytes (counting
`operator new`), time is best of 3. Numbers below: 8-core/16-thread CPU, 32 MB L3, GCC 16, `-O2`.

## Single thread (1M short keys, 300k 40–60 char paths, 2M interns of a 50k vocabulary)

| variant                         | B/key | B/path | peak  | intern key | intern path | intern dup | find | miss | view |
|---------------------------------|-------|--------|-------|------------|-------------|------------|------|------|------|
| old pool: ptr16, load 50%, vec  | 37.7  | 111.7  | 1.11  | 43         | 89          | 26         | 158  | 39   | 4.5  |
| **new pool** (ref12, load 75%)  | 33.5  | 97.7   | 1.13  | 44         | 89          | 29         | 166  | 34   | 5.2  |
| ref8 + hash in table, load 50%  | 37.7  | 111.7  | 1.00  | 45         | 96          | 25         | 141  | 34   | 1.5  |
| ref8 + hash in table, load 75%  | 37.7  | 97.7   | 1.22  | 43         | 82          | 27         | 141  | 30   | 1.4  |
| ref12, segmented entries        | 33.5  | 97.7   | 1.13  | 54         | 93          | 30         | 166  | 41   | 4.6  |
| ref12, segmented, fast hash     | 33.5  | 97.7   | 1.13  | 92         | 72          | 46         | 302  | 112  | 5.1  |

(times in ns/op; B = heap bytes per string including every array and the pool object)

## Threads (million ops/s, all threads together)

| variant                                   | thr | read (Find+View) | dup intern | unique intern |
|-------------------------------------------|-----|------------------|------------|---------------|
| old: one mutex                            | 1   | 4.2              | 22.8       | 18.8          |
|                                           | 8   | 1.4              | 7.1        | 4.0           |
| shared_mutex                              | 8   | 9.0              | 13.5       | 2.2           |
| **new: shared_mutex + lock-free View**    | 1   | 4.5              | 20.3       | 11.1          |
|                                           | 8   | 16.7             | 13.5       | 2.2           |

## Decisions

* **12-byte entries** (`{32-bit chunk ref, length, hash}` instead of a 16-byte pointer entry):
  −11% memory on short keys, no speed cost. Costs a fixed 8 KB chunk directory inside the pool
  object and a limit of 1024 chunks (4 GB of text; strings > 1 MB use one chunk each).
* **Hash table up to 75% full** (was 50%): up to −33% table memory, find/miss unchanged. Because the
  table size is a power of two the saving shows only at some sizes (300k: yes, 1M: no).
* **FNV-1a kept.** An 8-bytes-at-a-time hash is faster to compute and wins on long paths, but on
  short keys lookups got ~2x slower once the pool no longer fits in cache (probe-count statistics
  are identical; the loss is in memory access, not collisions). Short keys are the common case.
* **Hash in the table rejected**: −10% find time, but +4 bytes per bucket wipes out the memory win.
* **Thread-safe build only**: entries in segments that never move, `shared_mutex`, and `View`
  without any lock. Reads scale ~4x on 8 threads instead of collapsing to 0.3x; threads interning
  the same strings run 2x faster than with one mutex. The cost: inserting *new* strings from many
  threads is ~2x slower than with a plain mutex (double lookup + writer contention), and the
  single-thread speed of that build is a bit lower. The plain build keeps the `std::vector`
  (segment indexing makes `View` ~2 ns slower).
