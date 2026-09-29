# Trace format (v1)

One format for oracle traces, simulator traces and opt-in recorded fights.
The file is JSON Lines: a header line, followed by one line per game tick.

Floating-point values are stored as **hex bit patterns**, never as decimal
text, so a round trip is lossless and comparisons are bit-exact.

## Header

```json
{"format": "mcporl-trace", "version": 1,
 "mc_version": "26.x", "source": "oracle",
 "domains": ["movement", "melee"],
 "fields": {"pos.x": "f64", "pos.y": "f64", "pos.z": "f64",
            "yRot": "f32", "health": "f32", "hurtTime": "i32", "onGround": "bool"},
 "meta": {}}
```

- `source`: `oracle`, `sim` or `recording`.
- `domains`: the mechanic domains the trace exercises. The diff tool only
  compares fields that both traces declare.
- `fields`: per-entity state fields and their types. Types are `f32`, `f64`
  (hex bits, 8 or 16 digits), `i32`, `i64` (JSON integers) and `bool`.

## Tick lines

```json
{"t": 0,
 "inputs": {"p0": {"fwd": 1, "strafe": 0, "jump": false, "sprint": true, "yaw": "43340000", "pitch": "00000000"}},
 "entities": {"p0": {"pos.x": "3fe0000000000000", "health": "41a00000", "hurtTime": 0, "onGround": true}}}
```

- `t`: tick index, starting at 0 and strictly increasing.
- `inputs`: what each controlled entity's input was **as processed on this
  tick** (for recordings: the packets the server applied this tick, which
  already includes network delay).
- `entities`: state **after** the tick, keyed by a trace-local entity id.
  Recordings use random per-recording ids, never Minecraft UUIDs or names.

## Comparing

`tools/tracediff.py a.jsonl b.jsonl` reports the first tick and field where
the traces differ, with both values and the distance in ULPs, then a
per-field summary. It exits 0 only if every shared field matches exactly.
