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
Eight episodes of the final policy (`f00` to `f07`) are checked the same way.

## Reproducing

```
cmake -S sim -B sim/build -DCMAKE_BUILD_TYPE=Release && cmake --build sim/build -j
pip install -r trainer/requirements.txt
cd trainer && python3 scripts/train.py --out runs/sword --updates 500
python3 scripts/export_replays.py runs/sword/checkpoint.pt --count 8
```
