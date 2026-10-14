#!/usr/bin/env python3
"""Black-box tests: runs kv_cli on every tests/cases/*.kv and checks the behaviour.

Per case NAME.kv, optional sidecar files:
  NAME.err   -> loading must fail (exit 1), stderr must contain every non-comment line
  NAME.dump  -> expected `kv_cli dump` stdout (exact)
  NAME.out   -> expected bytes produced by `kv_cli write` (exact)
  NAME.get   -> lines `path | type | default => expected`, checked with `kv_cli get`
Every successful case is also round-tripped (write -> dump -> write must be stable).

Also runs window-boundary tests (the parser streams the file in blocks) and performance /
memory tests on generated files (see PERF below). Perf tests are SKIPped on non-optimized or
sanitized builds because their numbers would be meaningless.

Single thread and multithread: the script looks for two more tools next to kv_cli (CMake
builds them together):
  kv_stress     the library built with KV_THREAD_SAFE: every .kv case is also loaded by 8 threads
                at once (`mtload`) and must give the same dump / the same error as kv_cli; plus the
                pool threading, memory and speed tests
  kv_stress_st  the plain library: the same pool memory and speed tests, single-thread build
Missing tools are an error (exit 2); --stress=PATH / --stress-st=PATH override the lookup.

Usage: run_tests.py <path-to-kv_cli> [--stress=PATH] [--stress-st=PATH] [test-name ...]
"""
import os
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.dont_write_bytecode = True  # no __pycache__ in the source tree
sys.path.insert(0, str(Path(__file__).resolve().parent))
import gen_kv

CASES = Path(__file__).resolve().parent / "cases"
failures = []  # messages of the test currently running
TOOLS = {}     # "mt" -> kv_stress, "st" -> kv_stress_st
MT_LOAD_THREADS = 8


ESC = "1"  # KV_ESC for the tools: "1" = UsesEscapeSequences(true). Cases with a .noesc file run with the default (off)


def run(cli, *args, timeout=120):
    try:
        p = subprocess.run([cli, *map(str, args)], capture_output=True, timeout=timeout, env={**os.environ, "KV_ESC": ESC})
    except subprocess.TimeoutExpired:
        return -1, "", f"timed out after {timeout}s"
    return p.returncode, p.stdout.decode("utf-8", "replace").replace("\r\n", "\n"), p.stderr.decode("utf-8", "replace")


def check(name, cond, msg):
    if not cond:
        failures.append(f"{name}: {msg}")


def lines(path):
    return [l for l in path.read_text(encoding="utf-8").splitlines() if l.strip() and not l.startswith("#")]


def mt_load(tag, kv, want_rc, want_out="", want_err=()):
    """Loads `kv` from MT_LOAD_THREADS threads at once (thread-safe build); every thread must get
    exactly what the single-thread kv_cli got."""
    rc, so, se = run(TOOLS["mt"], "mtload", kv, MT_LOAD_THREADS)
    check(tag, rc == want_rc, f"[mt] exit {rc}, expected {want_rc}: {se[:300]}")
    check(tag, so == want_out, f"[mt] dump differs from the single-thread dump ({len(so)} vs {len(want_out)} bytes)")
    for want in want_err:
        check(tag, want in se, f"[mt] stderr {se[:300]!r} lacks {want!r}")


def test_case(cli, kv, tmp):
    global ESC
    ESC = "0" if kv.with_suffix(".noesc").exists() else "1"
    try:
        return test_case_inner(cli, kv, tmp)
    finally:
        ESC = "1"


def test_case_inner(cli, kv, tmp):
    name = kv.stem
    base = kv.with_suffix("")
    err, dump, out, get = (base.with_suffix(s) for s in (".err", ".dump", ".out", ".get"))
    written = Path(tmp) / (name + ".written")

    if err.exists():
        rc, so, se = run(cli, "dump", kv)
        check(name, rc == 1, f"dump exit {rc}, expected 1")
        check(name, so == "", "stdout not empty on failure")
        check(name, kv.name in se, f"stderr lacks file name: {se!r}")
        for want in lines(err):
            check(name, want in se, f"stderr {se!r} lacks {want!r}")
        rc, _, _ = run(cli, "write", kv, written)
        check(name, rc == 1 and not written.exists(), "write must fail and create nothing")
        mt_load(name, kv, 1, "", [kv.name] + lines(err))
        return "st+mt"

    rc, so, se = run(cli, "dump", kv)
    check(name, rc == 0, f"dump exit {rc}: {se}")
    if rc != 0:
        return
    if dump.exists():
        check(name, so == dump.read_text(encoding="utf-8"), f"dump mismatch:\n--- got\n{so}--- want\n{dump.read_text(encoding='utf-8')}")
    mt_load(name, kv, 0, so)

    # UsesConditionals(false): nothing is hidden. Files without a '[' must look the same either way.
    alldump = base.with_suffix(".alldump")
    rc, soall, se = run(cli, "dumpall", kv)
    check(name, rc == 0, f"dumpall exit {rc}: {se}")
    if alldump.exists():
        check(name, soall == alldump.read_text(encoding="utf-8"), f"dumpall mismatch:\n--- got\n{soall}--- want\n{alldump.read_text(encoding='utf-8')}")
    elif b"[" not in kv.read_bytes():
        check(name, soall == so, "dumpall differs from dump in a file without conditions")

    rc, _, se = run(cli, "write", kv, written)
    check(name, rc == 0, f"write exit {rc}: {se}")
    if rc != 0:
        return
    first = written.read_bytes()
    if out.exists():
        check(name, first == out.read_bytes(), f"written mismatch:\n--- got\n{first.decode()}--- want\n{out.read_bytes().decode()}")

    # round trip: same tree, byte-identical second write
    rc, so2, _ = run(cli, "dump", written)
    check(name, rc == 0 and so2 == so, "dump of written file differs from the original")
    written2 = Path(tmp) / (name + ".written2")
    rc, _, _ = run(cli, "write", written, written2)
    check(name, rc == 0 and written2.read_bytes() == first, "second write is not identical")

    if get.exists():
        for line in lines(get):
            left, sep, want = line.partition(" => ")
            if not sep:
                left, want = line.rstrip(" =>"), ""  # "path | type | =>" with empty expected
            parts = [p.strip() for p in left.rstrip().rstrip("=>").split("|")]
            path, typ = parts[0], parts[1]
            dflt = parts[2] if len(parts) > 2 and parts[2] else None
            rc, so, se = run(cli, "get", kv, path, typ, *([dflt] if dflt is not None else []))
            got = so.rstrip("\n")
            check(name, rc == 0 and got == want.strip(), f"get {line!r}: got {got!r} (rc {rc}) {se}")
    return "st+mt"


def misc_missing_file(cli, tmp):
    rc, _, se = run(cli, "dump", CASES / "does_not_exist.kv")
    check("missing-file", rc == 1 and "cannot open file" in se, f"rc={rc} stderr={se!r}")


def misc_no_args(cli, tmp):
    rc, _, _ = run(cli)
    check("usage", rc == 2, f"exit {rc}, expected 2")


def misc_bad_command(cli, tmp):
    rc, _, _ = run(cli, "bogus", "x")
    check("usage", rc == 2, f"exit {rc}, expected 2")


def misc_kvdef(cli, tmp):
    """GetKeyValue(name, def): the tree's own node when found, otherwise an independent copy of def."""
    good = CASES / "good.kv"
    for path, want in (("val2", "tree\nnonnull\n"),                       # found: no copy
                       ("missing", "copy fb x=9 fallback_x=7\nnull\n"),   # missing: copy, def untouched
                       ("val1", "copy fb x=9 fallback_x=7\nnull\n"),      # not a KeyValue: same as missing
                       ("missing/deeper", "copy fb x=9 fallback_x=7\nnull\n")):
        rc, so, se = run(cli, "get", good, path, "kvdef")
        check(path, rc == 0 and so == want, f"rc={rc} got {so!r} want {want!r} {se[:100]}")


API_SCENARIOS = {"move_assign_from_descendant", "copy_assign_from_descendant", "set_self", "set_replace_with_descendant",
                 "copy_independent", "move_construct", "set_replaces_in_place", "getters", "getkeyvalue_fallback",
                 "name_pointer_stable", "clear",
                 "dups_order", "dups_get_set_first", "dups_replace_first_block", "dups_first_wins_type", "dups_getstrings",
                 "dups_with_index", "dups_copy_move", "dups_write_reload", "dups_many_linear", "dups_clear", "children_empty", "conditions_custom",
                 "uc_hidden_skipped", "uc_false_shows_all", "uc_toggle_back", "uc_set_on_hidden", "uc_copy_move_adopt",
                 "uc_copy_independent", "uc_write", "uc_survives_load", "uc_clear", "names_case_insensitive",
                 "getbool", "int_truncates_float", "remove_key", "remove_key_big", "remove_key_all", "remove_key_hidden",
                 "uses_escape_sequences", "root_unnamed_takes_first", "root_named_finds_own",
                 "root_named_not_found", "root_other_roots_checked", "name_with_space", "numbers_kept_in_node",
                 "setstring_classifies", "getstring_on_types"}


def misc_api(cli, tmp):
    """CKeyValue scenarios that files cannot express (aliasing, copies, moves); crashes show up as rc != 0."""
    rc, so, se = run(cli, "api")
    got = dict(l.split() for l in so.splitlines())
    check("api", rc == 0, f"exit {rc} {se[:300]}")
    check("api", set(got) == API_SCENARIOS, f"scenarios ran: {sorted(got)}")
    for k, v in got.items():
        check("api", v == "1", f"{k} failed")
    return f"{len(got)} scenarios"


def dups_gameinfo(cli, tmp):
    """The real Half-Life 2 gameinfo.txt: SearchPaths repeats the names game, game+mod and platform.
    Every occurrence must be there, in file order (verified against Source's KeyValues)."""
    kv = CASES / "gameinfo.kv"
    sp = "FileSystem/SearchPaths"
    want = {
        (sp, "game"): ["|all_source_engine_paths|hl2/hl2_textures.vpk", "|all_source_engine_paths|hl2/hl2_sound_misc.vpk",
                       "|all_source_engine_paths|hl2/hl2_misc.vpk", "|all_source_engine_paths|hl2"],
        (sp, "game+mod"): ["hl2/custom/*", "hl2/hl2_sound_vo_english.vpk", "hl2/hl2_pak.vpk"],
        (sp, "platform"): ["|all_source_engine_paths|platform/platform_misc.vpk", "|all_source_engine_paths|platform"],
        (sp, "gamebin"): ["hl2/bin"],
        (sp, "nothing"): [],
        (".", "game"): ["HALF-LIFE 2"],
    }
    for (path, name), values in want.items():
        rc, so, se = run(cli, "list", kv, path, name)
        got = [l.split("\t")[2] for l in so.splitlines() if not l.startswith("#")]
        counts = [l for l in so.splitlines() if l.startswith("#")]
        check(f"{path}/{name}", rc == 0 and got == values, f"rc={rc} got {got}, want {values} {se[:100]}")
        check(f"{path}/{name}", counts[:2] == [f"#count {len(values)}", f"#api_count {len(values)}"], f"counts {counts}")
    # the unfiltered list has every entry once, interleaved in file order
    rc, so, _ = run(cli, "list", kv, sp)
    names = [l.split("\t")[0] for l in so.splitlines() if not l.startswith("#")]
    check("order", names == ["game+mod", "game_lv", "game+mod", "game+mod", "game", "game", "game", "platform",
                             "mod+mod_write+default_write_path", "game+game_write", "gamebin", "game", "platform"], f"{names}")
    rc, so, _ = run(cli, "list", kv, "FileSystem/nothing")
    check("missing object", rc == 0 and "#count 0" in so, so)
    return "13 entries, 5 repeated names"


def anonymous_root(cli, tmp):
    """A file starting with '{': an unnamed object takes it and is named after the path given to Load."""
    f = Path(tmp) / "Anon.kv"
    f.write_text("{\n\ta 1\n}\n\"S\" { b 2 }\n", encoding="utf-8", newline="\n")
    rc, so, _ = run(cli, "dump", f)
    check("first", rc == 0 and so == str(f).lower() + "/a\tint\t1\n", f"got {so!r}")
    f.write_text("\"R\" { b 2 }\n{ a 1 }\n", encoding="utf-8", newline="\n")
    rc, so, se = run(cli, "dump", f)
    check("not first", rc == 1 and "2:1: expected root name" in se, f"rc {rc} {se!r}")
    return "ok"


def encodings(cli, tmp):
    """The same file in 20 text encodings (and, for multi-byte ones, with characters whose bytes equal syntax characters)."""
    import enc_matrix
    rows = enc_matrix.matrix(cli)
    got = {r[0]: tuple(r[2:]) for r in rows}
    check("rows", set(got) == set(enc_matrix.EXPECTED), f"rows {sorted(set(got) ^ set(enc_matrix.EXPECTED))}")
    for label, want in enc_matrix.EXPECTED.items():
        check(label, got.get(label) == want, f"got {got.get(label)}, expected {want}")
    return f"{len(rows)} rows"


def errors_only(stderr):
    """stderr without the "[kv] ..." lines of a KV_VERBOSE build."""
    return "".join(l for l in stderr.splitlines(keepends=True) if not l.startswith("[kv] "))


def utf16_equivalence(cli, tmp):
    """Every valid-UTF-8 case file, converted to UTF-16 (LE and BE, with BOM), must load exactly as the original:
    same dump, same exit code, same error text and position (the converted text is the original UTF-8)."""
    n = 0
    for kv in sorted(CASES.glob("*.kv")):
        data = kv.read_bytes()
        if b"\0" in data or "utf" in kv.stem or "anon" in kv.stem or "bom" in kv.stem:
            continue
        try:
            text = data.decode("utf-8")
        except UnicodeDecodeError:
            continue
        want = {c: run(cli, c, kv) for c in ("dump", "dumpall")}
        for label, bom, codec in (("le", b"\xff\xfe", "utf-16-le"), ("be", b"\xfe\xff", "utf-16-be")):
            f = Path(tmp) / kv.name
            f.write_bytes(bom + text.encode(codec, "surrogatepass"))
            for c in ("dump", "dumpall"):
                rc, so, se = run(cli, c, f)
                wrc, wso, wse = want[c]
                se, wse = errors_only(se), errors_only(wse)  # a KV_VERBOSE build adds "[kv] ..." lines
                check(f"{kv.stem}/{label}/{c}", (rc, so) == (wrc, wso) and se.replace(str(f), str(kv)) == wse,
                      f"utf-8: {wrc} {wse!r}; utf-16: {rc} {se!r}")
            n += 1
    check("count", n >= 80, f"only {n} files were converted")
    return f"{n // 2} files x 2 endianness"


def utf16_boundaries(cli, tmp):
    """Big UTF-16 files whose characters (also surrogate pairs) straddle every block boundary of the reader."""
    body = []
    for i in range(30000):
        body.append(f'\t"k{i}" "значение 😀 {i} 日本語 𝄞"\n')
    for pad in range(4):
        text = '"Root"\n{\n' + "\t" + "p" * (pad + 1) + " 1\n" + "".join(body) + "}\n"
        u8 = Path(tmp) / f"bnd{pad}.u8.kv"
        u8.write_bytes(text.encode("utf-8"))
        rc0, so0, _ = run(cli, "dump", u8)
        check(f"pad{pad}", rc0 == 0 and len(so0) > 1_000_000, f"utf-8 load rc {rc0}")
        for label, bom, codec in (("le", b"\xff\xfe", "utf-16-le"), ("be", b"\xfe\xff", "utf-16-be")):
            f = Path(tmp) / f"bnd{pad}.{label}.kv"
            f.write_bytes(bom + text.encode(codec))
            rc, so, se = run(cli, "dump", f)
            check(f"pad{pad}/{label}", rc == 0 and so == so0, f"rc {rc} {se[:200]!r}; dump equal: {so == so0}")
    return f"{len(text) // 1024} KB x 4 alignments x 2 endianness"


def dups_blocks(cli, tmp):
    """Repeated names that hold objects, and a repeated name mixing a value and an object."""
    kv = CASES / "dup_blocks.kv"
    rc, so, _ = run(cli, "list", kv, ".", "item")
    check("item", rc == 0 and so == "item\tkv\t\n" * 4 + "#count 4\n#api_count 4\n", f"got {so!r}")
    rc, so, _ = run(cli, "list", kv, "sub", "a")
    check("a", rc == 0 and so == "a\tstring\tx\na\tstring\ty\na\tkv\t\n#count 3\n#api_count 3\n", f"got {so!r}")
    rc, so, _ = run(cli, "list", kv, "item", "tag")  # `item` resolves to the FIRST item block
    check("first block", rc == 0 and so == "tag\tstring\ta\n#count 1\n#api_count 1\n", f"got {so!r}")
    rc, so, _ = run(cli, "get", kv, "item/id", "int")
    check("get first", rc == 0 and so.strip() == "1", f"got {so!r}")
    return "4 blocks with one name"


def fail_cleanup(cli, tmp):
    """Every allocation of a load / write / copy fails in turn (kv_stress allocfail): after each failure the heap,
    the open files and the output file are as before, a failed load leaves an empty object, the pool is intact."""
    cases = sorted(CASES.glob("*.kv"))
    for kv in cases:
        for tag, tool in TOOLS.items():
            rc, _, se = run(tool, "allocfail", kv, timeout=300)
            check(f"{kv.stem} [{tag}]", rc == 0, f"exit {rc}: {se.strip()[-300:]}")
    return f"{len(cases)} files, both library builds"


def misc_write_unwritable(cli, tmp):
    rc, _, se = run(cli, "write", CASES / "good.kv", Path(tmp) / "no_such_dir" / "x.kv")
    check("write-unwritable", rc == 1, f"exit {rc}, expected 1")
    check("write-unwritable", "cannot create file" in se, f"stderr {se!r}")


def logging(cli, tmp):
    """By default the library prints errors only; a build with KV_VERBOSE adds "[kv] ..." lines (and only those)."""
    _, info, _ = run(cli, "info")
    verbose = "verbose=1" in info
    good = CASES / "good.kv"
    out = Path(tmp) / "log.out"
    for what, args in (("load", ("dump", good)), ("write", ("write", good, out))):
        rc, _, se = run(cli, *args)
        check(what, rc == 0, f"exit {rc}")
        lines = [l for l in se.splitlines() if l]
        if not verbose:
            check(what, lines == [], f"a default build printed {lines}")
        else:
            check(what, bool(lines) and all(l.startswith("[kv] ") for l in lines), f"lines {lines}")
            want = "loaded" if what == "load" else "wrote"
            check(what, any(l.startswith("[kv] " + want) for l in lines), f"no '{want}' line in {lines}")
    # errors are printed either way, in the "file:line:col: message" form without the prefix
    rc, _, se = run(cli, "dump", CASES / "err_trailing.kv")
    check("error", rc == 1 and any(l.startswith(str(CASES / "err_trailing.kv") + ":") for l in se.splitlines()), f"stderr {se!r}")
    return "verbose build" if verbose else "default build"


def write_large_values(cli, tmp):
    """Names and values around the writer's block and slack sizes (4 KB, 64 KB) and far beyond them."""
    sizes = [0, 1, 100, 4095, 4096, 4097, 65535, 65536, 65537, 300000]
    src = ['"Big"', "{"]
    for i, n in enumerate(sizes):
        src.append(f'    "k{i}{"n" * (70000 if i % 3 == 0 else 3)}" "{"v" * n}"')
        src.append(f'    "num{i}" {i}')
        src.append(f'    "sub{i}" {{ "x" "{"y" * n}" }}')
    src.append("}")
    path = Path(tmp) / "large_values.kv"
    path.write_text("\n".join(src) + "\n", encoding="utf-8", newline="\n")
    out = Path(tmp) / "large_values_out.kv"
    rc, _, se = run(cli, "write", path, out)
    check("write", rc == 0, f"exit {rc}: {se}")
    _, d1, _ = run(cli, "dump", path)
    rc, d2, _ = run(cli, "dump", out)
    check("reload", rc == 0 and d1 == d2 and d1 != "", "the written file does not read back the same")
    return f"{len(sizes)} sizes up to {max(sizes) // 1000} KB"


# ---------------------------------------------------------------------------------------------
# Streaming parser: tokens must survive being cut by the block boundary at any position.
# ---------------------------------------------------------------------------------------------

def escape_dump(s):
    return s.replace("\\", "\\\\").replace("\n", "\\n").replace("\t", "\\t").replace("\r", "\\r")


def build_mixed(pad, entries=14000):
    """Returns (kv_text, expected_dump). `pad` leading blanks shift every later byte."""
    src = [" " * pad + "// leading comment", '"Mix"', "{"]
    dump = []
    for i in range(entries):
        kind = i % 7
        if kind == 0:
            src.append(f'    "k{i}" "a\\"b\\\\c\\td {i} // not a comment"  // real comment {i}')
            dump.append(f"mix/k{i}\tstring\t" + escape_dump(f'a"b\\c\td {i} // not a comment'))
        elif kind == 1:
            src.append(f"    k{i} {i} // " + "c" * (i % 40))
            dump.append(f"mix/k{i}\tint\t{i}")
        elif kind == 2:
            src.append(f"    /* block {i}\n     * // \" {{ k{i}x 1 */ k{i}\t{i}.5//x /* not a block" if i % 2 else f"    k{i}\t{i}.5 /*c*/ /*d*/")
            dump.append(f"mix/k{i}\tfloat\t{i}.5")
        elif kind == 3:
            # bare key ends at '{'; every other one without blanks, so the bare value ends at '}'
            src.append(f"    k{i}{{ inner {i} }}" if (i // 7) % 2 else f"    k{i}{{inner {i}}}")
            dump.append(f"mix/k{i}\tkv\t")
            dump.append(f"mix/k{i}/inner\tint\t{i}")
        elif kind == 4:
            src.append(f"    k{i}\n    {{ a {i} b \"x y\" }}")
            dump.append(f"mix/k{i}\tkv\t")
            dump.append(f"mix/k{i}/a\tint\t{i}")
            dump.append(f"mix/k{i}/b\tstring\tx y")
        elif kind == 5:
            src.append(f'    "k{i}"\t"tab\\tnew\\nline"')
            dump.append(f"mix/k{i}\tstring\ttab\\tnew\\nline")
        else:
            src.append(f"    k{i} /not/a/comment // " + "d" * (i % 30))
            dump.append(f"mix/k{i}\tstring\t/not/a/comment")
    src.append("}")
    return "\n".join(src) + "\n", "".join(l + "\n" for l in dump)


def stream_boundaries(cli, tmp):
    for pad in range(0, 40):
        text, dump = build_mixed(pad)
        path = Path(tmp) / f"mixed_{pad}.kv"
        path.write_bytes(text.encode())
        rc, so, se = run(cli, "dump", path)
        check(f"pad={pad}", rc == 0, f"exit {rc}: {se[:200]}")
        if rc != 0:
            return
        if so != dump:
            got, want = so.splitlines(), dump.splitlines()
            idx = next((i for i in range(min(len(got), len(want))) if got[i] != want[i]), min(len(got), len(want)))
            check(f"pad={pad}", False, f"dump differs at line {idx}: got {got[idx:idx+1]} want {want[idx:idx+1]} ({len(got)} vs {len(want)} lines)")
            return
        if pad % 8 == 0:
            mt_load(f"pad={pad}", path, 0, dump)
    return "st+mt"


def stream_long_tokens(cli, tmp):
    big = "x" * 1_000_000
    cases = {
        "quoted": (f'"R" {{ a "{big}" b 1 }}', [f"r/a\tstring\t{big}", "r/b\tint\t1"]),
        "bare": (f'"R" {{ a {big} b 1 }}', [f"r/a\tstring\t{big}", "r/b\tint\t1"]),
        "comment": (f'"R" {{ // {big}\n a 1 }}', ["r/a\tint\t1"]),
        "block_comment": ('"R" { /*' + "* / /*" * 200_000 + '*/ a 1 }', ["r/a\tint\t1"]),
        "escapes": ('"R" { a "' + "\\\\n" * 300_000 + '" }', ["r/a\tstring\t" + "\\\\n" * 300_000]),
    }
    for name, (text, want) in cases.items():
        path = Path(tmp) / f"long_{name}.kv"
        path.write_bytes(text.encode())
        rc, so, se = run(cli, "dump", path)
        check(name, rc == 0 and so == "".join(w + "\n" for w in want), f"rc={rc} stderr={se[:200]!r} got {so[:80]!r}")


def stream_error_position(cli, tmp):
    n = 20000  # well past the first block, so the line count must survive discarded blocks
    body = "".join(f'    "key_{i}" "value of line {i}"\n' for i in range(n))
    for name, text, where in (
        ("lf", '"R"\n{\n' + body + "    {\n}\n", f":{n + 3}:5:"),
        ("crlf", ('"R"\r\n{\r\n' + body.replace("\n", "\r\n") + "    {\r\n}\r\n"), f":{n + 3}:5:"),
        ("unterminated", '"R"\n{\n' + body + '    "oops\n}\n', f":{n + 3}:5:"),
    ):
        path = Path(tmp) / f"errpos_{name}.kv"
        path.write_bytes(text.encode())
        rc, so, se = run(cli, "dump", path)
        check(name, rc == 1 and where in se, f"rc={rc} stderr={se!r}, expected {where}")


# ---------------------------------------------------------------------------------------------
# Deep and branching nesting.
# ---------------------------------------------------------------------------------------------

def branching_tree(depth, fan):
    """Every object holds `fan` child objects (until `depth` levels) plus one int leaf."""
    src, dump = [], []

    def rec(path, level, indent):
        pad = "    " * indent
        for i in range(fan):
            name = f"n{level}_{i}"
            src.append(f'{pad}"{name}"')
            src.append(f"{pad}{{")
            dump.append(f"{path}/{name}\tkv\t")
            src.append(f"{pad}    leaf {level * 10 + i}")
            dump.append(f"{path}/{name}/leaf\tint\t{level * 10 + i}")
            if level < depth:
                rec(f"{path}/{name}", level + 1, indent + 1)
            src.append(f"{pad}}}")

    src += ['"Tree"', "{"]
    rec("Tree", 1, 1)
    src.append("}")
    return "\n".join(src) + "\n", "".join(l.lower() + "\n" for l in dump)  # names are stored in lower case


def deep_branching(cli, tmp):
    text, want = branching_tree(depth=7, fan=3)  # 3279 objects, 7 levels, siblings at every level
    path = Path(tmp) / "branching.kv"
    path.write_text(text, encoding="utf-8", newline="\n")
    rc, so, se = run(cli, "dump", path)
    check("dump", rc == 0 and so == want, f"rc={rc} {se[:200]} (got {len(so.splitlines())} lines, want {len(want.splitlines())})")
    mt_load("dump", path, 0, want)
    out = Path(tmp) / "branching.out"
    rc, _, se = run(cli, "write", path, out)
    check("write", rc == 0, se)
    rc, so2, _ = run(cli, "dump", out)
    check("roundtrip", rc == 0 and so2 == want, "dump of the written file differs")
    last = "/".join(["n%d_2" % lvl for lvl in range(1, 8)])
    rc, so, _ = run(cli, "get", path, last + "/leaf", "int")
    check("get-deepest", rc == 0 and so.strip() == "72", f"got {so!r}")
    rc, so, _ = run(cli, "get", path, "n1_0/n2_1/n3_2/leaf", "int")
    check("get-mixed-branch", rc == 0 and so.strip() == "32", f"got {so!r}")
    return f"{len(want.splitlines()) // 2} objects, 7 levels, st+mt"


def chain(n):
    return '"R"\n{\n' + "a {\n" * n + "x 1\n" + "}\n" * n + "}\n"


def depth_limit(cli, tmp):
    ok = Path(tmp) / "chain512.kv"
    ok.write_text(chain(512), encoding="utf-8", newline="\n")
    rc, so, se = run(cli, "get", ok, "/".join(["a"] * 512) + "/x", "int")
    check("512 levels", rc == 0 and so.strip() == "1", f"rc={rc} out={so!r} {se[:100]}")
    out = Path(tmp) / "chain512.out"
    rc, _, _ = run(cli, "write", ok, out)
    rc2, so, _ = run(cli, "get", out, "/".join(["a"] * 512) + "/x", "int")
    check("512 levels roundtrip", rc == 0 and rc2 == 0 and so.strip() == "1", "write/reload of 512 levels failed")
    bad = Path(tmp) / "chain513.kv"
    bad.write_text(chain(513), encoding="utf-8", newline="\n")
    rc, _, se = run(cli, "dump", bad)
    check("513 levels", rc == 1 and "nesting too deep" in se, f"rc={rc} stderr={se[:100]!r}")
    return "512 levels load/write/reload, 513 rejected"


# ---------------------------------------------------------------------------------------------
# Performance / memory. Thresholds are deliberately ~4x looser than what an optimized build
# measures on a laptop, so they catch algorithmic regressions (quadratic lookup, whole-file
# buffering, string duplication) without failing on slow machines.
# ---------------------------------------------------------------------------------------------

class Skip(Exception):
    pass


PERF = {
    # dataset: (min load MB/s, min write MB/s, max tree bytes / file bytes, max pool bytes / file bytes)
    "wide":   (40, 100, 1.3, 4.5),   # pool bytes = allocated capacity of every pool array
    "nested": (60, 100, 1.5, 0.45),
    "unique": (50, 100, 1.4, 3.5),
}
MAX_LOOKUP_NS = 2000          # measured ~40; a linear scan of 100k keys is ~13000
MAX_SCALING_RATIO = 8.0       # 4x more keys: linear = 4, quadratic = 16
MAX_RSS_OVER_STRUCTURES = 1.6  # (peak RSS - startup RSS) / (tree + pool bytes); a whole-file buffer breaks it
RSS_SLACK = 3 << 20

_datasets = {}
_optimized = None


def optimized(cli):
    global _optimized
    if _optimized is None:
        _optimized = "optimized=1" in run(cli, "info")[1]
    return _optimized


def dataset(tmp, kind, n):
    key = (kind, n)
    if key not in _datasets:
        path = Path(tmp) / f"{kind}{n}.kv"
        path.write_text(gen_kv.GENERATORS[kind](n), encoding="utf-8", newline="\n")
        _datasets[key] = path
    return _datasets[key]


def bench(cli, tmp, path, runs=5, lookups=200000):
    """Best-of-N timings (each run is a fresh process: the pool is global, so only the first load
    in a process is representative). Memory numbers are deterministic."""
    best = None
    for _ in range(runs):
        rc, so, se = run(cli, "bench", path, Path(tmp) / "bench.out", lookups, timeout=20)
        if rc != 0:
            raise RuntimeError(f"bench failed: {se}")
        m = {k: float(v) for k, v in (l.split() for l in so.splitlines())}
        if best is None:
            best = m
        else:
            for k in ("load_ms", "write_ms", "lookup_ns"):
                best[k] = min(best[k], m[k])
    best["file_bytes"] = path.stat().st_size
    return best


def mb(x):
    return x / (1 << 20)


def perf_dataset(kind, n):
    def fn(cli, tmp):
        if not optimized(cli):
            raise Skip("not an optimized build")
        min_load, min_write, max_tree, max_pool = PERF[kind]
        path = dataset(tmp, kind, n)
        m = bench(cli, tmp, path)
        f = m["file_bytes"]
        load_mbps = mb(f) / (m["load_ms"] / 1000)
        write_mbps = mb(f) / (m["write_ms"] / 1000)
        check(kind, load_mbps >= min_load, f"load {load_mbps:.0f} MB/s < {min_load}")
        check(kind, write_mbps >= min_write, f"write {write_mbps:.0f} MB/s < {min_write}")
        check(kind, m["lookup_ns"] <= MAX_LOOKUP_NS, f"lookup {m['lookup_ns']:.0f} ns > {MAX_LOOKUP_NS}")
        check(kind, m["tree_bytes"] <= max_tree * f, f"tree {m['tree_bytes'] / f:.2f}x file > {max_tree}")
        check(kind, m["pool_bytes"] <= max_pool * f, f"pool {m['pool_bytes'] / f:.2f}x file > {max_pool}")
        check(kind, m["reload_new_strings"] <= 2 and m["reload_new_pool_bytes"] <= 64,
              f"reloading added {m['reload_new_strings']:.0f} strings / {m['reload_new_pool_bytes']:.0f} bytes to the pool")
        structures = m["tree_bytes"] + m["pool_bytes"]
        if m["rss_peak"] and m["rss_start"]:
            used = m["rss_peak"] - m["rss_start"]
            check(kind, used <= MAX_RSS_OVER_STRUCTURES * structures + RSS_SLACK,
                  f"peak RSS grew {mb(used):.1f} MB for {mb(structures):.1f} MB of structures (file is {mb(f):.1f} MB)")
        return (f"{mb(f):.1f} MB: load {m['load_ms']:.1f} ms ({load_mbps:.0f} MB/s), write {m['write_ms']:.1f} ms, "
                f"lookup {m['lookup_ns']:.0f} ns, tree {m['tree_bytes'] / f:.2f}x, pool {m['pool_bytes'] / f:.2f}x, "
                f"peak RSS +{mb(m['rss_peak'] - m['rss_start']):.1f} MB")
    return fn


def perf_dedup(cli, tmp):
    """The pool must hold each distinct string once (plus a handful of internal names)."""
    if not optimized(cli):
        raise Skip("not an optimized build")
    path = dataset(tmp, "nested", 50000)
    text = path.read_text(encoding="utf-8")
    tokens = {a or b for a, b in re.findall(r'"([^"]*)"|([^\s{}"]+)', text)}
    # a number written the way the library would write it is not kept in the pool, its text is made from the value
    canonical = re.compile(r"-?(0|[1-9][0-9]{0,8})|[0-9]+\.[0-9]*[1-9]")  # digits of a float32 round-trip up to 7
    distinct = {t for t in tokens if not (canonical.fullmatch(t) and t != "-0" and len(t.replace(".", "").lstrip("0")) <= 7)}
    m = bench(cli, tmp, path, runs=1, lookups=0)
    n = int(m["pool_strings"])
    check("dedup", len(distinct) <= n <= len(distinct) + 8, f"pool has {n} strings, file has {len(distinct)} distinct that need the pool")
    return f"{len(distinct)} distinct strings in file that need the pool ({len(tokens)} tokens), {n} in pool"


def perf_scaling(cli, tmp):
    """4x more keys in one object must cost about 4x, not 16x."""
    if not optimized(cli):
        raise Skip("not an optimized build")
    small = bench(cli, tmp, dataset(tmp, "wide", 50000))
    large = bench(cli, tmp, dataset(tmp, "wide", 200000))
    ratio = large["load_ms"] / small["load_ms"]
    check("scaling", ratio <= MAX_SCALING_RATIO, f"4x keys took {ratio:.1f}x longer (limit {MAX_SCALING_RATIO})")
    return f"50k keys {small['load_ms']:.1f} ms, 200k keys {large['load_ms']:.1f} ms -> x{ratio:.1f}"



# ---------------------------------------------------------------------------------------------
# String pool: memory accounting, threads, speed. Every memory/speed test runs against both builds
# ("st" = kv_stress_st, plain library; "mt" = kv_stress, KV_THREAD_SAFE library).
# Memory numbers are deterministic, so those are exact/structural checks.
# ---------------------------------------------------------------------------------------------

ENTRY_BYTES = 12        # sizeof(KVStringPool::Entry): {ref, length, hash}
BUCKET_BYTES = 4        # one slot per hash-table bucket
MAX_LOAD = 0.75         # the table doubles before it gets fuller than this
DEFAULT_RESERVE = 64 * 1024
SEGMENT0 = 256          # first entry segment of the thread-safe build


def stress(build, *args, timeout=300):
    rc, so, se = run(TOOLS[build], *args, timeout=timeout)
    try:
        m = {k: float(v) for k, v in (l.split() for l in so.splitlines())}
    except ValueError:
        m = {}
    return rc, m, se


def for_builds(fn):
    """Runs fn(build) for "st" and "mt"; failures are tagged with the build."""
    def wrapper(cli, tmp):
        details = []
        for build in ("st", "mt"):
            before = len(failures)
            d = fn(build)
            failures[before:] = [f"[{build}] {f}" for f in failures[before:]]
            if d:
                details.append(f"{build}: {d}")
        return "; ".join(details)
    return wrapper


@for_builds
def pool_empty(build):
    """A fresh pool holds exactly its default reserve plus the initial table and nothing hidden."""
    rc, m, se = stress(build, "poolmem", 0, 1, 1)
    check("empty", rc == 0, f"rc={rc} {se[:200]}")
    if rc != 0:
        return
    check("empty", m["empty_chars_allocated"] == DEFAULT_RESERVE, f"first chunk is {m['empty_chars_allocated']:.0f} bytes, want {DEFAULT_RESERVE}")
    check("empty", m["empty_chunks"] == 1 and m["empty_strings"] == 0, "fresh pool must have one chunk and no strings")
    check("empty", m["empty_allocated"] <= DEFAULT_RESERVE + 4096, f"fresh pool holds {m['empty_allocated']:.0f} heap bytes")
    check("audit", m["empty_heap"] == m["empty_allocated"] + m["object_size"],
          f"real heap {m['empty_heap']:.0f} != reported {m['empty_allocated']:.0f} + object {m['object_size']:.0f}")
    return f"{m['empty_allocated'] + m['object_size']:.0f} B"


def pool_memory(strings, length, rounds=1):
    @for_builds
    def fn(build):
        rc, m, se = stress(build, "poolmem", strings, length, rounds)
        check("run", rc == 0 and m.get("content_ok") == 1, f"rc={rc} content_ok={m.get('content_ok')} {se[:200]}")
        if rc != 0:
            return
        n = m["strings"]
        check("count", n == strings, f"{n:.0f} strings, expected {strings}")
        if length >= len(str(max(strings - 1, 0))):
            check("chars", m["chars"] == strings * (length + 1), f"{m['chars']:.0f} chars stored, expected {strings * (length + 1)}")
        # 1. the pool's own accounting covers every array: heap = arrays + the pool object, exactly
        check("audit", m["heap_live"] == m["allocated"] + m["object_size"],
              f"real heap {m['heap_live']:.0f} != reported {m['allocated']:.0f} + object {m['object_size']:.0f}")
        check("audit", m["allocated"] == m["chars_allocated"] + m["entries_allocated"] + m["table_allocated"],
              "allocated is not the sum of its arrays")
        # 2. nothing is stored twice and re-interning allocates nothing
        check("dedup", m["allocated"] == m["first_round_allocated"], f"{rounds} rounds grew the pool: {m['first_round_allocated']:.0f} -> {m['allocated']:.0f}")
        # 3. every array stays within a fixed factor of what it has to hold
        check("chars", m["chars_allocated"] <= 2 * m["chars"] + DEFAULT_RESERVE, f"char chunks {m['chars_allocated']:.0f} for {m['chars']:.0f} chars")
        check("entries", m["entries_allocated"] <= 2 * ENTRY_BYTES * n + SEGMENT0 * ENTRY_BYTES, f"entries {m['entries_allocated']:.0f} for {n:.0f} strings")
        check("table", m["table_allocated"] <= 2 * BUCKET_BYTES * n / MAX_LOAD + 4096, f"hash table {m['table_allocated']:.0f} for {n:.0f} strings")
        check("table-load", m["table_allocated"] >= BUCKET_BYTES * n / MAX_LOAD, f"hash table {m['table_allocated']:.0f} is fuller than {MAX_LOAD}")
        if m["allocated"] >= 4 * DEFAULT_RESERVE:
            check("used", m["used"] >= 0.5 * m["allocated"], f"only {m['used'] / m['allocated']:.2f} of the allocated bytes carry data")
        # 4. growth never holds two full copies of an array at once
        check("peak", m["heap_peak"] <= 1.6 * m["heap_live"] + DEFAULT_RESERVE, f"peak heap {m['heap_peak']:.0f} vs final {m['heap_live']:.0f}")
        return f"{m['heap_live'] / n:.1f} B/string, {m['used'] / m['allocated']:.0%} used, peak x{m['heap_peak'] / m['heap_live']:.2f}"
    return fn


@for_builds
def pool_large_strings(build):
    """Strings bigger than a chunk get an exact-size chunk instead of wasting the current one."""
    rc, m, se = stress(build, "poolmem", 10, 2_000_000, 2)
    check("run", rc == 0 and m.get("content_ok") == 1, f"rc={rc} {se[:200]}")
    if rc != 0:
        return
    check("chars", m["chars_allocated"] == m["chars"] + DEFAULT_RESERVE, f"{m['chars_allocated']:.0f} allocated for {m['chars']:.0f} chars")
    check("audit", m["heap_live"] == m["allocated"] + m["object_size"], "real heap != reported")
    check("dedup", m["allocated"] == m["first_round_allocated"], "second round grew the pool")
    return f"{m['chars_allocated'] / (1 << 20):.1f} MB for {m['chars'] / (1 << 20):.1f} MB of text"


def pool_threads(cli, tmp):
    """Concurrent load + intern from many threads: one slot per distinct string, stable pointers, consistent trees."""
    nested = Path(tmp) / "mt_nested.kv"
    nested.write_text(gen_kv.nested(20000), encoding="utf-8", newline="\n")
    summary = []
    for threads, unique, shared, kv in ((8, 20000, 5000, CASES / "good.kv"),
                                         (16, 3000, 3000, nested),
                                         (4, 50000, 50000, CASES / "deep_ok.kv"),
                                         (32, 500, 500, nested)):
        for attempt in range(3):  # different interleavings
            rc, m, se = stress("mt", "mt", kv, threads, unique, shared)
            tag = f"{threads}t/{kv.stem}/#{attempt}"
            check(tag, rc == 0 and m.get("errors") == 0, f"rc={rc} stats={m} {se[:200]}")
            check(tag, m.get("scalars_checked", 0) > 0, "the shared tree had no values to read")
            check(tag, m.get("dump_mismatch") == 0, f"{m.get('dump_mismatch')} threads built a different tree")
            check(tag, m.get("slot_mismatch") == 0, f"{m.get('slot_mismatch')} shared strings got different slots in different threads")
            check(tag, m.get("content_mismatch") == 0, f"{m.get('content_mismatch')} slots resolve to the wrong text")
            check(tag, m.get("new_strings") == m.get("expected_new_strings"),
                  f"pool gained {m.get('new_strings')} strings, expected {m.get('expected_new_strings')} (duplicates or lost inserts)")
            if failures:
                return
        summary.append(f"{threads}t")
    return "x3 runs each: " + ", ".join(summary)


# Single-thread limits (ns/op), ~4x above an optimized laptop build. find_hit is a random access
# into a 1M-string pool, i.e. a few cache misses.
POOL_ST_LIMITS = {"intern_unique_ns": 250, "intern_dup_ns": 150, "find_hit_ns": 800, "find_miss_ns": 250, "view_ns": 30}
MT_BUILD_SLACK = 1.5     # the thread-safe build pays for its locks even with one thread
MIN_READ_SCALING = 1.5   # 8 readers vs 1; measured ~4x, a plain mutex gives ~0.3x
MIN_DUP_SCALING = 0.45   # 8 threads interning the same strings vs 1; measured ~0.85x, a plain mutex ~0.3x


def pool_perf(cli, tmp):
    if not optimized(cli):
        raise Skip("not an optimized build")
    details = []
    threads = max(1, min(8, os.cpu_count() or 1))
    for build in ("st", "mt"):
        rc, m, se = stress(build, "poolperf", 1_000_000, threads, timeout=600)
        check(build, rc == 0, f"rc={rc} {se[:200]}")
        if rc != 0:
            continue
        slack = MT_BUILD_SLACK if build == "mt" else 1.0
        for k, limit in POOL_ST_LIMITS.items():
            check(build, m[k] <= limit * slack, f"{k} {m[k]:.1f} > {limit * slack:.0f}")
        details.append(f"{build}: " + ", ".join(f"{k[:-3]} {m[k]:.0f}" for k in POOL_ST_LIMITS))
        if build == "mt":
            read, dup, uniq = m["read_mops_n"] / m["read_mops_1"], m["dup_mops_n"] / m["dup_mops_1"], m["unique_mops_n"] / m["unique_mops_1"]
            if threads >= 4:
                check("scaling", read >= MIN_READ_SCALING, f"{threads} readers reach only x{read:.2f} of one")
                check("scaling", dup >= MIN_DUP_SCALING, f"{threads} threads interning the same strings reach only x{dup:.2f} of one")
            details.append(f"{threads} threads vs 1: read x{read:.1f}, dup-intern x{dup:.2f}, unique-intern x{uniq:.2f}")
    return "; ".join(details) + " (ns/op)"


def find_tool(cli, name):
    exe = Path(cli).with_name(name + Path(cli).suffix)
    return str(exe) if exe.exists() else None


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    # messages may hold any text; a console that is not UTF-8 (Windows) must not turn a report into a crash
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    cli = str(Path(sys.argv[1]).resolve())  # never looked up on PATH, whatever the working directory is
    wanted = set()
    TOOLS["mt"] = find_tool(cli, "kv_stress")
    TOOLS["st"] = find_tool(cli, "kv_stress_st")
    for a in sys.argv[2:]:
        if a.startswith("--stress="):
            TOOLS["mt"] = str(Path(a[len("--stress="):]).resolve())
        elif a.startswith("--stress-st="):
            TOOLS["st"] = str(Path(a[len("--stress-st="):]).resolve())
        else:
            wanted.add(a)
    for build, tool in (("mt", "kv_stress"), ("st", "kv_stress_st")):
        if not TOOLS[build]:
            print(f"error: {tool} not found next to {cli}; build it (cmake builds it with kv_cli) or pass --stress{'-st' if build == 'st' else ''}=PATH")
            return 2
    rc, so, _ = run(TOOLS["mt"], "info")
    if "thread_safe 1" not in so:
        print(f"error: {TOOLS['mt']} is not a KV_THREAD_SAFE build")
        return 2
    print(f"single thread: {cli}, {TOOLS['st']}\nmultithread:   {TOOLS['mt']} ({MT_LOAD_THREADS} threads per case)\n")

    tests = [(kv.stem, lambda c, t, kv=kv: test_case(c, kv, t))
             for kv in sorted(CASES.glob("*.kv"))]
    tests += [(f.__name__, f) for f in (misc_missing_file, misc_no_args, misc_bad_command, misc_write_unwritable, logging, fail_cleanup, write_large_values, misc_kvdef, misc_api, anonymous_root, encodings, utf16_equivalence, utf16_boundaries, dups_gameinfo, dups_blocks,
                                         stream_boundaries, stream_long_tokens, stream_error_position,
                                         deep_branching, depth_limit)]
    tests += [("perf_wide", perf_dataset("wide", 100000)),
              ("perf_nested", perf_dataset("nested", 50000)),
              ("perf_unique", perf_dataset("unique", 100000)),
              ("perf_dedup", perf_dedup),
              ("perf_scaling", perf_scaling),
              ("pool_empty", pool_empty),
              ("pool_mem_small", pool_memory(1000, 8)),
              ("pool_mem_100k", pool_memory(100_000, 40, 2)),
              ("pool_mem_1m", pool_memory(1_000_000, 12, 3)),
              ("pool_mem_large", pool_large_strings),
              ("pool_threads", pool_threads),
              ("pool_perf", pool_perf)]
    if wanted:
        tests = [t for t in tests if t[0] in wanted]

    passed = skipped = 0
    with tempfile.TemporaryDirectory() as tmp:
        for name, fn in tests:
            failures.clear()
            detail = None
            try:
                detail = fn(cli, tmp)
            except Skip as e:
                print(f"SKIP {name} ({e})")
                skipped += 1
                sys.stdout.flush()
                continue
            except Exception as e:  # noqa: BLE001 - a crashing test is a failing test
                failures.append(f"{type(e).__name__}: {e}")
            if failures:
                print(f"FAIL {name}")
                for f in failures:
                    print("     " + f.replace("\n", "\n     "))
            else:
                print(f"PASS {name}" + (f"  [{detail}]" if detail else ""))
                passed += 1
            sys.stdout.flush()

    failed = len(tests) - passed - skipped
    print(f"\n{passed} passed, {failed} failed, {skipped} skipped (of {len(tests)})")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
