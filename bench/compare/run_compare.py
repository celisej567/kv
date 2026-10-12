#!/usr/bin/env python3
"""Source Engine KeyValues vs this library: builds both benchmark backends, runs them on the same
generated files and prints markdown tables.

  run_compare.py --source-engine=DIR [--quick]

DIR is a nillerusr/source-engine checkout configured and built with
  python3 waf configure -T release --build-games=hl2 --prefix=_BUILD --disable-warns
  python3 waf build --targets=tier0,tier1,vstdlib      (the whole `waf build` also works)
Only libtier1.a, libtier0.so and libvstdlib.so are used. Nothing is written into DIR or this repo
except the build output in --out (default: a temp dir).
"""
import argparse
import os
import platform
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
sys.path.insert(0, str(ROOT / "tests"))
import gen_kv  # noqa: E402

# Flags Source's waf uses for a release build on x86_64 (taken from its configure log).
SRC_FLAGS = ("-std=c++11 -fpermissive -w -O2 -funsafe-math-optimizations -ftree-vectorize -ffast-math -pipe -fPIC "
             "-pthread -march=core2 -mfpmath=sse -fno-strict-aliasing -fvisibility=hidden -DPLATFORM_64BITS=1 "
             "-D_GLIBCXX_USE_CXX11_ABI=0 -DLINUX=1 -D_LINUX=1 -DPOSIX=1 -D_POSIX=1 -DPLATFORM_POSIX=1 -DGNUC "
             "-DNO_HOOK_MALLOC -D_DLL_EXT=.so -DNO_MEMOVERRIDE_NEW_DELETE=1 -DNDEBUG -DCOMPILER_GCC=1").split()
LIB_SOURCES = [str(ROOT / "src" / f) for f in ("kv_string_pool.cpp", "keyvalue.cpp", "kv_parser.cpp", "kv_writer.cpp")]


def sh(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f"command failed: {' '.join(map(str, cmd))}\n{r.stdout}{r.stderr}")
    return r.stdout


def build(out, src_engine):
    exe = {}
    s = Path(src_engine)
    for lib in ("build/tier1/libtier1.a", "build/tier0/libtier0.so", "build/vstdlib/libvstdlib.so"):
        if not (s / lib).exists():
            sys.exit(f"{s / lib} not found: build source-engine first (see --help)")
    exe["source"] = out / "cmp_source"
    sh(["g++", *SRC_FLAGS, "-DBACKEND_SOURCE", f"-I{s}/public", f"-I{s}/public/tier0", f"-I{s}/public/tier1", f"-I{s}/common",
        str(HERE / "kv_compare.cpp"), "-o", exe["source"], f"-L{s}/build/tier0", f"-L{s}/build/vstdlib",
        f"{s}/build/tier1/libtier1.a", "-ltier0", "-lvstdlib", f"-Wl,-rpath,{s}/build/tier0:{s}/build/vstdlib", "-ldl"])
    common = ["g++", "-std=c++17", "-DBACKEND_OURS", f"-I{ROOT / 'src'}", "-pthread", str(HERE / "kv_compare.cpp"), *LIB_SOURCES]
    # same optimisation flags as Source, single thread build / thread-safe build / our own default
    variants = {
        "ours": ["-O2", "-march=core2", "-mfpmath=sse", "-ffast-math"],
        "ours_mt": ["-O2", "-march=core2", "-mfpmath=sse", "-ffast-math", "-DKV_THREAD_SAFE"],
        "ours_O3": ["-O3", "-march=native", "-DNDEBUG"],
    }
    for name, flags in variants.items():
        exe[name] = out / f"cmp_{name}"
        sh([*common[:1], *flags, *common[1:], "-o", exe[name]])
    return exe


GROWABLE_USED = set()  # (file) names for which Source needed its growable key-name table


def run(exe, *args):
    cmd = [str(exe), *map(str, args)]
    r = subprocess.run(cmd, capture_output=True, text=True, timeout=900)
    if r.returncode and "keyvalue string space" in r.stdout + r.stderr:
        # Source's default key-name table holds 4 MB and is full: use its growable table instead
        GROWABLE_USED.add(Path(str(args[1])).stem)
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=900, env={**os.environ, "KV_SOURCE_GROWABLE": "1"})
    if r.returncode:
        sys.exit(f"command failed: {' '.join(cmd)}\n{r.stdout}{r.stderr}")
    return {k: v for k, v in (l.split(None, 1) for l in r.stdout.splitlines() if " " in l)}


def table(title, header, rows):
    print(f"\n### {title}\n")
    print("| " + " | ".join(header) + " |")
    print("|" + "|".join("---" for _ in header) + "|")
    for r in rows:
        print("| " + " | ".join(str(c) for c in r) + " |")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--source-engine", required=True)
    ap.add_argument("--out")
    ap.add_argument("--quick", action="store_true", help="smaller files, fewer repetitions")
    a = ap.parse_args()
    out = Path(a.out or tempfile.mkdtemp(prefix="kvcompare_"))
    out.mkdir(parents=True, exist_ok=True)
    exe = build(out, a.source_engine)

    k = 0.2 if a.quick else 1.0
    sets = {"wide": ("wide", int(100000 * k)), "nested": ("nested", int(50000 * k)),
            "unique": ("unique", int(100000 * k)), "nested_big": ("nested", int(400000 * k))}
    files = {}
    for name, (kind, n) in sets.items():
        files[name] = out / f"{name}.kv"
        files[name].write_text(gen_kv.GENERATORS[kind](n), newline="\n")
    reps = 3 if a.quick else 7

    cpu = next((l.split(":", 1)[1].strip() for l in open("/proc/cpuinfo") if l.startswith("model name")), platform.processor())
    print(f"## Source Engine KeyValues vs this library\n\nCPU: {cpu}, {os.cpu_count()} threads; {platform.platform()}")
    print(f"Source: libtier1.a built by its waf (-O2 -march=core2 -ffast-math, gcc, x86_64); "
          f"ours is compiled with the same flags (`ours`), plus `ours_O3` = -O3 -march=native.")
    print("File sizes: " + ", ".join(f"{n} {files[n].stat().st_size / 1e6:.1f} MB" for n in files))

    labels = [("source", "Source"), ("ours", "ours"), ("ours_O3", "ours -O3 native")]

    rows = []
    for n in files:
        r = {e: run(exe[e], "load", files[n], reps) for e, _ in labels}
        rows.append([n] + [f"{float(r[e]['load_ms']):.1f} ms ({float(r[e]['load_mbps']):.0f} MB/s)" for e, _ in labels] +
                    [f"{float(r['source']['load_ms']) / float(r['ours']['load_ms']):.1f}x"])
    table("Load file -> tree (best of %d, page cache warm)" % reps, ["file"] + [l for _, l in labels] + ["ours vs Source"], rows)

    rows = []
    for n in files:
        r = {e: run(exe[e], "load", files[n], reps) for e, _ in labels}
        rows.append([n] + [f"{float(r[e]['destroy_ms']):.1f} ms" for e, _ in labels])
    table("Destroy tree", ["file"] + [l for _, l in labels], rows)

    rows = []
    for n in files:
        r = {e: run(exe[e], "save", files[n], out / "saved.kv", reps) for e, _ in labels}
        rows.append([n] + [f"{float(r[e]['save_ms']):.1f} ms ({float(r[e]['save_mbps']):.0f} MB/s)" for e, _ in labels] +
                    [f"{int(r['source']['saved_bytes']) / 1e6:.1f} / {int(r['ours']['saved_bytes']) / 1e6:.1f} MB"])
    table("Save tree -> file (Source writes to a buffer, then fwrite)", ["file"] + [l for _, l in labels] + ["output size Source / ours"], rows)

    rows = []
    for n in files:
        r = {e: run(exe[e], "mem", files[n]) for e, _ in labels[:2]}
        s, o = r["source"], r["ours"]
        rows.append([n, f"{int(s['file_bytes']) / 1e6:.1f} MB"] +
                     [f"{int(r[e]['heap_bytes']) / 1e6:.1f} MB ({r[e]['heap_over_file'].strip()}x)" for e, _ in labels[:2]] +
                     [f"{int(r[e]['rss_bytes']) / 1e6:.1f} MB" for e, _ in labels[:2]] +
                     [f"{int(r[e]['peak_rss_bytes']) / 1e6:.1f} MB" for e, _ in labels[:2]])
    table("Memory of one loaded tree (fresh process; heap = malloc in use, RSS = resident growth)",
          ["file", "file size", "heap Source", "heap ours", "RSS Source", "RSS ours", "peak RSS Source", "peak RSS ours"], rows)

    rows = []
    for n in ("nested", "wide"):
        r = {}
        for e, _ in labels[:2]:
            r[e] = run(exe[e], "lookup", files[n], 2000 if e == "source" else 200000)
        row = [n, r["source"].get("top_level_objects", "").strip()]
        row += [f"{float(r[e]['top_level_lookup_ns']):.0f} ns" for e, _ in labels[:2]]
        if "small_object_getint_ns" in r["source"]:
            row += [f"{float(r[e]['small_object_getint_ns']):.0f} / {float(r[e]['small_object_getstring_ns']):.0f} ns" for e, _ in labels[:2]]
            row += ["same" if r["source"]["checksum"] == r["ours"]["checksum"] else f"DIFFERENT {r['source']['checksum']} vs {r['ours']['checksum']}"]
        else:
            row += ["-", "-", "-"]
        rows.append(row)
    table("Lookups", ["file", "keys in the top object", "top-level by name: Source", "ours",
                      "6-key object GetInt / GetString: Source", "ours", "values read"], rows)

    rows = []
    cores = os.cpu_count() or 1
    for t in sorted({1, 2, 4, 8, min(16, cores)}):
        r = {e: run(exe[e], "mt", files["nested"], t, 4 if not a.quick else 2) for e in ("source", "ours_mt")}
        rows.append([t] + [f"{float(r[e]['mt_total_mbps']):.0f} MB/s" for e in ("source", "ours_mt")] +
                    [f"{float(r['ours_mt']['mt_total_mbps']) / float(r['source']['mt_total_mbps']):.1f}x"])
    if GROWABLE_USED:
        print("\nSource's default key-name table (4 MB) overflowed on: " + ", ".join(sorted(GROWABLE_USED)) +
              "; its numbers for those files use KeyValues::SetUseGrowableStringTable(true).")
    table("Threads loading nested.kv at the same time (total MB/s; own tree per thread)",
          ["threads", "Source", "ours (thread-safe build)", "ours vs Source"], rows)


if __name__ == "__main__":
    main()
