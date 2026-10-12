#!/usr/bin/env python3
"""Deterministic generators for big KV files used by the performance tests.

  wide(n)    one object with n flat keys (worst case for per-object key lookup)
  nested(n)  n small objects with 6 keys each, values drawn from a small vocabulary
             (typical config/data file; strings repeat a lot)
  unique(n)  n objects whose keys and values are all distinct (worst case for the string pool)
"""
import random
import sys


def wide(n):
    out = ['"Wide"', '{']
    for i in range(n):
        k = i % 3
        v = str(i) if k == 0 else (f"{i}.{i % 97}25" if k == 1 else f'"value number {i}"')
        out.append(f'    "key_{i}" {v}')
    out.append('}')
    return "\n".join(out) + "\n"


def nested(n):
    rnd = random.Random(1234)
    vocab = [f"word{i}" for i in range(200)]
    out = ['"Nested"', '{']
    for i in range(n):
        out.append(f'    "item_{i}"')
        out.append('    {')
        out.append(f'        "name" "{rnd.choice(vocab)}"')
        out.append(f'        "kind" "{rnd.choice(vocab[:20])}"')
        out.append(f'        "count" {rnd.randrange(1000)}')
        out.append(f'        "weight" {rnd.randrange(1000)}.{rnd.randrange(100):02d}')
        out.append(f'        "enabled" {rnd.choice(["true", "false"])}')
        out.append('        "pos"')
        out.append('        {')
        out.append(f'            "x" {rnd.randrange(100)}')
        out.append(f'            "y" {rnd.randrange(100)}')
        out.append('        }')
        out.append('    }')
    out.append('}')
    return "\n".join(out) + "\n"


def unique(n):
    out = ['"Unique"', '{']
    for i in range(n):
        out.append(f'    "group_{i}_{i * 7919 % 100003}"')
        out.append('    {')
        out.append(f'        "label_{i}" "text-{i * 2654435761 % 4294967296:x}-{i}"')
        out.append('    }')
    out.append('}')
    return "\n".join(out) + "\n"


GENERATORS = {"wide": wide, "nested": nested, "unique": unique}

if __name__ == "__main__":
    kind, n, path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
    with open(path, "w", newline="\n") as f:
        f.write(GENERATORS[kind](n))
