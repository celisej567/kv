# UTF-16 conversion: A/B load timing

A file that starts with a UTF-16 BOM is converted to UTF-8 while it is read (see
[docs/DETAILS.md](../../docs/DETAILS.md), *Text encoding*). This script measures what that costs.

* **A** = `kv_cli` built from the code before the conversion (UTF-8 only; no NUL check, as in B)
* **B** = `kv_cli` with the conversion

The same generated files (`tests/gen_kv.py`) are loaded alternately by A and B, best of N, so drift of the machine
hits both. `kv_cli bench` times one `LoadFromFile` in a fresh process, so the numbers include a cold string pool and
are slower than the warm "best of 7 in one process" tables elsewhere; compare A with B, not with those tables.

## Reproduce

    git worktree add /tmp/wt-a <commit before the conversion>   # (without any NUL check, to compare like with like)
    cmake -S /tmp/wt-a -B /tmp/b-a -DCMAKE_BUILD_TYPE=Release && cmake --build /tmp/b-a -j --target kv_cli
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j --target kv_cli
    python3 -I bench/utf16/run_utf16.py --a /tmp/b-a/kv_cli --b build/kv_cli [--runs 9] [--quick] > result.md

## Result





### UTF-8 files: before (A) and after (B) the change

| file | size | A | B | B vs A |
|---|---|---|---|---|
| wide | 2.8 MB | 16.2 ms (175 MB/s) | 16.0 ms (178 MB/s) | 1.02x |
| nested | 10.8 MB | 33.8 ms (319 MB/s) | 35.1 ms (308 MB/s) | 0.96x |
| unique | 7.9 MB | 33.9 ms (235 MB/s) | 33.7 ms (236 MB/s) | 1.01x |
| nested_big | 86.7 MB | 284.5 ms (305 MB/s) | 290.9 ms (298 MB/s) | 0.98x |

### UTF-16 files (with BOM) loaded by B, against the same text as UTF-8

| file | UTF-16 size | UTF-8 | UTF-16 LE | UTF-16 BE | LE vs UTF-8 |
|---|---|---|---|---|---|
| wide | 5.7 MB | 16.0 ms | 21.1 ms | 20.9 ms | 0.76x |
| nested | 21.6 MB | 35.1 ms | 53.0 ms | 53.3 ms | 0.66x |
| unique | 15.9 MB | 33.7 ms | 47.2 ms | 48.2 ms | 0.71x |
| nested_big | 173.4 MB | 290.9 ms | 443.9 ms | 464.6 ms | 0.66x |

* **UTF-8 files: B is within noise of A** (0.96-1.02x; `nested` is the slowest at 4 %, `wide` and `unique` are even slightly
  faster in this run). The BOM test and the converting branch cost nothing measurable for files without a UTF-16 BOM.
* **UTF-16 files load in about 1.3-1.5 times the time of the same text in UTF-8** (0.66-0.76x speed), although they
  are twice as large on disk: the extra time works out at about 1 GB/s of UTF-16 input. BE is a few percent slower than LE.
