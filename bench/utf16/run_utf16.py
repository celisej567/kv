#!/usr/bin/env python3
"""A/B load timing for the UTF-16 conversion.

  A = a kv_cli built from the commit before the conversion (UTF-8 only)
  B = a kv_cli with the conversion
The same generated files are loaded alternately by A and B (best of N), so slow drift of the machine hits both.
UTF-8 files: B must not be slower than A (the conversion costs nothing when there is no UTF-16 BOM).
UTF-16 files: only B can read them; their speed is compared with B's own UTF-8 speed.

  python3 -I bench/utf16/run_utf16.py --a <kv_cli A> --b <kv_cli B> [--runs 9] [--quick] > result.md
"""
import argparse
import platform
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests"))
import gen_kv  # noqa: E402


def load_ms(cli, f, out):
    p = subprocess.run([str(cli), "bench", str(f), str(out), "1000"], capture_output=True, text=True, timeout=300)
    if p.returncode != 0:
        return None
    for line in p.stdout.splitlines():
        if line.startswith("load_ms "):
            return float(line.split()[1])
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--a", required=True)
    ap.add_argument("--b", required=True)
    ap.add_argument("--runs", type=int, default=9)
    ap.add_argument("--quick", action="store_true")
    a = ap.parse_args()
    k = 5 if a.quick else 1
    files = {"wide": gen_kv.wide(100000 // k), "nested": gen_kv.nested(50000 // k),
             "unique": gen_kv.unique(100000 // k), "nested_big": gen_kv.nested(400000 // k)}
    print("## UTF-16 conversion: A/B load time\n")
    print(f"CPU: {platform.processor() or platform.machine()}; {platform.platform()}; best of {a.runs}, files loaded alternately by A and B.\n")
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        rows8, rows16 = [], []
        for name, text in files.items():
            u8 = tmp / f"{name}.u8.kv"
            u8.write_bytes(text.encode("utf-8"))
            le = tmp / f"{name}.le.kv"
            le.write_bytes(b"\xff\xfe" + text.encode("utf-16-le"))
            be = tmp / f"{name}.be.kv"
            be.write_bytes(b"\xfe\xff" + text.encode("utf-16-be"))
            best = {k_: 1e9 for k_ in ("A8", "B8", "Ble", "Bbe")}
            out = tmp / "o.kv"
            for _ in range(a.runs):
                for key, cli, f in (("A8", a.a, u8), ("B8", a.b, u8), ("Ble", a.b, le), ("Bbe", a.b, be)):
                    t = load_ms(cli, f, out)
                    if t is None:
                        sys.exit(f"{key}: load of {f.name} failed")
                    best[key] = min(best[key], t)
            mb = u8.stat().st_size / 1e6
            rows8.append((name, mb, best["A8"], best["B8"]))
            rows16.append((name, le.stat().st_size / 1e6, best["B8"], best["Ble"], best["Bbe"]))
        print("### UTF-8 files: before (A) and after (B) the change\n")
        print("| file | size | A | B | B vs A |\n|---|---|---|---|---|")
        for n, mb, ta, tb in rows8:
            print(f"| {n} | {mb:.1f} MB | {ta:.1f} ms ({mb / ta * 1000:.0f} MB/s) | {tb:.1f} ms ({mb / tb * 1000:.0f} MB/s) | {ta / tb:.2f}x |")
        print("\n### UTF-16 files (with BOM) loaded by B, against the same text as UTF-8\n")
        print("| file | UTF-16 size | UTF-8 | UTF-16 LE | UTF-16 BE | LE vs UTF-8 |\n|---|---|---|---|---|---|")
        for n, mb16, t8, tle, tbe in rows16:
            print(f"| {n} | {mb16:.1f} MB | {t8:.1f} ms | {tle:.1f} ms | {tbe:.1f} ms | {t8 / tle:.2f}x |")


main()
