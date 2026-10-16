# kv

A C++17 key/value parser and editor for Valve's KeyValue-style files
(`"key" "value"`, nested `{ }` blocks, `//` comments). Partially assisted by Claude.

It is **not** a drop-in replacement for the Source Engine `KeyValues` class and does not aim at
compatibility with it. It is a parody of it: the same kind of files and a similar API, with its own
design and with differences in behaviour (see [Differences from Source](#differences-from-source)).

## What it does

- Reads and writes KV text files: `//` and `/* ... */` comments, quoted and unquoted tokens, nested blocks. A `/*`
  starts a comment only at the start of a token, so a value like `hl2/custom/*` is kept as it is.
- Types are decided from the text of a value, quotes don't matter: an integer is `Int`, a decimal number
  is `Float`, anything else is `String` (`SetString` uses the same rule). Numbers are written back as read and also keep their parsed value, so `GetInt` / `GetFloat` don't parse again. The text of a number is only stored when it is not the canonical one (`+5`, `007`, `1.50`, `1e3`); otherwise it is made from the value.
- A file may hold several root objects. `CKeyValue kv;` (unnamed) loads the first and takes its name;
  `CKeyValue kv("name");` loads the first root with that name (case-insensitive), and `LoadFromFile` fails with
  `root "name" not found` if there is none. The other roots are only checked for matching braces.
- `GetString`, `GetInt`, `GetFloat`, `GetBool` and `GetKeyValue` with defaults. `GetInt` on a float
  truncates toward zero; `GetBool` accepts numbers and `true`/`false`.
- `SetString/Int/Float/KeyValue` (replace the first entry of a name or append), `AddString/Int/Float/KeyValue`
  (always append), `RemoveKey`/`RemoveKeys`, `Clear`.
- Duplicate keys (like repeated `Game` lines in a `gameinfo.txt`), `Children()` / `Children(name)` ranges
  for a range-for, `Count`, `GetStrings`, `GetKeyValues`.
- Key names are case-insensitive and stored (and written) in lower case. Values are kept as they are.
- Conditions: `"key" "value" [$LINUX]`, `[!$X360]`, `[$A && ($B || !$C)]`, also after a block name or the
  root name. The table of names is extensible with `CKeyValue::SetCondition`. Entries with a false
  condition are loaded and kept, and hidden by default; `UsesConditionals(false)` shows them.
- Optional backslash escapes in quoted text with `UsesEscapeSequences(true)` (off by default).
- One global pool of unique strings: every distinct name or value is stored once, and the tree holds only
  slot numbers and types. The pool is optionally thread-safe (`KV_THREAD_SAFE`).
- Streaming parser (a 256 KB sliding window), so peak memory is the tree and not tree plus file. Errors are
  reported as `file:line:col: message`.

```cpp
#include "keyvalue.h"

CKeyValue kv;  // unnamed: takes the name of the first root; CKeyValue kv("game") picks that root
if (!kv.LoadFromFile("game.txt")) return 1;

int width = kv.GetInt("width", 1280);
bool windowed = kv.GetBool("windowed", true);
for (const auto c : kv.Children("path"))
    std::printf("%s\n", c.GetString().c_str());

kv.SetFloat("scale", 1.5f);
kv.RemoveKey("obsolete");
kv.WriteToFile("game_out.txt");
```

The behaviour of every function and the non-obvious cases are described in [docs/DETAILS.md](docs/DETAILS.md).

## Build

Requires a C++17 compiler and CMake 3.14 or newer.

```
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure
```

To use it in your project:

```cmake
add_subdirectory(kv)
target_link_libraries(my_app PRIVATE kv)
```

then `#include "keyvalue.h"`.

The tests (`ctest`) run `tests/run_tests.py` with the Python 3 that CMake finds; they work on Linux, macOS and Windows,
from any directory and any location of the repository (they use only temporary directories). To run them by hand:
`python tests/run_tests.py <path to kv_cli>` (`python3` on Linux and macOS). The benchmark scripts in `bench/` build
programs with GCC/Clang flags and read Linux memory counters, so they need Linux (`CXX` selects the compiler).

### Presets

`CMakePresets.json` (CMake 3.21+); each preset builds in `build/<preset>`:

| preset | what it builds |
|---|---|
| `lib-static` | the library only, static (`libkv.a`) |
| `lib-shared` | the library only, shared (`libkv.so` / `kv.dll`) |
| `release`, `debug` | everything, with tests |
| `verbose` | everything, with `KV_VERBOSE` |
| `asan`, `tsan` | everything, with AddressSanitizer + UBSan / ThreadSanitizer |

```
cmake --preset lib-shared && cmake --build --preset lib-shared
cmake --preset release && cmake --build --preset release -j && ctest --preset release
```

### CMake options

| option | default | meaning |
|---|---|---|
| `KV_THREAD_SAFE` | `OFF` | Compiles the string pool with locks, so several threads can load different trees at the same time. A tree itself is still not safe to modify from several threads. |
| `KV_VERBOSE` | `OFF` | Detailed logging to `stderr` (`[kv] ...` lines: file, encoding, roots, conditions, sizes, time). Off by default: the library prints errors only. |
| `KV_BUILD_SHARED` | `OFF` | Builds `kv` as a shared library (`libkv.so` / `kv.dll`) instead of a static one. |
| `KV_BUILD_TOOLS` | `ON` | Builds `kv_cli`, the stress/bench tools, the example and the tests. `OFF` builds the library only. |
| `KV_SANITIZE` | `OFF` | AddressSanitizer + UBSan (GCC/Clang). |
| `KV_TSAN` | `OFF` | ThreadSanitizer (GCC/Clang). Cannot be combined with `KV_SANITIZE`. |

`CMAKE_BUILD_TYPE` defaults to `Release`. The `kv_mt` target is the same library with `KV_THREAD_SAFE` always on
(used by the multithreading tests). Other targets: `kv_cli` (dump/write/get/bench tool), `kv_stress`,
`kv_pool_bench`, `kv_example`.

## Speed compared to Source Engine and P2CE

The same files loaded by `KeyValues::LoadFromBuffer` from [nillerusr/source-engine](https://github.com/nillerusr/source-engine),
by the `KeyValues` code inside Portal 2: Community Edition (P2CE, called through the game's own binaries; this harness is
local and not in the repository) and by this library (`LoadFromFile`), built with the same compiler flags as Source uses (`-O2 -march=core2
-ffast-math`). Best of 7, page cache warm. AMD Ryzen 7 5800X, GCC 16.2, Linux.

The test files are generated by `tests/gen_kv.py`, not stored:

- **wide**: one object with 100 000 flat keys (integers, floats and quoted strings). Worst case for lookup by
  name in one object. 2.8 MB.
- **nested**: 50 000 objects with 6 keys and a nested `pos` object each; values come from a vocabulary of 200 words. The closest to
  real game data: many small objects, few distinct names and values. 10.8 MB.
- **unique**: 100 000 objects whose keys and values are all distinct. Worst case for a string pool, nothing to
  deduplicate. 7.9 MB.
- **nested_big**: the same as `nested`, 8 times larger. 86.7 MB.

### Load file -> tree

| file | Source | P2CE | ours | ours vs Source | ours vs P2CE |
|---|---|---|---|---|---|
| wide | 134.4 ms (20 MB/s) | 119.6 ms (23 MB/s) | 11.4 ms (238 MB/s) | 11.8x | 10.5x |
| nested | 148.1 ms (70 MB/s) | 109.3 ms (94 MB/s) | 28.6 ms (359 MB/s) | 5.2x | 3.8x |
| unique | 266.2 ms (28 MB/s) | 272.0 ms (28 MB/s) | 21.2 ms (358 MB/s) | 12.6x | 12.9x |
| nested_big | 1446.9 ms (57 MB/s) | 1966.8 ms (42 MB/s) | 271.0 ms (305 MB/s) | 5.3x | 7.3x |

### Save tree -> file

| file | Source | P2CE | ours | ours vs Source | ours vs P2CE | output size Source / P2CE / ours |
|---|---|---|---|---|---|---|
| wide | 15.6 ms (174 MB/s) | 12.9 ms (211 MB/s) | 1.9 ms (1241 MB/s) | 8.0x | 6.6x | 2.8 / 2.8 / 2.5 MB |
| nested | 54.3 ms (142 MB/s) | 41.4 ms (186 MB/s) | 10.1 ms (677 MB/s) | 5.4x | 4.1x | 8.1 / 8.1 / 7.1 MB |
| unique | 20.9 ms (298 MB/s) | 14.8 ms (422 MB/s) | 5.0 ms (1222 MB/s) | 4.2x | 2.9x | 6.5 / 6.5 / 6.4 MB |
| nested_big | 432.1 ms (144 MB/s) | 338.2 ms (184 MB/s) | 85.9 ms (638 MB/s) | 5.0x | 3.9x | 65.1 / 65.1 / 57.5 MB |

Our files are smaller than Source's because numbers are written as they were read, without quotes (Source quotes them and rewrites `7.11` as `"7.110000"`). Both indent with tabs; a key and its value are separated by one tab in ours and two tabs in Source.

### Lookup by name

| file | keys in the top object | Source | P2CE | ours | 6-key `GetInt` / `GetString`: Source | P2CE | ours |
|---|---|---|---|---|---|---|---|
| nested | 50000 | 51227 ns | 52922 ns | 39 ns | 23 / 19 ns | 25 / 20 ns | 13 / 23 ns |
| wide | 100000 | 127334 ns | 79246 ns | 101 ns | - | - | - |

In a big object ours uses an index (built for objects over 16 keys), Source walks the list of siblings. In small
objects `GetString` takes about the same time in all three (19–22 ns), and `GetInt` is faster in ours (13 ns against 24–28 ns) because the parsed number is kept in the entry. P2CE (a CS:GO-branch Source) behaves like Source here; its
loading is a little faster on small trees and slower on the largest one. The `wide` number for ours varies from run to run on the test machine
(cache misses on 100 000 keys).

Memory, destroy time, multithreaded loading, a longer explanation of the numbers and how to reproduce them:
[bench/compare/README.md](bench/compare/README.md).

## Differences from Source

The main ones (full list in [bench/compare/README.md](bench/compare/README.md)):

- `/* */` comments are supported here (Source has only `//`).
- A file may hold several roots, but an object loads only one of them (see above) and the others are not stored;
  `#include` and `#base` are not processed.
- Any parse error fails the whole load (returns `false`, the object is left empty); Source keeps what it read.
- `GetInt` / `GetFloat` on a non-numeric string return the default; Source applies `atoi` / `atof`.
- Integers outside the `int` range become floats.
- Conditions are real expressions over an extensible table; `[$OSX]` is true on macOS, and there is no
  `LINUX` condition there.
- Key names are lower-cased; escape sequences are off by default.
- Written files use one tab per level and one tab between a key and its value, and keep number text as read.
- No colour, pointer, 64-bit or wide-string types.
