# Training

The first milestone: a PPO agent learns to fight in the bit-exact simulator,
and its fights replay identically on the real 26.3 server.

## Pipeline

- `sim/include/mcp/env.hpp`: a batch of duels (auto-reset, fairness gate,
  reward, per-episode stats, recording) in a walled 49 x 49 arena;
  `libmcp_env` exposes it to Python (`trainer/mcporl/env.py`).
- `trainer/mcporl/ppo.py`: PPO with factored actions (keys and click
  categorical, the turn Gaussian), self-play mixed with scripted opponents.
- `trainer/scripts/train.py`: `python3 scripts/train.py --out runs/x`.
- `trainer/scripts/export_replays.py`: records episodes of a checkpoint and
  writes them to `oracle/replays/` (see "Replays").

### What the agent sees and does

Only what its client knows: its own state, the opponent as its client draws
it (the interpolated remote player, about a tick late), the opponent's held
item and hurt flash, and where the arena walls are. Never the opponent's
health or true position. Every tick it chooses movement keys, jump, sprint,
a click (a tap) and a turn; the fairness gate caps the turn at 200 degrees
per tick, and a click only hits if the vanilla crosshair pick says so.

### Opponents

Half of the duels are self-play; the rest are scripted: a standing dummy
(30%) or an aim bot with random aim noise (0.5 to 12 degrees) and turn rate
(6 to 60 degrees per tick) that walks in, sprints when far, and clicks at
full attack strength when a hit would land. The learner alternates between
player A and B, because the server handles A's packets first and so A wins
same-tick trades (in a mirror match of identical aim bots, A wins 93%).

Evaluation (every 10 updates, deterministic policy, one 30-second episode
per duel, the learner on both sides equally) uses fixed opponents: the dummy,
three aim bots (easy: 8 degrees noise, 12/tick; medium: 4, 25; hard: 1.5,
45), and a frozen copy of the policy from about 50 updates earlier.

## Result: run `sword8` (diamond swords)

500 updates, 24.6M agent-steps, about 15 minutes on 4 CPU cores. Checkpoint,
config and full metrics: `trainer/checkpoints/sword8*`.

| Update | vs dummy | vs easy bot | vs medium bot | vs hard bot | hits/click (hard) | aim error (hard) |
|---|---|---|---|---|---|---|
| 10 | 0% | 0% | 0% | 0% | 0.00 | 111 deg |
| 60 | 100% | 0% | 0% | 0% | 0.01 | 56 deg |
| 110 | 100% | 62% | 30% | 23% | 0.20 | 23 deg |
| 210 | 100% | 82% | 58% | 52% | 0.29 | 25 deg |
| 310 | 100% | 95% | 76% | 72% | 0.59 | 21 deg |
| 410 | 100% | 96% | 94% | 90% | 0.86 | 21 deg |
| 500 | 100% | 98% | 92% | 86% | 0.90 | 16 deg |

At the end, against the hard bot it deals 19.1 damage per fight and takes
7.2; it clicks 2.6 times per fight, almost only at full attack strength
(three full hits of a diamond sword kill). Early on it clicked ~377 times per
30-second episode, resetting its own attack strength with every click.

It first learned to kill a standing target (update ~50), then to win
trades against bots that fight back (~80 onwards), then to stop wasting
clicks (hits per click 0.3 to 0.9 after update 300, when the training aids
had faded and only damage and wins were paid).

Against its own snapshot from ~50 updates earlier it wins 100% at update 60
and 46% at update 500: self-play improvement has flattened. The next gains
need a league of past policies and harder opponents, and longer runs.

## Run `sword9`: the tactician, the expert panel, and a league

`sword8` had flattened against itself, and nothing it trained against
strafed. Run 9 continues from it with harder, more human-like opponents:

- **The tactician**: a port of the melee tactics of the public-domain Fabric
  mod PvP Bot (github.com/Stepan1411/PVP-bot-fabric): sprint in with bunny
  hops, strafe within 6 blocks, swing only at full strength then wait 10
  ticks, crit by dropping sprint and swinging on the way down, W-tap after
  each swing. It plays fair: turn cap, the lagged view, the crosshair pick.
  The mod itself fakes its crits (it sets `fallDistance`); a literal port
  never critted because it jumped while sprinting.
- **The expert panel**: the tactician with cheats, opponent-only, chosen so
  the learner's cues stay honest: *snap aim* (no turn cap), *true sight*
  (aims at the true position, not the lagged view), *range hit* (a swing
  lands whenever the true hitbox is in reach, without the crosshair pick;
  such episodes are marked non-replayable). The mod's grounded crits are left
  out: they would teach that a crit needs no jump. The learner never gets a
  cheat; the panel is ~10% of scripted duels and evaluated on its own line.
- **A league**: a share of duels against past snapshots of the learner
  (the starting checkpoint, then one every 20 updates).

Mix: 30% self-play; of the rest, 30% league, and of the scripted ones 60%
tactician, 15% expert, a few dummies, the rest aim bots. 200 updates (it
plateaued by ~60; stopped at 200), no training aids.

| Opponent (win rate) | `sword8` | `sword9` |
|---|---|---|
| hard aim bot | 91% | 91% |
| tactician, fair, crits | 85% | 91% |
| **tactician, fair, no crits** (strafe + W-tap: the hardest fair opponent) | **60%** | **82%** |
| expert: snap aim + true sight (crits) | 98% | 98% |
| expert: all cheats (crits) | 85% | 92% |
| expert: all cheats, no crits | 68% | ~72% |
| head to head vs `sword8` | | 48% (deterministic), 54% (sampled) |

The tactician's crit routine makes it *weaker*: the jump costs time the
learner punishes. Against its own snapshot from 40 updates earlier the
learner hovered around 40 to 50%: its style drifts between roughly equal
ones, while head to head against `sword8` it stays even, so the gains
against scripted opponents did not cost general strength.

Eight episodes of `sword9` (`t00` to `t07`: four against the fair
tactician, two of them with crits, and four self-play) matched vanilla on
every field of every tick, first time, adding strafing, W-taps and crits to
the replay coverage. Many of those crits (10.5 damage with a diamond sword)
are the learner's own: it opens fights with a falling hit, e.g. in `t00`,
where the tactician does not crit at all. All 22 replays are part of `ctest`.

## Hotbar and shields: does it learn to swap?

Hotbar switching and shields are ported bit for bit (oracle scenarios 40 to
51, `oracle/combat/README.md`). The policy gets a hotbar-key head (none,
slot 0, slot 1) and 16 more observations: the selected slot, what the first
three slots hold (damage, and whether it disables shields), its own shield
and cooldown, and the opponent's shield as its client sees it (raised, and for
how long) and whether the opponent holds an axe. Both experiments start from
`sword9`, grown to the new inputs and head (zero weights; the new head starts
at ~96% "no key"), so at update 0 the policy fights exactly like `sword9` and
never swaps. The reward is unchanged: damage and wins, nothing for swapping.

### Experiment A: wooden sword in hand, netherite sword in slot 1

The learner spawns holding a wooden sword (4.0 per hit) with a netherite
sword (8.0) in slot 1, against diamond swords (7.0): the tactician (60%),
aim bots, a few dummies, and 20% self-play.

| Update | vs tactician: win | swaps per fight | ticks holding the netherite | vs hard bot: win |
|---|---|---|---|---|
| 10 | 1% | 0 | 2% | 51% |
| 20 | 55% | 0.92 | 76% | 86% |
| 30 | 64% | 1.00 | 100% | 89% |
| 60 | 76% | 1.00 | 100% | 91% |
| 80 | 74% | 1.00 | 100% | 89% |

**Yes, within 20 updates (~0.8M agent-steps, 30 seconds).** It swaps
exactly once per fight, on its first tick (before the opponent is in reach,
so the attack-strength reset the swap causes costs nothing), and never swaps
back. Stuck with the wooden sword it won 1% against the tactician; with the
swap it is back to `sword9`'s level with a better sword.

### Experiment B: netherite sword in hand, wooden axe in slot 1, vs a shield

The opponent is a scripted shield user (`BatchEnv::shielder`, fair): it walks
in with a diamond sword and keeps its off-hand shield raised, lowers it
voluntarily at random (a per-tick chance), and swings at full strength while
it is down or disabled. Variants: never lowering it voluntarily, and panicking
(never lowering it again at or below a health threshold, which its player
sees). Before training, `sword9` with this loadout wins 20% against the
standard shield user (lowers ~every 33 ticks), 2% against a stubborn one
(every 200) and 0% (all draws) against one that never lowers it; it hits the
raised shield 3 to 18 times a fight and never touches the axe.

| Run | Opponents | What it learned |
|---|---|---|
| B1 | all lower at random (1 to 6% per tick) | **Wait, don't swap.** No axe; hits land while the shield is down (2.1 of 2.4 hits), clicks per fight halved; 20% to 84% wins vs the standard shield user within 40 updates. Against these opponents the axe is unnecessary, and it does not waste time on it. |
| B2 | 30% never lower, all panic at 0 to 10 health | **Actively suppresses the axe key**: P(slot 1) falls from 1.7% to 0.24% per tick; 100% draws vs the never-lowering user. |
| B3 | all never lower; hotbar head starts less certain | Random axe hits early (3.8 damage a fight), then un-learned (0.5). |
| B4 | half start **holding the axe**; 50% never lower, panic | Updates 20-30: disables the shield and **swaps back to the netherite** (0.4 swap-backs a fight, 12-15% wins where all else drew); then drops the axe on tick 1 instead (the Experiment A lesson). Updates 60-80: learns "shield up means axe" (axe swaps with the shield raised, 0 to 23 a fight) but dithers without hitting. |

Why the axe loses: a scripted check (`sword9`'s movement and aim, plus: shield
up -> axe, click; shield down -> netherite), 128 fights each:

| Shield user | plain: win / draw | combo: win / draw | combo damage dealt / taken |
|---|---|---|---|
| never lowers | 0% / 100% | 10% / 23% | 7.5 / 12.6 |
| never lowers, 5-tick reaction to the break | 0% / 100% | 22% / 23% | 9.2 / 11.5 |
| panics at 8 health | 3% / 40% | 4% / 20% | 9.3 / 17.3 |
| standard | 21% / 0% | 9% / 0% | 9.9 / 19.1 |

A tempo rule of 26.3 (all bit-exact, scenarios 43 to 45): the blocker stands
at full attack strength behind its shield; the disabling blow spends the
attacker's strength (every attack resets it) and each swap resets it again;
so the moment the shield drops the blocker has the first full-strength hit.
With netherite (8) against diamond (7) a lost first strike loses the trade.
Under a damage-and-win reward the learner's refusal to break the shield is
correct play against an opponent that counters at once; a draw costs it
nothing. The disable does not depend on attack strength (44), and a swap-hit
disables with the sword's attributes (45), which makes the break cheaper but
does not give back the first strike.

### What went wrong on the way (runs 1 to 7)

Each failure was the reward or the rules, not the code:

1. **Running away.** In an open arena an episode that timed out was a
   draw, and leaving the area forced one: the policy fled. Now the arena is
   walled with barrier blocks, as a real one would be.
2. **Mining to escape.** Holding the attack key on the ground pushed the duel
   outside the simulator's supported domain, which ended the episode as a
   draw. Clicks are now taps, and leaving the supported domain loses.
3. **Forfeits were cheaper than deaths.** Leaving the arena cost the loss
   bonus only, dying also cost the remaining health; now a forfeit is priced
   like a death.
4. **Shaping that could not teach.** Potential-based terms leave advantages
   unchanged, so they gave the learner almost no signal: an aim-only probe
   did not learn to aim. Dense per-tick aids (aim, and time in hit range)
   did (aim error 125 to 8 degrees in 40 updates); they are annealed to 0
   by 60% of training because they can be farmed (docs/REWARD.md).
5. **Blind to the walls.** The policy could not see the arena edge until it
   was added to the observation.

## Replays: does it transfer bit for bit?

`export_replays.py` records episodes of a checkpoint, skips any whose outcome
used a random draw vanilla makes from state we cannot reproduce (knockback
between players on the same spot), round-trips each through `duel_replay`,
and writes it to `oracle/replays/`. The oracle workflow replays them on the
real server (walled arena included) and `replay_*` tests require the
simulator to match every traced field of every tick.

Six episodes of the policy at update 120 (`r00` to `r05`) matched vanilla
on every field of every tick but one: on the killing tick, vanilla's dead
player had dropped its sword (`ServerPlayer.die` drops the inventory), which
resets the attack strength. The port now models it and all six match.
Eight episodes of the final policy (`f00` to `f07`: four against the hard
bot, four self-play) then matched vanilla on every field of every tick on the
first attempt. All 14 replays are part of `ctest`.

## Reproducing

```
cmake -S sim -B sim/build -DCMAKE_BUILD_TYPE=Release && cmake --build sim/build -j
pip install -r trainer/requirements.txt
cd trainer && python3 scripts/train.py --out runs/sword --updates 500
python3 scripts/export_replays.py runs/sword/checkpoint.pt --count 8
```
