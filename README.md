# MCPPORL

A PPO agent that learns modern Minecraft Java PvP, trained in a GPU-batched
simulator that is **bit-exact with vanilla** in every mechanic it covers.

Goals:

- **Parity in known domains.** Each simulated mechanic matches vanilla tick
  for tick, verified against the real game. Mechanics we haven't modelled yet
  are left out rather than approximated, and adding one later is cheap.
- **Fast training** on an AMD GPU (RX 9070 XT, HIP/ROCm on Windows).
- **Realistic conditions:** randomised network latency and jitter, and
  procedurally varied arenas.
- **The modern PvP stack**, built up in phases: melee, shields, totems,
  golden apples, projectiles, pearls, potions, blocks, elytra, maces, spears.
- **A server plugin** so people can fight the bot, with **opt-in recording**
  of fights so the bot can learn from the ones it loses.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the design and phase plan,
[docs/PERFORMANCE.md](docs/PERFORMANCE.md) for the throughput budget,
and [docs/TRACE_FORMAT.md](docs/TRACE_FORMAT.md) for the shared trace format.

## Layout

| Path | What |
|---|---|
| `sim/` | C++ simulator core (header-only, CPU now, HIP backend later) |
| `sim/include/mcp/jmath.hpp` | Java-exact casts, integer wrapping and `Mth` ports |
| `sim/include/mcp/latency.hpp` | TCP-style latency and jitter model |
| `oracle/jref/` | JVM-generated reference vectors for the math layer |
| `tools/tracediff.py` | Bit-exact trace comparison |
| `docs/` | Architecture and formats |

## Building and testing

Requires CMake 3.20+, a C++17 compiler and Java 21+ (for the parity vectors).

```sh
cmake -S sim -B sim/build -DCMAKE_BUILD_TYPE=Release
cmake --build sim/build --config Release
ctest --test-dir sim/build -C Release --output-on-failure

python3 -m unittest discover -s tools
```

On Windows with Visual Studio, `--config Release` matters: Visual Studio
generators ignore `CMAKE_BUILD_TYPE` and build Debug by default.

## Status

Phase 1 in progress. Target version: **26.3** (unobfuscated, Java 25).

- The math layer (`sin`/`cos`, the sine table, `floor`/`ceil`, Java casts) is
  verified against the JVM and the real 26.3 jar (`oracle-probe` workflow).
- The oracle harness (`oracle/harness`) runs vanilla 26.3 on CI and records
  golden traces (`oracle/traces`).
- The C++ player movement port (`sim/include/mcp/player.hpp`) reproduces all
  flat-ground golden traces **bit for bit**: standing, walking, sprinting,
  sprint-jumping, jumping, diagonal strafing, sprint-turning, sneaking and
  mid-air turns. `ctest` runs these parity checks offline.
- The C++ melee port (`sim/include/mcp/duel.hpp`) reproduces all 27
  two-player combat traces **bit for bit**: plain, sprint and critical hits,
  the 180 hit, invulnerability, W-tap, moving, sprinting and airborne victims,
  same-tick trades, partial-strength hits, the post-hit sprint echo, and every
  sword and axe (`weapons.hpp`: damage, attack speed, cooldown)
  (`combat_*` tests; `duel_replay` and `bench_duel` tools).
- Clicks are resolved by a port of the client's crosshair pick (`pick.hpp`,
  4000 vanilla picks matched bit for bit), so the bot can only hit what it
  aims at (`fairness.hpp`: turn-rate cap, one click per tick). Deaths end an
  episode (`Duel::done`, `winner`, `reset`); overlapping players push.
- Each client sees the opponent the way a vanilla client does
  (`tracker.hpp`): the server's entity tracker (update cadence, 1/4096 deltas,
  position syncs) and the client's remote-player interpolation, matched bit
  for bit against the oracle. Clicks aim at that view, so a moving opponent
  is hit where it is seen, about a tick late.
- A PPO agent trained in the simulator (`docs/TRAINING.md`) learns to fight:
  from 0% to 86% wins against a hard scripted aim bot in 15 minutes of CPU,
  with 90% of its clicks landing. Its fights replay bit for bit on the real
  26.3 server (`oracle/replays`, `replay_*` tests).
