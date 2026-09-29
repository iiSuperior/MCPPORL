#!/usr/bin/env python3
"""Print selected members from a decompiled Java source file.

Usage: extract_methods.py <File.java> <name> [<name> ...]

Each <name> prints every method or constructor with that name (all
overloads), including its annotations and body. Special names:
  :fields     all field declarations at class-body level
  :signatures one line per method/constructor declaration
  :all        the whole file
"""
from __future__ import annotations

import re
import sys

DECL = re.compile(
    r"^(\s*)(?:@\w+(?:\([^)]*\))?\s+)*"
    r"(?:(?:public|protected|private|static|final|abstract|synchronized|native|default|strictfp)\s+)*"
    r"(?:<[^>]+>\s+)?(?:[\w.$<>\[\], ?]+\s+)?(\w+)\s*\(")
KEYWORDS = {"if", "for", "while", "switch", "catch", "return", "new", "throw", "else", "synchronized", "try", "do"}


def member_indent(lines: list[str]) -> str:
    for ln in lines:
        m = re.match(r"^(\s*)(public |protected |private |final |abstract )*(class|interface|record|enum) ", ln)
        if m:
            return m.group(1) + "   "
    return "   "


def is_decl(line: str, indent: str) -> str | None:
    if not line.startswith(indent) or line[len(indent)] == " ":
        return None
    m = DECL.match(line)
    if not m or m.group(2) in KEYWORDS or line.rstrip().endswith(";") and "(" not in line.split("=")[0]:
        return None
    return m.group(2)


def block_end(lines: list[str], start: int) -> int:
    depth, seen = 0, False
    for i in range(start, len(lines)):
        code = re.sub(r'"(\\.|[^"\\])*"|\'(\\.|[^\'\\])*\'|//.*', "", lines[i])
        depth += code.count("{") - code.count("}")
        seen = seen or "{" in code
        if seen and depth <= 0:
            return i
        if not seen and code.rstrip().endswith(";"):
            return i  # abstract / interface method
    return len(lines) - 1


def annotations_start(lines: list[str], i: int) -> int:
    while i > 0 and lines[i - 1].strip().startswith("@"):
        i -= 1
    return i


def main(argv: list[str]) -> int:
    if len(argv) < 3:
        print(__doc__, file=sys.stderr)
        return 2
    path, names = argv[1], argv[2:]
    with open(path, encoding="utf-8") as f:
        lines = f.read().splitlines()
    indent = member_indent(lines)

    if ":all" in names:
        print("\n".join(lines))
        return 0

    found: dict[str, int] = {n: 0 for n in names if not n.startswith(":")}
    for i, line in enumerate(lines):
        name = is_decl(line, indent)
        if ":fields" in names and name is None and line.startswith(indent) and not line.startswith(indent + " ") \
                and line.rstrip().endswith(";") and "(" not in line.split("=")[0]:
            print(line.strip())
        if name is None:
            continue
        if ":signatures" in names:
            print(line.strip().rstrip("{").rstrip())
        if name in found:
            found[name] += 1
            end = block_end(lines, i)
            print("\n".join(lines[annotations_start(lines, i):end + 1]))
            print()
    for n, count in found.items():
        if count == 0:
            print(f"!! no member named {n!r} in {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
