#!/usr/bin/env python3
"""Bit-exact diff of two MCPPORL traces (see docs/TRACE_FORMAT.md).

Exit status: 0 if every shared field matches bit for bit, 1 on divergence,
2 on malformed input.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from collections import defaultdict
from dataclasses import dataclass, field

FLOAT_TYPES = {"f32": (8, ">I", ">f"), "f64": (16, ">Q", ">d")}
SCALAR_TYPES = {"i32", "i64", "bool"}


class TraceError(Exception):
    pass


@dataclass
class Trace:
    header: dict
    ticks: list[dict]

    @property
    def fields(self) -> dict[str, str]:
        return self.header["fields"]


def load(path: str) -> Trace:
    with open(path, encoding="utf-8") as f:
        lines = [ln for ln in f if ln.strip()]
    if not lines:
        raise TraceError(f"{path}: empty file")
    header = json.loads(lines[0])
    if header.get("format") != "mcporl-trace" or header.get("version") != 1:
        raise TraceError(f"{path}: not an mcporl-trace v1 file")
    for name, typ in header.get("fields", {}).items():
        if typ not in FLOAT_TYPES and typ not in SCALAR_TYPES:
            raise TraceError(f"{path}: field {name!r} has unknown type {typ!r}")
    ticks = [json.loads(ln) for ln in lines[1:]]
    for prev, cur in zip(ticks, ticks[1:]):
        if cur["t"] <= prev["t"]:
            raise TraceError(f"{path}: tick {cur['t']} does not increase")
    return Trace(header, ticks)


def float_bits(typ: str, raw) -> int:
    digits = FLOAT_TYPES[typ][0]
    if not isinstance(raw, str) or len(raw) != digits:
        raise TraceError(f"{typ} value must be {digits} hex digits, got {raw!r}")
    return int(raw, 16)


def decode_float(typ: str, bits: int) -> float:
    _, ipack, fpack = FLOAT_TYPES[typ]
    return struct.unpack(fpack, struct.pack(ipack, bits))[0]


def ordered(typ: str, bits: int) -> int:
    """Map float bits to a monotonic integer line so ULP distance is a subtraction."""
    width = 32 if typ == "f32" else 64
    sign = 1 << (width - 1)
    return sign - (bits & ~sign) if bits & sign else sign + bits


def ulps(typ: str, a: int, b: int) -> int:
    return abs(ordered(typ, a) - ordered(typ, b))


@dataclass
class Mismatch:
    tick: int
    entity: str
    field: str
    typ: str
    a: object
    b: object
    ulps: int | None = None

    def describe(self) -> str:
        if self.typ in FLOAT_TYPES:
            va, vb = decode_float(self.typ, self.a), decode_float(self.typ, self.b)
            w = FLOAT_TYPES[self.typ][0]
            dist = "signed zero" if self.ulps == 0 else f"{self.ulps} ulp"
            return (f"tick {self.tick} {self.entity}.{self.field}: "
                    f"{va!r} (0x{self.a:0{w}x}) vs {vb!r} (0x{self.b:0{w}x}), {dist}")
        return f"tick {self.tick} {self.entity}.{self.field}: {self.a!r} vs {self.b!r}"


@dataclass
class Report:
    shared_fields: list[str]
    ticks_compared: int = 0
    mismatches: list[Mismatch] = field(default_factory=list)
    structural: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.mismatches and not self.structural


def compare(a: Trace, b: Trace) -> Report:
    shared = sorted(set(a.fields) & set(b.fields))
    report = Report(shared)
    for name in shared:
        if a.fields[name] != b.fields[name]:
            report.structural.append(
                f"field {name!r} is {a.fields[name]} in A but {b.fields[name]} in B")
    if report.structural:
        return report

    if len(a.ticks) != len(b.ticks):
        report.structural.append(f"tick count differs: {len(a.ticks)} vs {len(b.ticks)}")

    for ta, tb in zip(a.ticks, b.ticks):
        if ta["t"] != tb["t"]:
            report.structural.append(f"tick index differs: {ta['t']} vs {tb['t']}")
            break
        report.ticks_compared += 1
        ea, eb = ta.get("entities", {}), tb.get("entities", {})
        if set(ea) != set(eb):
            report.structural.append(
                f"tick {ta['t']}: entity sets differ: {sorted(ea)} vs {sorted(eb)}")
            break
        for ent in sorted(ea):
            for name in shared:
                typ = a.fields[name]
                if name not in ea[ent] or name not in eb[ent]:
                    if (name in ea[ent]) != (name in eb[ent]):
                        report.mismatches.append(
                            Mismatch(ta["t"], ent, name, typ, ea[ent].get(name), eb[ent].get(name)))
                    continue
                if typ in FLOAT_TYPES:
                    va, vb = float_bits(typ, ea[ent][name]), float_bits(typ, eb[ent][name])
                    if va != vb:
                        report.mismatches.append(
                            Mismatch(ta["t"], ent, name, typ, va, vb, ulps(typ, va, vb)))
                elif ea[ent][name] != eb[ent][name]:
                    report.mismatches.append(
                        Mismatch(ta["t"], ent, name, typ, ea[ent][name], eb[ent][name]))
    return report


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--max", type=int, default=20, help="mismatches to print (default 20)")
    args = ap.parse_args(argv)
    try:
        report = compare(load(args.a), load(args.b))
    except (TraceError, json.JSONDecodeError, KeyError, ValueError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 2

    for msg in report.structural:
        print(f"STRUCTURE: {msg}")
    if report.mismatches:
        first = report.mismatches[0]
        print(f"FIRST DIVERGENCE: {first.describe()}")
        for m in report.mismatches[1:args.max]:
            print(f"  {m.describe()}")
        per_field: dict[str, int] = defaultdict(int)
        for m in report.mismatches:
            per_field[m.field] += 1
        print("mismatches per field: " + ", ".join(f"{k}={v}" for k, v in sorted(per_field.items())))
    print(f"compared {report.ticks_compared} ticks over {len(report.shared_fields)} shared fields: "
          f"{'IDENTICAL' if report.ok else 'DIVERGED'}")
    return 0 if report.ok else 1


if __name__ == "__main__":
    sys.exit(main())
