# MCPPORL architecture

A PPO agent that learns modern (latest-release) Minecraft Java PvP by training
in a GPU-batched reimplementation of the game's combat-relevant logic, verified
tick-for-tick against the real game.

## Guiding principle: parity in known domains

We do not try to reimplement Minecraft. We reimplement a **named, versioned set
of mechanics** ("domains"), and within each domain the simulator must be
**bit-exact** with vanilla. Anything outside the known domains is simply not
simulated: the bot won't be good at it, but it also can't learn exploits from a
half-correct version of it.

Adding a mechanic later must be cheap:

- **Mechanics are modules.** Each domain (movement, melee, shields, totems,
  pearls, …) lives in its own module with its own parity tests and can be
  switched on or off per scenario.
- **The action space is a fixed superset with masking.** New item actions get
  reserved slots, so adding a mechanic means unmasking actions instead of
  changing the network's output shape.
- **Observations are versioned.** Each schema version is a list of named
  feature blocks. A new block means new input columns; old checkpoints can be
  expanded with zero-initialised weights and fine-tuned rather than trained
  from scratch.

## Components

```
            ┌────────────────────┐        traces (same format)        ┌───────────────────┐
            │  Oracle            │ ─────────────────────────────────▶ │  tracediff        │
            │  vanilla code,     │                                    │  bit-exact diff,  │
            │  headless, scripted│ ◀── same inputs replayed ───┐      │  first divergence │
            └────────────────────┘                             │      └───────────────────┘
                                                               │               ▲
            ┌────────────────────┐                             │               │
            │  sim (C++)         │ ── CPU reference backend ───┴───────────────┘
            │  one source, two   │ ── HIP backend (batched, 9070 XT) ── must equal CPU
            │  backends          │
            └────────────────────┘
                      │ batched env API
                      ▼
            ┌────────────────────┐        ONNX         ┌────────────────────────────┐
            │  trainer (PyTorch, │ ──────────────────▶ │  server plugin              │
            │  PPO, self-play    │                     │  fake player on a real      │
            │  league)           │ ◀── opt-in recorded │  server + fight recorder    │
            └────────────────────┘     fights ──────── └────────────────────────────┘
```

### Oracle: where "truth" comes from

**Player movement is client-authoritative in Minecraft.** The server does not
simulate player physics; it receives positions from the client and checks
them. Knockback is sent to the client as a velocity, and the *client*
integrates it. So for players, parity means parity with the movement code that
runs on the client (`LivingEntity`/`Player` travel logic plus the local-player
input handling), not with anything the server computes.

The plan for the oracle is therefore a headless Java harness that loads the
official game jar and drives vanilla entity code directly with scripted inputs,
recording state every tick. Starting with the 26.x line, Mojang ships the jar
without obfuscation (to be confirmed once the jar is reachable), which makes
calling vanilla code directly practical.

Server-authoritative mechanics (damage, armor, cooldowns, totems, potion
effects, projectiles, explosions) are recorded from the same harness running a
real server tick.

### Floating-point rules (non-negotiable for bit-exactness)

Java has been strict IEEE 754 since Java 17: no extended precision and no
fused multiply-add. The simulator must match that:

- Build with `-ffp-contract=off` and never with `-ffast-math`, on both host
  and HIP compilers.
- Keep Java's `float`/`double` types exactly as vanilla uses them, including
  the implicit widenings and narrowings.
- Use Java cast semantics: float/double → int saturates and NaN → 0. In C++
  that cast is undefined behaviour, so always go through `jmath` helpers.
- Port Mojang's `Mth` helpers (lookup-table `sin`/`cos`, `floor`, …) instead
  of using libm.
- The HIP backend must match the CPU backend bit for bit; that is a test, not
  a hope.

### Latency

Real fights are played through a network. Each environment draws a
per-episode base latency and per-packet jitter. Packets travel over TCP, so
they are **delayed but never reordered**: jitter shows up as ticks where the
server processes zero packets from a client and later ticks where it
processes two or more. The policy observes the world as delayed by its own
latency, and its actions arrive late. See `sim/include/mcp/latency.hpp`.

### Arenas

Procedural arena generator plus a fixed held-out evaluation set. Block
collision is part of the movement domain, so arenas start with a restricted
block palette and grow as collision shapes pass parity.

### Recorded real fights (opt-in)

Players on the bot server can opt in to having their fights recorded ("can we
record this please 😁"). Recordings use **the same trace format as oracle
traces**, which gives us three uses for free:

1. **Scenario replay.** Rewind a lost fight to any tick, load that state into
   the simulator, and train the bot from there (reset-to-state curriculum).
2. **Opponent modelling.** Behaviour-clone human play styles into extra
   opponents for the self-play league, so the bot doesn't only learn to beat
   itself.
3. **Parity checking.** Real-world traces catch divergences our scripted
   oracle scenarios missed.

Because movement is client-authoritative, the server sees exactly what the
bot sees: the packets each client sent and the tick they arrived on. That is
precisely what a replay needs. Recordings store a random per-recording player
ID, not a Minecraft UUID or username, and are deletable on request.

## Domains and phases

| Phase | Domains |
|---|---|
| 0 | Repo, trace format, diff tool, Java-exact math layer, latency model, oracle harness |
| 1 | Movement on flat ground, melee (attack cooldown, crits, sprint knockback), CPU then HIP; first PPO duelist |
| 2 | Shields, axes disabling shields, food / golden apples, totems, armor; latency on by default |
| 3 | Bows, crossbows, fishing rods, ender pearls, splash potions, cobwebs, buckets |
| 4 | Block placing and mining, arena generator, falling |
| 5 | Elytra, mace, spears, TNT / explosions |
| ongoing | Server plugin, opt-in fight recorder, league and evaluation |

## Target hardware

Training runs on an RX 9070 XT (RDNA4, `gfx1201`) under Windows with ROCm/HIP.
The simulator's HIP backend is plain HIP C++ exposed to Python through a thin
binding, so it does not depend on any particular PyTorch-ROCm build.
