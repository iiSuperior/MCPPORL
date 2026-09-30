# Combat scenarios

Two-player scripts for the combat oracle. Same keys as `oracle/scenarios`,
plus the attack key. Each segment line gives a tick count, then player A's
inputs, `|`, player B's:

```
start A x=0.5 z=0.5 yaw=0        # optional placement per player
start B x=0.5 z=3.0 yaw=180
20 idle | idle                   # both idle 20 ticks (attack cooldown refills)
1  attack | idle                 # A clicks once
30 idle | idle                   # watch the knockback play out
```

Attack tokens (the key is released on ticks that have none of them):

- `attack`: a click, key still down when the tick samples it;
- `tap`: a click, key already released;
- `hold`: key down, no new click.

A click is resolved like the vanilla client: the crosshair pick decides
whether it attacks, whiffs or taps a block (see docs/ARCHITECTURE.md,
"Input plausibility").

`yaw=`/`pitch=` on a segment is the rotation during each of those ticks: it is
used by the pick, the movement and the rotation packet sent at the end of the
tick. The server evaluates an attack with the rotation sent on the previous
tick, which is what the 180-hit scenario exercises.

## Golden results (26.3)

Goldens live in `oracle/traces/combat/`. Knockback is B's velocity the tick
after the hit; "displacement" is B's z-change over that tick.

| Scenario | What it shows | Damage | B knockback |
|---|---|---|---|
| `10_hit_standing` | Plain hit | 1.0 | vz 0.2184 (0.4 before friction), vy 0.2752 |
| `11_sprint_hit` | Sprint hit adds extra knockback | 1.0 | 0.7 displacement, vy 0.3136 |
| `12_crit` | Falling hit at full attack strength; aimed 20 degrees down (a level crosshair passes over B's head) | **1.5** | same as a plain hit |
| `13_180_sprint_hit` | Extra knockback uses the attacker's server yaw: B pulled **toward** A | 1.0 | 0.3 toward A |
| `14_invulnerability` | Second hit 5 ticks later: no damage, no knockback | 1.0 + 0 | none |
| `15_wtap` | W-tap restores sprint: the second hit is a sprint hit again | 1.0 each | 0.7 both |
| `16_no_wtap` | Sprint held: the server copy stopped sprinting at the first hit | 1.0 each | 0.7, then 0.4 |
| `17_victim_walking` | Knockback uses the server copy's velocity, which has no walking component | 1.0 | as standing |
| `18_victim_sprinting` | Launched victim: the server runs `jumpFromGround` with the sprint boost on its copy | 1.0 | as standing |
| `19_victim_airborne` | In the air, vertical knockback keeps the server's vertical velocity (one tick stale) | 1.0 | vy 0.0831 |
| `20_trade` | Same-tick hits: A's packets first, B is still sprinting server-side | 1.0 each | both 0.7 |
| `21_partial_strength` | Attack strength 0.5: damage x(0.2 + 0.8 x 0.5^2), no sprint knockback | 0.4 | as standing |
| `22_release_after_hit` | The server echoes the removed sprint speed to A's client: A strafes at walk speed | 1.0 | 0.7 |

| `23_whiff` | Whiff, then a click every tick with the key held: all eaten for 9 ticks | 1.0 on the 10th | as standing |
| `24_look_away` | Click facing away: whiff; release, flick back, click: hits, but the whiff's punch halved the cooldown | 0.4 | as standing |
| `25_aim_edge` | 8 degrees off-centre misses by a centimetre, 5 degrees hits (one tick after the whiff's punch) | 0.27 | as standing |
| `26_ground_click` | Clicking the ground punches too: the next hit is at half strength | 0.4 | as standing |
| `27_kill` | Sprint hit on 1 health: the episode ends on the killing tick, knockback included | 1.0 | 0.7 |
| `28_trade_kill` | Both on 1 health, same-tick clicks: A's packets come first, B dies and B's hit is ignored | 1.0 (A survives) | 0.7 |
| `29_crit_kill` | A crit on exactly 1.5 health: health reaches 0.0 | 1.5 | as standing |
| `30_lagged_whiff` | B's hitbox is 2.66 from A's eye, but A sees B 0.42 further back (3.07): the click whiffs | none | none |
| `31_sword_hit` | Diamond sword: 1.0 + 6.0 attack damage (`item=diamond_sword` on a `start` line) | 7.0 | as standing |
| `32_axe_hit` | Diamond axe: 1.0 + 8.0, 20-tick cooldown | 9.0 | as standing |
| `33_sword_early_click` | Sword cooldown 12.5 ticks: a click into B's damage cooldown, then one at strength 0.44 | 7.0, 0, 2.48 | as standing |
| `34_sword_crit` | Iron sword crit: (1.0 + 5.0) x 1.5 | 9.0 | as standing |
| `35_sword_vs_axe_trade` | Netherite sword (8.0) vs stone axe (9.0), same tick, both sprint hits | 8.0 / 9.0 | 0.7 both |
| `36_wood_sword_combo` | Wooden sword 4.0 per hit: two full-strength hits kill B at 8 health | 4.0 + 4.0 | as standing |

### Hotbar and shields (40-51)

Tokens `slot=N` (a hotbar key, 0-8) and `use` (the use key is down; the first
such tick is a press); `start` keys `hotbar=a,b,...` (slot 0 is held, `-` for
an empty slot) and `offhand=`. Traces add `slot`, `using`, `cooldown` (the
client's), `server.slot/using/useTicks/blocking/cooldown/yHeadRot`,
`server.mainDamage/offDamage` (durability used) and `view.using` (the other
player's raised shield as this client sees it).

| Scenario | What it shows | Result |
|---|---|---|
| `40_swap_sword` | Wooden to netherite: the slot goes out with the next tick's `gameMode.tick`; the server resets the attack strength | 8.0 |
| `41_swap_hit_same_tick` | Slot key and click on one tick: `attack()` flushes the slot first, but the attributes (updated in the server tick) are still the sword's | **8.0 with an axe in hand** |
| `42_shield_raise` | A hit on the 4th use tick lands (the shield blocks after 5); later ones are blocked | 7.0, then blocked |
| `43_axe_disable` | Diamond axe on a raised shield: blocked, shield disabled for 100 ticks and lowered; the held use key retries every 4 ticks and succeeds when the cooldown ends | shield -10, then 9.0 |
| `44_axe_break_swap_back` | Wooden axe hit at strength ~0.1 still disables (strength does not matter); swap back, full netherite hit | disabled, then 8.0 |
| `45_swap_hit_disable` | Swap-hit to the axe: the sword's damage is blocked, the axe's Weapon component disables; reverse swap-hit uses the axe's attributes at low strength | shield -9, disabled, then 1.51 |
| `46_shield_arc` | 80 degrees from A: blocked; 100 degrees: lands | blocked, then 7.0 |
| `47_head_yaw_lag` | B turns away on the tick A's hit arrives: blocked (the head yaw updates in the server tick) | blocked, then 7.0 |
| `48_shield_walk` | Raised shield: input x0.2, no sprint start, a running sprint continues | |
| `49_block_then_cooldown` | A blocked hit still starts the damage cooldown (lastHurt 0): the follow-up deals its damage but no knockback | blocked, then 2.84 |
| `50_block_and_swap` | While blocking: clicks eaten, a hotbar swap leaves the off-hand shield up | blocked |
| `51_use_packets` | Right-click interaction and use-on-block packets change nothing | |

Scenarios can set `health=` on a `start` line; a scenario ends on the tick a
player dies. Overlapping players' server copies push each other (see 11, 13,
15, 16 once A catches up with B).

Every combat trace also records each tick's crosshair pick (type and hit
location), `missTime`, and each client's view of the other player
(`view.x/y/z/yRot`, the remote player's interpolated position, and
`view.recv`, the tracker packets delivered before the tick: 1 `Pos`,
2 `PosRot`, 4 `Rot`, 8 `PositionSync`). Picks test that view, so clicks on a
moving opponent land one to two ticks later than they would against its true
position; 17, 18, 20 and 28 click when the view is in reach. Natural health regeneration is off in the oracle
(it depends on hunger).

Two traps found while building these, both harness artifacts rather than
vanilla behaviour:

- Crits need `fallDistance > 0` on the server copy, and
  `Entity.doCheckFallDamage` does nothing while the player's hitbox (inflated
  by 1) touches a chunk that is not FULL-loaded. The harness force-loads the
  arena chunks and fails any scenario that traces such a state.
- Crits and the sprint extra knockback also need full attack strength
  (`getAttackStrengthScale(0.5) > 0.9`), traced as `server.attackStrength`.
  Scenarios idle 20 ticks before the first swing so the cooldown is full.
