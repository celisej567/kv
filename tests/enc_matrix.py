#!/usr/bin/env python3
"""Text-encoding matrix: the same small file written in many encodings, loaded with kv_cli.

The library works on bytes, so what matters is whether the bytes of an encoding collide with the
syntax characters. For every encoding and every sample (a quoted value, a bare value, a quoted value
with escape sequences on) the result is one of
  ok    loads, the value comes back byte for byte, write -> load gives the same tree
  NUL   rejected with "NUL byte in file" (UTF-16/32)
  BAD   loads (or fails) but not as the author meant: a trail byte of a multi-byte character
        equals a syntax character

run_tests.py runs it as the test `encodings` and compares with EXPECTED. Run it directly to print
the Markdown table that docs/DETAILS.md quotes:  python3 -I tests/enc_matrix.py <kv_cli>
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ESC_BYTES = {ord("\\"): b"\\\\", ord("\n"): b"\\n", ord("\t"): b"\\t", ord("\r"): b"\\r"}


def kv_escape(b):
    return b"".join(ESC_BYTES.get(c, bytes([c])) for c in b)


# (name, python codec, a sample of text in that script, what it is)
ENCODINGS = [
    ("ASCII", "ascii", "plain text 123", "7-bit"),
    ("UTF-8", "utf-8", "Привет мир 日本語 çödé 😀", "Unicode, ASCII-compatible"),
    ("UTF-8 with BOM", "utf-8-sig", "Привет мир 日本語", "BOM is skipped"),
    ("UTF-16 LE with BOM", "utf-16", "Привет", "wide"),
    ("UTF-16 LE", "utf-16-le", "Привет", "wide"),
    ("UTF-16 BE", "utf-16-be", "Привет", "wide"),
    ("UTF-32 LE with BOM", "utf-32", "Привет", "wide"),
    ("UTF-32 BE", "utf-32-be", "Привет", "wide"),
    ("UTF-7", "utf-7", "Привет мир", "7-bit"),
    ("Latin-1 (ISO 8859-1)", "latin-1", "çödé ñandú ÿ", "8-bit"),
    ("Windows-1252", "cp1252", "çödé € ‘x’", "8-bit"),
    ("ISO 8859-2", "iso8859-2", "zażółć gęślą", "8-bit"),
    ("Windows-1251", "cp1251", "Привет мир", "8-bit"),
    ("KOI8-R", "koi8-r", "Привет мир", "8-bit"),
    ("CP866", "cp866", "Привет мир", "8-bit"),
    ("Shift_JIS", "shift_jis", "日本語 ソ表能", "multi-byte, trail bytes 40..FC"),
    ("EUC-JP", "euc-jp", "日本語 ソ表能", "multi-byte, all bytes 80..FF"),
    ("GBK", "gbk", "中文 简体 乗", "multi-byte, trail bytes 40..FE"),
    ("Big5", "big5", "中文 繁體 功", "multi-byte, trail bytes 40..FE"),
    ("EUC-KR", "euc-kr", "한국어 조선", "multi-byte, all bytes 80..FF"),
]

SYNTAX_TRAILS = b'\\{}[]/"'


def trail_sample(codec):
    """A short text of characters whose encoding contains a syntax byte; None for single-byte codecs."""
    out = []
    for cp in range(0x80, 0x10000):
        ch = chr(cp)
        try:
            raw = ch.encode(codec)
        except UnicodeError:
            continue
        if len(raw) > 1 and any(b in SYNTAX_TRAILS for b in raw[1:]):
            out.append(ch)
            if len(out) == 6:
                break
    return "".join(out) or None


def kv_unescape(b):
    return (b.replace(b"\\n", b"\n").replace(b"\\t", b"\t").replace(b"\\r", b"\r")
             .replace(b"\\\\", b"\\"))


def run(cli, args, env_esc=False):
    env = {**os.environ, "KV_ESC": "1" if env_esc else "0"}
    p = subprocess.run([str(cli), *map(str, args)], capture_output=True, timeout=60, env=env)
    return p.returncode, p.stdout, p.stderr


def attempt(cli, tmp, codec, text, style, esc):
    """Loads one file; returns 'ok', 'NUL' or 'BAD'."""
    if style == "bare":
        text = text.replace(" ", "")
        src = f'"Root" {{\n\tkey {text}\n}}\n'
    else:
        src = f'"Root" {{\n\t"key" "{text}"\n}}\n'
    try:
        data = src.encode(codec)
    except UnicodeError:
        return "n/a"
    f = Path(tmp) / "enc.kv"
    f.write_bytes(data)
    rc, so, se = run(cli, ["dump", f], esc)
    if rc != 0:
        return "NUL" if b"NUL byte in file" in se else "BAD"
    head = b"root/key\tstring\t"
    if not so.startswith(head) or not so.endswith(b"\n"):
        return "BAD"
    try:
        # the value as the library hands it out, decoded the way the author encoded it
        if kv_unescape(so[len(head):-1]).decode(codec.replace("-sig", "")) != text:
            return "BAD"
    except UnicodeError:
        return "BAD"
    w = Path(tmp) / "enc.out"
    rc, _, _ = run(cli, ["write", f, w], esc)
    if rc != 0:
        return "BAD"
    rc, so2, _ = run(cli, ["dump", w], esc)
    return "ok" if rc == 0 and so2 == so else "BAD"


def matrix(cli):
    """[(label, kind, quoted, bare, escaped)] for every encoding and, for multi-byte ones, its trail-byte sample."""
    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        for label, codec, text, kind in ENCODINGS:
            rows.append((label, kind, *(attempt(cli, tmp, codec, text, s, e)
                                        for s, e in (("quoted", False), ("bare", False), ("quoted", True)))))
            t = trail_sample(codec) if kind.startswith("multi") else None
            if t:
                rows.append((label + ", chars with a syntax byte", "sample " + ascii(t.encode(codec))[1:],
                             *(attempt(cli, tmp, codec, t, s, e)
                               for s, e in (("quoted", False), ("bare", False), ("quoted", True)))))
    return rows


# The pinned result, including what the library cannot do, so that a change shows up as a failure.
EXPECTED = {
    'ASCII': ('ok', 'ok', 'ok'),
    'UTF-8': ('ok', 'ok', 'ok'),
    'UTF-8 with BOM': ('ok', 'ok', 'ok'),
    'UTF-16 LE with BOM': ('NUL', 'NUL', 'NUL'),
    'UTF-16 LE': ('NUL', 'NUL', 'NUL'),
    'UTF-16 BE': ('NUL', 'NUL', 'NUL'),
    'UTF-32 LE with BOM': ('NUL', 'NUL', 'NUL'),
    'UTF-32 BE': ('NUL', 'NUL', 'NUL'),
    'UTF-7': ('ok', 'ok', 'ok'),
    'Latin-1 (ISO 8859-1)': ('ok', 'ok', 'ok'),
    'Windows-1252': ('ok', 'ok', 'ok'),
    'ISO 8859-2': ('ok', 'ok', 'ok'),
    'Windows-1251': ('ok', 'ok', 'ok'),
    'KOI8-R': ('ok', 'ok', 'ok'),
    'CP866': ('ok', 'ok', 'ok'),
    'Shift_JIS': ('ok', 'BAD', 'BAD'),
    'Shift_JIS, chars with a syntax byte': ('ok', 'BAD', 'ok'),
    'EUC-JP': ('ok', 'ok', 'ok'),
    'GBK': ('ok', 'ok', 'BAD'),
    'GBK, chars with a syntax byte': ('ok', 'BAD', 'ok'),
    'Big5': ('ok', 'ok', 'BAD'),
    'Big5, chars with a syntax byte': ('ok', 'BAD', 'ok'),
    'EUC-KR': ('ok', 'ok', 'ok'),
}


def markdown(rows):
    out = ["| encoding | note | quoted value | unquoted value | quoted, escapes on |", "|---|---|---|---|---|"]
    for r in rows:
        out.append("| " + " | ".join(r) + " |")
    return "\n".join(out)


if __name__ == "__main__":
    print(markdown(matrix(Path(sys.argv[1]).resolve())))
