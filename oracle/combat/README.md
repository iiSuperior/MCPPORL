# Combat scenarios

Two-player scripts for the combat oracle. Same keys as `oracle/scenarios`,
plus `attack` (one click on that tick, aimed at the other player). Each
segment line gives a tick count, then player A's inputs, `|`, player B's:

```
start A x=0.5 z=0.5 yaw=0        # optional placement per player
start B x=0.5 z=3.0 yaw=180
20 idle | idle                   # both idle 20 ticks (attack cooldown refills)
1  attack | idle                 # A clicks once
30 idle | idle                   # watch the knockback play out
```

`yaw=` on a segment is the rotation the client *sends* at the end of each of
those ticks. The server evaluates an attack with the rotation sent on the
previous tick, which is what the 180-hit scenario exercises.

## Golden results (26.3)

Goldens live in `oracle/traces/combat/`. Knockback is B's velocity the tick
after the hit; "displacement" is B's z-change over that tick.

| Scenario | What it shows | Damage | B knockback |
|---|---|---|---|
| `10_hit_standing` | Plain hit | 1.0 | vz 0.2184 (0.4 before friction), vy 0.2752 |
| `11_sprint_hit` | Sprint hit adds extra knockback | 1.0 | 0.7 displacement, vy 0.3136 |
| `12_crit` | Falling hit at full attack strength | **1.5** | same as a plain hit |
| `13_180_sprint_hit` | Extra knockback uses the attacker's server yaw: B pulled **toward** A | 1.0 | 0.3 toward A |
| `14_invulnerability` | Second hit 5 ticks later: no damage, no knockback | 1.0 + 0 | none |
| `15_wtap` | W-tap restores sprint: the second hit is a sprint hit again | 1.0 each | 0.7 both |
| `16_no_wtap` | Sprint held: the server copy stopped sprinting at the first hit | 1.0 each | 0.7, then 0.4 |

Two traps found while building these, both harness artifacts rather than
vanilla behaviour:

- Crits need `fallDistance > 0` on the server copy, and
  `Entity.doCheckFallDamage` does nothing while the player's hitbox (inflated
  by 1) touches a chunk that is not FULL-loaded. The harness force-loads the
  arena chunks and fails any scenario that traces such a state.
- Crits and the sprint extra knockback also need full attack strength
  (`getAttackStrengthScale(0.5) > 0.9`), traced as `server.attackStrength`.
  Scenarios idle 20 ticks before the first swing so the cooldown is full.
