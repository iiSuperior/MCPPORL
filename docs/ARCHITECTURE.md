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

#### Two-player combat oracle (zero latency)

Combat parity needs the real client/server split, so each player exists twice
in the harness: a **client copy** (an `OraclePlayer` that owns movement, as
`LocalPlayer` does) and a **server copy** (a real `ServerPlayer` on a mock
connection). One harness tick follows the real order:

1. Each client: `handleKeybinds` step. A scripted click queues an attack
   packet and resets the client's attack strength (the client-side
   `Player.attack` does nothing else: `hurtClient` is false).
2. Each client: movement tick, then `sendChanges` (input, sprint command,
   position/rotation), then `ServerboundClientTickEndPacket`.
3. Server: each client's packets in send order (so an attack uses the
   rotation sent on the previous tick), then one server tick.
4. Each client: apply the server's replies (knockback velocity through the
   `LpVec3` round trip, the echoed sprint flag and movement-speed attribute).

What the server copy actually does (verified against the traces):

- It runs its own **shadow physics** every tick (`AUTHORITATIVE_SIDE_AND_SERVER`
  lets the server simulate players) with **no input**, then `tickPlayer`
  snaps it back to the last position the client sent. Only its velocity and
  ground state carry over, and knockback is computed from that server-side
  velocity: a walking victim is knocked back as if standing, an airborne one
  keeps the server's (one tick stale) vertical velocity.
- `handlePlayerPositionChange` calls `jumpFromGround` on the copy when the
  client leaves the ground moving up, including the sprint-jump boost. Being
  launched by knockback counts.
- Fall distance, used by crits, is accumulated from the client's reported
  positions (`doCheckFallDamage`).
- Changes to the server's sprint flag are echoed to the player's own client
  as entity data plus the movement-speed attribute; the client applies both.
- Server copies do not push each other. A real client is pushed locally by
  the remote player; neither the oracle nor the simulator models that yet
  (the simulator counts such ticks, `Duel::clientPushTicks`).
- The oracle disables natural health regeneration: it is driven by hunger,
  which is not in the known domain yet.

The simulator mirrors this structure in `sim/include/mcp/duel.hpp`; every
scenario in `oracle/combat/` must replay bit-identically (`combat_*` tests).

Latency is added later by delaying the packet queues, using the same
`DelayLine` model as the simulator.

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

### Tricks that depend on facing and packet order

Some real PvP techniques exist only because of *when* the server sees what,
for example the "180 hit": part of melee knockback follows the attacker's
facing rather than their position, so turning around as the hit lands can pull
the victim toward you. The simulator and action space must therefore model:

- rotation and attack as separate actions in the same tick, in the exact order
  the client sends them (verified from the client's tick loop, not assumed);
- the server's view of the attacker's rotation at the moment it processes the
  attack packet, including what latency and jitter do to that ordering.

If the ordering or the knockback direction were simplified, the agent would
either miss the technique or learn one that does not work on a real server.

Verified against the 26.3 source:

- `Player.attack` → `causeExtraKnockback` pushes the victim with direction
  `(sin(yaw), -cos(yaw))` of the **attacker's server-side yaw**, strength
  `ATTACK_KNOCKBACK/2 (+ enchantments) + 0.5 if sprint-hit at full strength`.
  The base knockback is applied first by `dealDefaultKnockback`:
  `knockback(0.4F, attacker.x - victim.x, attacker.z - victim.z)`. Each
  `knockback` call halves the current horizontal velocity before subtracting
  its push, so a sprint hit on a standing victim nets about 0.7 away when
  facing them, and about 0.3 *toward* the attacker when facing away.
- The victim's client receives knockback as `ClientboundSetEntityMotionPacket`
  (compressed velocity encoding since 1.21.9); the simulator must reproduce
  that quantisation (`sim/include/mcp/lpvec3.hpp`, checked against the
  game's `LpVec3`). The client applies it with `lerpMotion`.
- The attacker's client also runs `Player.attack` locally, but `hurtClient`
  returns false there, so the attacker's own movement never gets the x0.6
  slowdown or the sprint reset. Only the server's copy of the attacker stops
  sprinting. The client reports sprint only when its own sprint state changes,
  so the server keeps treating the attacker as not sprinting until they
  release and re-press sprint: the mechanism behind W-tapping and sprint
  resets. (To confirm with the oracle: whether the server's sprint flag syncs
  back to the attacker's own client.)
- Hit invulnerability: a full hit sets `damageCooldownTime = 20` and
  `hurtTime = 10`. While `damageCooldownTime > 10`, a new hit only deals the
  damage above the previous hit's and applies **no knockback**.
- Client tick order (`Minecraft.tick`): `handleKeybinds` → `startAttack` sends
  `ServerboundAttackPacket` **before** `level.tickEntities` → `LocalPlayer.tick`
  → `sendPosition` sends that tick's rotation. So the server evaluates an
  attack with the rotation from the **previous** tick's movement packet.
- The client picks its target from the crosshair at the moment of the click,
  but that aim is never sent. The server only checks reach (eye to hitbox
  within 3.0 + 3.0 buffer, `AttackRange.isInRange`), not facing. The fairness
  policy below closes that gap by resolving every click with the client's pick.
- Result: face away at the end of tick N (rotation sent), flick back onto the
  target and click in tick N+1: the hit lands and the extra knockback uses the
  away-facing yaw, pulling the victim toward the attacker.
- A player victim's knockback is sent to their client as a velocity packet and
  the server restores its own copy; the victim only feels it after the
  server-to-client delay. The latency model must delay knockback accordingly.

### Input plausibility (fairness policy)

The bot may have inhuman *reaction time*, but its *inputs* must be ones a
human could physically produce. The goal is a bot that beats you by playing
well, not a killaura. Enforced in the simulator's action interface
(`sim/include/mcp/fairness.hpp`), and therefore in training, and again in the
server plugin:

- **Aim to hit, by construction.** The action is a *click*, not "attack
  player X". The duel resolves it exactly as the vanilla client does
  (`Minecraft.tick`: `pick(1.0F)`, then `handleKeybinds`), with a port of
  `LocalPlayer.pick` (`sim/include/mcp/pick.hpp`) that matches 4000
  vanilla-resolved picks bit for bit (`pick` test): the eye position, the
  view vector from the tick's rotation, blocks up to 4.5 blocks away and the
  opponent's hitbox within 3.0. An attack packet exists only if that ray hits
  the opponent before any block. The server alone would accept hits up to 6
  blocks from the eye in any direction.
- **Misses cost what they cost a human.** A click that finds nothing is a
  whiff: `missTime = 10` eats further clicks while the attack key stays held
  (releasing it clears the lockout), and every click that is not eaten sends
  `ServerboundPunchPacket`, which resets the server's attack strength. Clicking
  the ground starts and aborts mining a block and also punches. Holding the key
  on a block mines it; beyond the first tick that is out of scope (flagged).
- **Turn-rate cap.** The rotation change per tick is capped. Default: 200
  degrees per tick (about 4000 degrees per second, an elite human flick).
  Rotation only changes between ticks (the client picks, clicks, moves and
  sends its rotation inside one `Minecraft.tick`), so this is the whole
  budget: it keeps the "180 hit" possible while ruling out snapping away and
  back within a tick.
- **Click cap.** At most one attack click per tick (20 CPS), and the attack key
  can only be held if it was pressed this tick or held on the previous one.
- **Reaction time is unrestricted.** The bot may act on the newest
  information it has received, but never on information still in flight.

All caps are configuration values (`FairnessCaps` in `trainer/mcporl/config.py`
and `fairness.hpp`).

Known gap: a client sees a remote player where its interpolation puts it,
a few ticks behind the server. Until remote-player interpolation is ported,
the pick uses the opponent's latest server position, which is slightly more
generous than what a human sees. The oracle does the same (it has no client
level), so parity holds; the gap is against real clients, and is tracked with
client-side pushing, which needs the same interpolation.

### Contract vs runtime settings

Every setting belongs to one of two classes (`trainer/mcporl/config.py`):

- **Contract**: what the policy was trained against. Changing it requires
  retraining or fine-tuning. Includes the game version, enabled mechanic
  domains, observation/action schema versions, tick rate, the fairness caps,
  and the *training range* of every runtime knob. Stored in every checkpoint
  and identified by a fingerprint.
- **Runtime**: deploy-time knobs (latency, jitter, arena, difficulty delays)
  that may change freely, but only within the ranges the contract recorded.

A knob can have both halves: the latency value is runtime, the latency range
it was trained over is contract. `check_deployable` rejects a runtime value
outside its trained range, and reports any contract difference between a
checkpoint and the deployment as "retrain, do not reconfigure". The server
plugin runs the same check before loading a bot.

New settings default to the contract class. A setting only moves to runtime
once training randomises over it.

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
