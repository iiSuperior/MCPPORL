# Reward

The duel reward lives in `sim/include/mcp/reward.hpp`. Its weights are part of
the training contract (`RewardConfig` in `trainer/mcporl/config.py`): the
policy optimises exactly this, so changing a weight means retraining.

Per player, per tick:

```
r = damage_dealt * (health the opponent lost)
  - damage_taken * (health I lost)
  + win  on the tick the opponent dies,  - loss  on the tick I die
  + reach_shaping * (gamma * phi(s') - phi(s))
```

Defaults: 1, 1, 10, 10, 0.1, gamma 0.99.

## Reach: where the fight is actually won

A click lands only if the crosshair ray from the eye meets the opponent's
hitbox within 3.0 blocks. The shortest such ray goes to the nearest point of
the hitbox, so "eye to nearest point of the box is under 3.0" is exactly "a
perfectly aimed click would land" (`reach.hpp`).

Measuring from the *eye* (1.62 above the feet) to the *nearest point* of a
1.8-tall box is what makes height matter, which is Technoblade's low-ground
argument. The low player's eye is level with the high player's legs, so their
reach line is horizontal. The high player has to reach down, past their own
feet, to a head 0.82 (one block) or 1.82 (two blocks) below their eye, along
the hypotenuse. On flat blocks, by horizontal distance between the players:

| Height difference | Low player reaches until | High player reaches until | Only the low player can hit |
|---|---|---|---|
| 0 | 3.30 | 3.30 | never |
| 1 block | 3.30 | 3.19 | a 0.11-block band |
| 2 blocks | 3.28 | 2.69 | a 0.59-block band |

None of this is special-cased: it falls out of the geometry.

Two details make the metric honest:

- It is measured the way the game decides a click. My reach uses my eye
  against the opponent *as my client sees it*: the interpolated remote
  player, about a tick behind and quantised to 1/4096 (see
  docs/ARCHITECTURE.md, "How a client sees the other player"). Their reach
  uses their eye against their view of me. A sprinting opponent is seen about
  0.4 blocks behind where they are, so being in range of their true position
  is not enough (`30_lagged_whiff`).
- The potential is the reach *advantage*: phi = +1 when I can hit and they
  cannot, -1 for the reverse, 0 when both or neither can. Mutual range is
  neutral: it is where trades happen, and trades are paid by the damage
  terms.

## Why shaping, not "reward per tick in range"

"Time spent in hit range" is a very good signal: most won fights are won by
controlling range. As a raw per-tick reward, though, it is farmable. A policy
paid every tick it is in range learns to hover in range and never commit, or
to prefer staying in range over winning, and PPO will find that quickly.

Potential-based shaping (Ng, Harada and Russell, 1999) keeps the signal and
removes the exploit. The reach term is `gamma * phi(s') - phi(s)`:

- Stepping into an advantage pays about +0.1 once; letting the opponent into
  one costs about 0.1 once.
- *Holding* an advantage earns `(gamma - 1) * phi`, a tiny cost. Nothing
  accumulates by hovering.
- Over an episode the term telescopes to `-phi(start)` (phi is 0 at the
  terminal state), whatever the players did. So it cannot change which policy
  is optimal: winning and damage decide that. It only makes the right moves
  easier to discover early (`test_reward` checks the telescoping on full
  random fights).

This holds only if `gamma` in `RewardConfig` is the discount the learner
uses; the config keeps them in one place for that reason.

## Tuning notes

- `reach_shaping` is small (0.1 against 1 per health point and 10 per win) so
  that damage dominates once the policy starts landing hits. Annealing it
  toward 0 late in training is safe for the same telescoping reason.
- A smooth potential (for example the reach margin, clipped) would give a
  denser signal than the {-1, 0, +1} advantage. It is a one-line change in
  `reachPotential` and stays potential-based, so it is a candidate for
  ablation once training runs.
- Health lost counts damage taken from any source, which today is only the
  opponent's hits (fall damage and regeneration are outside the known
  domain).
