#!/usr/bin/env python3
"""source-kv (Rust crate from SourceRS) vs this library: builds both benchmark backends, runs them on
the same generated files and prints markdown tables. Needs cargo.

  run_sourcers.py [--crate=DIR] [--src=DIR] [--quick]

DIR of --crate is a checkout of crates/source-kv (default: crate/ next to this script, which is empty in
the repository: see crate/README.md). The crate is copied to a temp dir with its
workspace-inherited fields filled in; nothing is written into it. --src is the directory with our
library sources (default: src/ of this repository).
"""
import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
COMPARE = HERE.parent / "compare"
sys.path.insert(0, str(ROOT / "tests"))
import gen_kv  # noqa: E402

CXX = os.environ.get("CXX", "g++")  # GCC- or Clang-compatible
LIB_FILES = ("kv_string_pool.cpp", "keyvalue.cpp", "kv_parser.cpp", "kv_writer.cpp")


def sh(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f"command failed: {' '.join(map(str, cmd))}\n{r.stdout}{r.stderr}")
    return r.stdout


def run(exe, *args):
    r = subprocess.run([str(exe), *map(str, args)], capture_output=True, text=True, timeout=900)
    if r.returncode:
        sys.exit(f"command failed: {exe} {args}\n{r.stdout}{r.stderr}")
    return {k: v.strip() for k, v in (l.split(None, 1) for l in r.stdout.splitlines() if " " in l)}


def table(title, header, rows):
    print(f"\n### {title}\n")
    print("| " + " | ".join(header) + " |")
    print("|" + "|".join("---" for _ in header) + "|")
    for r in rows:
        print("| " + " | ".join(str(c) for c in r) + " |")


def main():
    if not sys.platform.startswith("linux"):
        sys.exit("run_sourcers.py needs Linux: the benchmark programs read glibc/proc memory counters")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out")
    ap.add_argument("--crate", help="checkout of crates/source-kv (default: crate/ next to this script)")
    ap.add_argument("--src", help="our library sources (default: src/)")
    ap.add_argument("--quick", action="store_true")
    a = ap.parse_args()
    out = Path(a.out or tempfile.mkdtemp(prefix="kvsourcers_"))
    out.mkdir(parents=True, exist_ok=True)

    src = Path(a.src).resolve() if a.src else ROOT / "src"
    lib_sources = [str(src / f) for f in LIB_FILES]
    crate = Path(a.crate) if a.crate else HERE / "crate"

    # the bench program next to a copy of the crate whose `*.workspace = true` fields are filled in
    rs = out / "rs"
    shutil.rmtree(rs, ignore_errors=True)
    shutil.copytree(HERE / "bench", rs, ignore=shutil.ignore_patterns("target"))
    cargo = (rs / "Cargo.toml").read_text()
    if not (crate / "Cargo.toml").is_file():
        sys.exit(f"{crate}: no source-kv crate here. Copy crates/source-kv of https://github.com/LaVashikk/SourceRS "
                 f"into {HERE / 'crate'} (see crate/README.md) or pass --crate=DIR")
    shutil.copytree(crate, out / "skv", ignore=shutil.ignore_patterns("target", "tests", "examples"), dirs_exist_ok=True)
    m = (out / "skv" / "Cargo.toml").read_text()
    fill = {"version": '"0.6.0"', "edition": '"2024"', "rust-version": '"1.85"', "license": '"MIT"', "authors": '["x"]',
            "serde": '"1"', "thiserror": '"2"'}
    for k, v in fill.items():
        m = re.sub(r"(?m)^%s\.workspace = true$" % re.escape(k), f"{k} = {v}", m)
    (out / "skv" / "Cargo.toml").write_text(m.split("[dev-dependencies]")[0] + "\n[workspace]\n")
    cargo = re.sub(r"(?m)^source-kv = .*$", f'source-kv = {{ path = "{out / "skv"}" }}', cargo)
    (rs / "Cargo.toml").write_text(cargo)
    sh(["cargo", "build", "--release", "--manifest-path", str(rs / "Cargo.toml"), "--target-dir", str(out / "cargo")],
       env={**os.environ, "RUSTFLAGS": "-C target-cpu=native"})  # same CPU target as our -march=native
    exe = {"rs": out / "cargo" / "release" / "kv_compare_rs", "ours": out / "cmp_ours", "ours_mt": out / "cmp_ours_mt"}
    base = [CXX, "-std=c++17", "-O3", "-march=native", "-DNDEBUG", "-DBACKEND_OURS", f"-I{src}", "-pthread", str(COMPARE / "kv_compare.cpp"), *lib_sources]
    sh([*base, "-o", exe["ours"]])
    sh([*base, "-DKV_THREAD_SAFE", "-o", exe["ours_mt"]])

    k = 0.2 if a.quick else 1.0
    sets = {"wide": ("wide", int(100000 * k)), "nested": ("nested", int(50000 * k)),
            "unique": ("unique", int(100000 * k)), "nested_big": ("nested", int(400000 * k))}
    files = {}
    for name, (kind, n) in sets.items():
        files[name] = out / f"{name}.kv"
        files[name].write_text(gen_kv.GENERATORS[kind](n), newline="\n")
    reps = 3 if a.quick else 7

    cpu = next((l.split(":", 1)[1].strip() for l in open("/proc/cpuinfo") if l.startswith("model name")), platform.processor())
    print(f"## source-kv (Rust) vs this library\n\nCPU: {cpu}, {os.cpu_count()} threads; {platform.platform()}")
    print("source-kv: `cargo build --release` (opt-level 3, LTO, target-cpu=native), parsed with `Deserializer::parse_root` into `Value`; "
          "ours: -O3 -march=native.")
    print("File sizes: " + ", ".join(f"{n} {files[n].stat().st_size / 1e6:.1f} MB" for n in files))
    L = [("rs", "source-kv"), ("ours", "ours")]

    def ratio(x, y):
        return f"{float(x) / float(y):.1f}x" if float(y) > 0 else "-"  # a tiny file can time as 0.0 ms

    rows, drows = [], []
    for n in files:
        r = {e: run(exe[e], "load", files[n], reps) for e, _ in L}
        rows.append([n] + [f"{float(r[e]['load_ms']):.1f} ms ({float(r[e]['load_mbps']):.0f} MB/s)" for e, _ in L] + [ratio(r['rs']['load_ms'], r['ours']['load_ms'])])
        drows.append([n] + [f"{float(r[e]['destroy_ms']):.1f} ms" for e, _ in L] + [ratio(r['rs']['destroy_ms'], r['ours']['destroy_ms'])])
    table("Load file -> tree (best of %d, page cache warm)" % reps, ["file", "source-kv", "ours", "ours vs source-kv"], rows)

    rows = []
    for n in files:
        t = run(exe["rs"], "tape", files[n], reps)
        o = run(exe["ours"], "load", files[n], reps)
        rows.append([n, f"{float(t['load_ms']):.1f} ms ({float(t['load_mbps']):.0f} MB/s)", f"{float(o['load_ms']):.1f} ms ({float(o['load_mbps']):.0f} MB/s)",
                     ratio(t['load_ms'], o['load_ms'])])
    table("Parse only: source-kv `from_str::<IgnoredAny>` (the borrowed tape, no owned tree) vs our full load",
          ["file", "source-kv parse only", "ours (full tree)", "ours vs source-kv"], rows)
    table("Destroy tree", ["file", "source-kv", "ours", "ours vs source-kv"], drows)

    rows = []
    for n in files:
        r = {e: run(exe[e], "save", files[n], out / "saved.kv", reps) for e, _ in L}
        rows.append([n] + [f"{float(r[e]['save_ms']):.1f} ms ({float(r[e]['save_mbps']):.0f} MB/s)" for e, _ in L] +
                    [ratio(r['rs']['save_ms'], r['ours']['save_ms']), f"{int(r['rs']['saved_bytes']) / 1e6:.1f} / {int(r['ours']['saved_bytes']) / 1e6:.1f} MB"])
    table("Save tree -> file (source-kv: `to_string`, then write)", ["file", "source-kv", "ours", "ours vs source-kv", "output size source-kv / ours"], rows)

    rows = []
    for n in files:
        r = {e: run(exe[e], "mem", files[n]) for e, _ in L}
        rows.append([n, f"{int(r['rs']['file_bytes']) / 1e6:.1f} MB"] +
                    [f"{int(r[e]['heap_bytes']) / 1e6:.1f} MB ({float(r[e]['heap_over_file']):.2f}x)" for e, _ in L] +
                    [f"{int(r[e]['rss_bytes']) / 1e6:.1f} MB" for e, _ in L])
    table("Memory of one loaded tree (fresh process; heap = bytes the allocator holds live, RSS = resident growth)",
          ["file", "file size", "heap source-kv", "heap ours", "RSS source-kv", "RSS ours"], rows)

    rows = []
    for n in ("nested", "wide"):
        # source-kv looks a key up by scanning the object, so it gets fewer lookups
        r = {e: run(exe[e], "lookup", files[n], 200000 if e == "ours" else 2000) for e, _ in L}
        row = [n, r["rs"]["top_level_objects"]] + [f"{float(r[e]['top_level_lookup_ns']):.0f} ns" for e, _ in L]
        if "small_object_getint_ns" in r["rs"]:
            row += [f"{float(r[e]['small_object_getint_ns']):.0f} / {float(r[e]['small_object_getstring_ns']):.0f} ns" for e, _ in L]
            row += ["same" if r["rs"]["checksum"] == r["ours"]["checksum"] else f"DIFFERENT {r['rs']['checksum']} vs {r['ours']['checksum']}"]
        else:
            row += ["-", "-", "-"]
        rows.append(row)
    table("Lookups (source-kv has no typed values: its GetInt is `get_string` + `parse::<i64>`)",
          ["file", "keys in the top object", "top-level by name: source-kv", "ours", "6-key object GetInt / GetString: source-kv", "ours", "values read"], rows)

    rows = []
    for t in sorted({1, 2, 4, 8, min(16, os.cpu_count() or 1)}):
        r = {e: run(exe[e], "mt", files["nested"], t, 2 if a.quick else 4) for e in ("rs", "ours_mt")}
        rows.append([t] + [f"{float(r[e]['mt_total_mbps']):.0f} MB/s" for e in ("rs", "ours_mt")] + [ratio(r['ours_mt']['mt_total_mbps'], r['rs']['mt_total_mbps'])])
    table("Threads loading nested.kv at the same time (total MB/s; own tree per thread)",
          ["threads", "source-kv", "ours (thread-safe build)", "ours vs source-kv"], rows)


if __name__ == "__main__":
    main()
